// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#include "fboss/fsdb/client/FsdbPubSubManager.h"

#include <folly/String.h>
#include <folly/logging/xlog.h>
#include <cstddef>
#include <string>
#include "FsdbPubSubManager.h"
#include "FsdbStreamClient.h"
#include "fboss/fsdb/client/FsdbDeltaPublisher.h"
#include "fboss/fsdb/client/FsdbPatchPublisher.h"
#include "fboss/fsdb/client/FsdbStatePublisher.h"

namespace {
using namespace facebook::fboss::fsdb;
auto constexpr kDelta = "delta";
auto constexpr kState = "state";
auto constexpr kStats = "stats";
auto constexpr kPath = "path";
auto constexpr kReconnectThread = "FsdbReconnectThread";
auto constexpr kSubscriberThread = "FsdbSubscriberThread";
auto constexpr kStatsPublisherThread = "FsdbStatsPublisherThread";
auto constexpr kStatePublisherThread = "FsdbStatePublisherThread";

std::string toSubscriptionStr(
    const std::string& fsdbHost,
    const std::vector<std::string>& path,
    bool isDelta,
    bool subscribeStats) {
  return folly::to<std::string>(
      fsdbHost,
      ":/",
      (isDelta ? kDelta : kPath),
      ":/",
      (subscribeStats ? kStats : kState),
      ":/",
      folly::join('/', path.begin(), path.end()));
}

std::string toSubscriptionStr(
    const std::string& fsdbHost,
    const std::vector<ExtendedOperPath>& paths,
    bool isDelta,
    bool subscribeStats) {
  return folly::to<std::string>(
      fsdbHost,
      ":/",
      (isDelta ? kDelta : kPath),
      ":/",
      (subscribeStats ? kStats : kState),
      ":/",
      extendedPathsStr(paths));
}

std::tuple<std::string, std::string, std::string, std::vector<std::string>>
parseSubscriptionStr(const std::string& subStr) {
  std::vector<std::string> elements;
  std::vector<std::string> paths;
  folly::split(":/", subStr, elements);
  // Server, Delta/Path, State/Stat, Paths
  CHECK_EQ(elements.size(), 4);
  const auto& pathStr = elements[elements.size() - 1];
  if (!pathStr.empty()) {
    folly::split('_', pathStr, paths);
  }
  return std::make_tuple(elements[0], elements[1], elements[2], paths);
}

std::vector<ExtendedOperPath> toExtendedOperPath(
    const std::vector<std::vector<std::string>>& paths) {
  std::vector<ExtendedOperPath> extPaths;
  for (const auto& path : paths) {
    ExtendedOperPath extPath;
    for (const auto& pathElm : path) {
      OperPathElem operPathElm;
      operPathElm.raw_ref() = pathElm;
      extPath.path()->push_back(std::move(operPathElm));
    }
    extPaths.push_back(std::move(extPath));
  }
  return extPaths;
}
} // namespace
namespace facebook::fboss::fsdb {

FsdbPubSubManager::FsdbPubSubManager(
    const std::string& clientId,
    folly::EventBase* reconnectEvb,
    folly::EventBase* subscriberEvb,
    folly::EventBase* statsPublisherEvb,
    folly::EventBase* statePublisherEvb)
    : clientId_(clientId),
      // if event base pointer is passed in, no need to spawn local thread
      reconnectEvbThread_(
          reconnectEvb ? nullptr
                       : std::make_unique<folly::ScopedEventBaseThread>(
                             kReconnectThread)),
      subscriberEvbThread_(
          subscriberEvb ? nullptr
                        : std::make_unique<folly::ScopedEventBaseThread>(
                              kSubscriberThread)),
      statsPublisherStreamEvbThread_(
          statsPublisherEvb ? nullptr
                            : std::make_unique<folly::ScopedEventBaseThread>(
                                  kStatsPublisherThread)),
      statePublisherStreamEvbThread_(
          statePublisherEvb ? nullptr
                            : std::make_unique<folly::ScopedEventBaseThread>(
                                  kStatePublisherThread)),
      // if local thread available, use local event base otherwise use passed in
      reconnectEvb_(
          reconnectEvbThread_ ? reconnectEvbThread_->getEventBase()
                              : reconnectEvb),
      subscriberEvb_(
          subscriberEvbThread_ ? subscriberEvbThread_->getEventBase()
                               : subscriberEvb),
      statsPublisherEvb_(
          statsPublisherStreamEvbThread_
              ? statsPublisherStreamEvbThread_->getEventBase()
              : statsPublisherEvb),
      statePublisherEvb_(
          statePublisherStreamEvbThread_
              ? statePublisherStreamEvbThread_->getEventBase()
              : statePublisherEvb) {}

FsdbPubSubManager::~FsdbPubSubManager() {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  stopPublisher(lk, std::move(stateDeltaPublisher_));
  stopPublisher(lk, std::move(statePathPublisher_));
  stopPublisher(lk, std::move(statDeltaPublisher_));
  stopPublisher(lk, std::move(statPathPublisher_));
  clearStateSubscriptions();
  clearStatSubscriptions();
}

void FsdbPubSubManager::stopPublisher(
    const std::lock_guard<std::mutex>& /*lk*/,
    std::unique_ptr<FsdbStreamClient> publisher) {
  if (publisher) {
    publisher->cancel();
  }
}

template <typename PublisherT>
std::unique_ptr<PublisherT> FsdbPubSubManager::createPublisherImpl(
    const std::lock_guard<std::mutex>& /*lk*/,
    const Path& publishPath,
    bool publishStats,
    FsdbStreamClient::FsdbStreamStateChangeCb publisherStateChangeCb,
    int32_t fsdbPort) const {
  auto publisherExists = publishStats
      ? (statDeltaPublisher_ || statPathPublisher_)
      : (stateDeltaPublisher_ || statePathPublisher_);

  if (publisherExists) {
    throw std::runtime_error(
        "Only one instance of delta or state publisher allowed");
  }
  auto& publisherEvb = publishStats ? statsPublisherEvb_ : statePublisherEvb_;
  auto publisher = std::make_unique<PublisherT>(
      clientId_,
      publishPath,
      publisherEvb,
      reconnectEvb_,
      publishStats,
      publisherStateChangeCb);
  publisher->setServerOptions(FsdbStreamClient::ServerOptions("::1", fsdbPort));
  return publisher;
}

void FsdbPubSubManager::createStateDeltaPublisher(
    const Path& publishPath,
    FsdbStreamClient::FsdbStreamStateChangeCb publisherStateChangeCb,
    int32_t fsdbPort) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  stateDeltaPublisher_ = createPublisherImpl<FsdbDeltaPublisher>(
      lk,
      publishPath,
      false /*subscribeStat*/,
      publisherStateChangeCb,
      fsdbPort);
}

void FsdbPubSubManager::createStatePathPublisher(
    const Path& publishPath,
    FsdbStreamClient::FsdbStreamStateChangeCb publisherStateChangeCb,
    int32_t fsdbPort) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  statePathPublisher_ = createPublisherImpl<FsdbStatePublisher>(
      lk,
      publishPath,
      false /*subscribeStat*/,
      publisherStateChangeCb,
      fsdbPort);
}

void FsdbPubSubManager::createStatePatchPublisher(
    const Path& publishPath,
    FsdbStreamClient::FsdbStreamStateChangeCb publisherStateChangeCb,
    int32_t fsdbPort) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  statePatchPublisher_ = createPublisherImpl<FsdbPatchPublisher>(
      lk,
      publishPath,
      false /*subscribeStat*/,
      publisherStateChangeCb,
      fsdbPort);
}

void FsdbPubSubManager::createStatDeltaPublisher(
    const Path& publishPath,
    FsdbStreamClient::FsdbStreamStateChangeCb publisherStateChangeCb,
    int32_t fsdbPort) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  statDeltaPublisher_ = createPublisherImpl<FsdbDeltaPublisher>(
      lk,
      publishPath,
      true /*subscribeStat*/,
      publisherStateChangeCb,
      fsdbPort);
}

void FsdbPubSubManager::createStatPathPublisher(
    const Path& publishPath,
    FsdbStreamClient::FsdbStreamStateChangeCb publisherStateChangeCb,
    int32_t fsdbPort) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  statPathPublisher_ = createPublisherImpl<FsdbStatePublisher>(
      lk,
      publishPath,
      true /*subscribeStat*/,
      publisherStateChangeCb,
      fsdbPort);
}

void FsdbPubSubManager::createStatPatchPublisher(
    const Path& publishPath,
    FsdbStreamClient::FsdbStreamStateChangeCb publisherStateChangeCb,
    int32_t fsdbPort) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  statPatchPublisher_ = createPublisherImpl<FsdbPatchPublisher>(
      lk,
      publishPath,
      true /*subscribeStat*/,
      publisherStateChangeCb,
      fsdbPort);
}

void FsdbPubSubManager::removeStateDeltaPublisher(bool gracefulRestart) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  if (gracefulRestart && stateDeltaPublisher_) {
    stateDeltaPublisher_->disconnectForGR();
  }
  stateDeltaPublisher_.reset();
}
void FsdbPubSubManager::removeStatePathPublisher(bool gracefulRestart) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  if (gracefulRestart && statePathPublisher_) {
    statePathPublisher_->disconnectForGR();
  }
  statePathPublisher_.reset();
}
void FsdbPubSubManager::removeStatePatchPublisher(bool gracefulRestart) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  if (gracefulRestart && statePatchPublisher_) {
    statePatchPublisher_->disconnectForGR();
  }
  statePatchPublisher_.reset();
}
void FsdbPubSubManager::removeStatDeltaPublisher(bool gracefulRestart) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  if (gracefulRestart && statDeltaPublisher_) {
    statDeltaPublisher_->disconnectForGR();
  }
  statDeltaPublisher_.reset();
}
void FsdbPubSubManager::removeStatPathPublisher(bool gracefulRestart) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  if (gracefulRestart && statPathPublisher_) {
    statPathPublisher_->disconnectForGR();
  }
  statPathPublisher_.reset();
}
void FsdbPubSubManager::removeStatPatchPublisher(bool gracefulRestart) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  if (gracefulRestart && statPatchPublisher_) {
    statPatchPublisher_->disconnectForGR();
  }
  statPatchPublisher_.reset();
}

template <typename PublisherT, typename PubUnitT>
void FsdbPubSubManager::publishImpl(PublisherT* publisher, PubUnitT&& pubUnit) {
  if (!publisher) {
    throw std::runtime_error("Publisher must be created before publishing");
  }
  publisher->write(std::forward<PubUnitT>(pubUnit));
}

void FsdbPubSubManager::publishState(OperDelta&& pubUnit) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  publishImpl(stateDeltaPublisher_.get(), std::move(pubUnit));
}

void FsdbPubSubManager::publishState(OperState&& pubUnit) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  publishImpl(statePathPublisher_.get(), std::move(pubUnit));
}

void FsdbPubSubManager::publishState(Patch&& pubUnit) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  publishImpl(statePatchPublisher_.get(), std::move(pubUnit));
}

void FsdbPubSubManager::publishStat(OperDelta&& pubUnit) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  publishImpl(statDeltaPublisher_.get(), std::move(pubUnit));
}

void FsdbPubSubManager::publishStat(OperState&& pubUnit) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  publishImpl(statPathPublisher_.get(), std::move(pubUnit));
}

void FsdbPubSubManager::publishStat(Patch&& pubUnit) {
  std::lock_guard<std::mutex> lk(publisherMutex_);
  publishImpl(statPatchPublisher_.get(), std::move(pubUnit));
}

void FsdbPubSubManager::addStatDeltaSubscription(
    const Path& subscribePath,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbDeltaSubscriber::FsdbOperDeltaUpdateCb operDeltaCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbDeltaSubscriber>(
      subscribePath,
      stateChangeCb,
      operDeltaCb,
      true /*subscribeStat*/,
      std::move(serverOptions));
}

void FsdbPubSubManager::addStatPathSubscription(
    const Path& subscribePath,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbStateSubscriber::FsdbOperStateUpdateCb operStateCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbStateSubscriber>(
      subscribePath,
      stateChangeCb,
      operStateCb,
      true /*subscribeStat*/,
      std::move(serverOptions));
}
/* multi path subscriptions */
void FsdbPubSubManager::addStateDeltaSubscription(
    const MultiPath& subscribePaths,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbExtDeltaSubscriber::FsdbOperDeltaUpdateCb operDeltaCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbExtDeltaSubscriber>(
      toExtendedOperPath(subscribePaths),
      stateChangeCb,
      operDeltaCb,
      false /*subscribeStat*/,
      std::move(serverOptions));
}

void FsdbPubSubManager::addStatPathSubscription(
    const MultiPath& subscribePaths,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbExtStateSubscriber::FsdbOperStateUpdateCb operStateCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbExtStateSubscriber>(
      toExtendedOperPath(subscribePaths),
      stateChangeCb,
      operStateCb,
      true /*subscribeStat*/,
      std::move(serverOptions));
}

void FsdbPubSubManager::addStateDeltaSubscription(
    const Path& subscribePath,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbDeltaSubscriber::FsdbOperDeltaUpdateCb operDeltaCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbDeltaSubscriber>(
      subscribePath,
      stateChangeCb,
      operDeltaCb,
      false /*subscribeStat*/,
      std::move(serverOptions));
}

void FsdbPubSubManager::addStatePathSubscription(
    const Path& subscribePath,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbStateSubscriber::FsdbOperStateUpdateCb operStateCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbStateSubscriber>(
      subscribePath,
      stateChangeCb,
      operStateCb,
      false /*subscribeStat*/,
      std::move(serverOptions));
}

void FsdbPubSubManager::addStatePathSubscription(
    const MultiPath& subscribePaths,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbExtStateSubscriber::FsdbOperStateUpdateCb operStateCb,
    FsdbStreamClient::ServerOptions&& serverOptions,
    const std::optional<std::string>& clientIdSuffix) {
  addSubscriptionImpl<FsdbExtStateSubscriber>(
      toExtendedOperPath(subscribePaths),
      stateChangeCb,
      operStateCb,
      false /*subscribeStat*/,
      std::move(serverOptions),
      clientIdSuffix);
}

void FsdbPubSubManager::addStatePathSubscription(
    SubscriptionOptions&& subscriptionOptions,
    const Path& subscribePath,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbStateSubscriber::FsdbOperStateUpdateCb operStateCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbStateSubscriber>(
      std::move(subscriptionOptions),
      subscribePath,
      stateChangeCb,
      operStateCb,
      std::move(serverOptions));
}

void FsdbPubSubManager::addStatePathSubscription(
    SubscriptionOptions&& subscriptionOptions,
    const MultiPath& subscribePaths,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbExtStateSubscriber::FsdbOperStateUpdateCb operStateCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbExtStateSubscriber>(
      std::move(subscriptionOptions),
      toExtendedOperPath(subscribePaths),
      stateChangeCb,
      operStateCb,
      std::move(serverOptions));
}

void FsdbPubSubManager::addStateExtPathSubscription(
    const std::vector<ExtendedOperPath>& subscribePaths,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbExtStateSubscriber::FsdbOperStateUpdateCb operStateCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbExtStateSubscriber>(
      subscribePaths,
      stateChangeCb,
      operStateCb,
      false /*subscribeStat*/,
      std::move(serverOptions));
}

void FsdbPubSubManager::addStatExtPathSubscription(
    const std::vector<ExtendedOperPath>& subscribePaths,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbExtStateSubscriber::FsdbOperStateUpdateCb operStateCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbExtStateSubscriber>(
      subscribePaths,
      stateChangeCb,
      operStateCb,
      true /*subscribeStat*/,
      std::move(serverOptions));
}

void FsdbPubSubManager::addStateExtDeltaSubscription(
    const std::vector<ExtendedOperPath>& subscribePaths,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbExtDeltaSubscriber::FsdbOperDeltaUpdateCb operDeltaCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbExtDeltaSubscriber>(
      subscribePaths,
      stateChangeCb,
      operDeltaCb,
      false /*subscribeStat*/,
      std::move(serverOptions));
}

void FsdbPubSubManager::addStatExtDeltaSubscription(
    const std::vector<ExtendedOperPath>& subscribePaths,
    SubscriptionStateChangeCb stateChangeCb,
    FsdbExtDeltaSubscriber::FsdbOperDeltaUpdateCb operDeltaCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  addSubscriptionImpl<FsdbExtDeltaSubscriber>(
      subscribePaths,
      stateChangeCb,
      operDeltaCb,
      true /*subscribeStat*/,
      std::move(serverOptions));
}

template <typename SubscriberT, typename PathElement>
void FsdbPubSubManager::addSubscriptionImpl(
    const std::vector<PathElement>& subscribePath,
    SubscriptionStateChangeCb stateChangeCb,
    typename SubscriberT::FsdbSubUnitUpdateCb subUnitAvailableCb,
    bool subscribeStats,
    FsdbStreamClient::ServerOptions&& serverOptions,
    const std::optional<std::string>& clientIdSuffix) {
  auto isDelta = std::disjunction_v<
      std::is_same<SubscriberT, FsdbDeltaSubscriber>,
      std::is_same<SubscriberT, FsdbExtDeltaSubscriber>>;
  auto subsStr = toSubscriptionStr(
      serverOptions.dstAddr.getAddressStr(),
      subscribePath,
      isDelta,
      subscribeStats);
  auto& path2Subscriber =
      subscribeStats ? statPath2Subscriber_ : statePath2Subscriber_;
  auto path2SubscriberW = path2Subscriber.wlock();

  auto clientStr = folly::to<std::string>(clientId_);
  if (clientIdSuffix.has_value()) {
    clientStr.append(folly::to<std::string>("_", clientIdSuffix.value()));
  }

  auto [itr, inserted] = path2SubscriberW->emplace(std::make_pair(
      subsStr,
      std::make_unique<SubscriberT>(
          clientStr,
          subscribePath,
          subscriberEvb_,
          reconnectEvb_,
          subUnitAvailableCb,
          subscribeStats,
          stateChangeCb)));
  if (!inserted) {
    throw std::runtime_error(
        "Subscription at : " + subsStr + " already exists");
  }
  XLOG(DBG2) << " Added subscription for: " << subsStr;
  itr->second->setServerOptions(std::move(serverOptions));
}

template <typename SubscriberT, typename PathElement>
void FsdbPubSubManager::addSubscriptionImpl(
    SubscriptionOptions&& subscriptionOptions,
    const std::vector<PathElement>& subscribePath,
    SubscriptionStateChangeCb stateChangeCb,
    typename SubscriberT::FsdbSubUnitUpdateCb subUnitAvailableCb,
    FsdbStreamClient::ServerOptions&& serverOptions) {
  auto isDelta = std::disjunction_v<
      std::is_same<SubscriberT, FsdbDeltaSubscriber>,
      std::is_same<SubscriberT, FsdbExtDeltaSubscriber>>;
  auto subsStr = toSubscriptionStr(
      serverOptions.dstAddr.getAddressStr(),
      subscribePath,
      isDelta,
      subscriptionOptions.subscribeStats_);
  auto& path2Subscriber = subscriptionOptions.subscribeStats_
      ? statPath2Subscriber_
      : statePath2Subscriber_;
  auto path2SubscriberW = path2Subscriber.wlock();

  auto [itr, inserted] = path2SubscriberW->emplace(std::make_pair(
      subsStr,
      std::make_unique<SubscriberT>(
          std::move(subscriptionOptions),
          subscribePath,
          subscriberEvb_,
          reconnectEvb_,
          subUnitAvailableCb,
          stateChangeCb)));
  if (!inserted) {
    throw std::runtime_error(
        "Subscription at : " + subsStr + " already exists");
  }
  XLOG(DBG2) << " Added subscription for: " << subsStr;
  itr->second->setServerOptions(std::move(serverOptions));
}

const std::vector<FsdbPubSubManager::SubscriptionInfo>
FsdbPubSubManager::getSubscriptionInfo() const {
  std::vector<SubscriptionInfo> subscriptionInfo;
  auto statePath2SubscriberR = statePath2Subscriber_.rlock();
  for (const auto& [subStr, streamClient] : *statePath2SubscriberR) {
    const auto& [server, delta, stats, paths] = parseSubscriptionStr(subStr);
    subscriptionInfo.push_back(
        {server,
         delta == kDelta,
         stats == kStats,
         paths,
         streamClient->getState(),
         streamClient->getDisconnectReason()});
  }
  auto statPath2SubscriberR = statPath2Subscriber_.rlock();
  for (const auto& [subStr, streamClient] : *statPath2SubscriberR) {
    const auto& [server, delta, stats, paths] = parseSubscriptionStr(subStr);
    subscriptionInfo.push_back(
        {server,
         delta == kDelta,
         stats == kStats,
         paths,
         streamClient->getState(),
         streamClient->getDisconnectReason()});
  }
  return subscriptionInfo;
}

std::string FsdbPubSubManager::subscriptionStateToString(
    FsdbStreamClient::State state) {
  return FsdbStreamClient::connectionStateToString(state);
}

void FsdbPubSubManager::removeStateDeltaSubscription(
    const Path& subscribePath,
    const std::string& fsdbHost) {
  removeSubscriptionImpl(
      subscribePath, fsdbHost, true /*delta*/, false /*subscribeStats*/);
}
void FsdbPubSubManager::removeStatePathSubscription(
    const Path& subscribePath,
    const std::string& fsdbHost) {
  removeSubscriptionImpl(
      subscribePath, fsdbHost, false /*delta*/, false /*subscribeStats*/);
}
void FsdbPubSubManager::removeStatDeltaSubscription(
    const Path& subscribePath,
    const std::string& fsdbHost) {
  removeSubscriptionImpl(
      subscribePath, fsdbHost, true /*delta*/, true /*subscribeStats*/);
}
void FsdbPubSubManager::removeStatPathSubscription(
    const Path& subscribePath,
    const std::string& fsdbHost) {
  removeSubscriptionImpl(
      subscribePath, fsdbHost, false /*delta*/, true /*subscribeStats*/);
}

void FsdbPubSubManager::removeStateDeltaSubscription(
    const MultiPath& subscribePath,
    const std::string& fsdbHost) {
  removeSubscriptionImpl(
      toExtendedOperPath(subscribePath),
      fsdbHost,
      true /*delta*/,
      false /*subscribeStats*/);
}
void FsdbPubSubManager::removeStatePathSubscription(
    const MultiPath& subscribePath,
    const std::string& fsdbHost) {
  removeSubscriptionImpl(
      toExtendedOperPath(subscribePath),
      fsdbHost,
      false /*delta*/,
      false /*subscribeStats*/);
}
void FsdbPubSubManager::removeStatDeltaSubscription(
    const MultiPath& subscribePath,
    const std::string& fsdbHost) {
  removeSubscriptionImpl(
      toExtendedOperPath(subscribePath),
      fsdbHost,
      true /*delta*/,
      true /*subscribeStats*/);
}
void FsdbPubSubManager::removeStatPathSubscription(
    const MultiPath& subscribePath,
    const std::string& fsdbHost) {
  removeSubscriptionImpl(
      toExtendedOperPath(subscribePath),
      fsdbHost,
      false /*delta*/,
      true /*subscribeStats*/);
}
void FsdbPubSubManager::removeStateExtDeltaSubscription(
    const std::vector<ExtendedOperPath>& subscribePaths,
    const std::string& fsdbHost) {
  removeSubscriptionImpl(
      subscribePaths, fsdbHost, true /*delta*/, false /*subscribeStats*/);
}
void FsdbPubSubManager::removeStatExtDeltaSubscription(
    const std::vector<ExtendedOperPath>& subscribePaths,
    const std::string& fsdbHost) {
  removeSubscriptionImpl(
      subscribePaths, fsdbHost, true /*delta*/, true /*subscribeStats*/);
}

void FsdbPubSubManager::clearStateSubscriptions() {
  statePath2Subscriber_.wlock()->clear();
}
void FsdbPubSubManager::clearStatSubscriptions() {
  statPath2Subscriber_.wlock()->clear();
}

template <typename PathElement>
void FsdbPubSubManager::removeSubscriptionImpl(
    const std::vector<PathElement>& subscribePath,
    const std::string& fsdbHost,
    bool isDelta,
    bool subscribeStats) {
  auto subsStr =
      toSubscriptionStr(fsdbHost, subscribePath, isDelta, subscribeStats);
  auto& path2Subscriber =
      subscribeStats ? statPath2Subscriber_ : statePath2Subscriber_;
  if (path2Subscriber.wlock()->erase(subsStr)) {
    XLOG(DBG2) << "Erased subscription for : " << subsStr;
  }
}

FsdbStreamClient::State FsdbPubSubManager::getStatePathSubsriptionState(
    const MultiPath& subscribePath,
    const std::string& fsdbHost) const {
  auto subsStr = toSubscriptionStr(
      fsdbHost, toExtendedOperPath(subscribePath), false, false);
  auto path2SubscriberR = statePath2Subscriber_.rlock();
  if (path2SubscriberR->find(subsStr) == path2SubscriberR->end()) {
    return FsdbStreamClient::State::CANCELLED;
  }
  return path2SubscriberR->at(subsStr)->getState();
}

} // namespace facebook::fboss::fsdb
