// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#include <folly/String.h>
#include <folly/Subprocess.h>
#include <gflags/gflags.h>

#include <thrift/lib/cpp2/protocol/DebugProtocol.h>
#include "fboss/agent/AgentConfig.h"
#include "fboss/agent/LldpManager.h"
#include "fboss/agent/SwSwitch.h"
#include "fboss/agent/hw/gen-cpp2/hardware_stats_types.h"
#include "fboss/agent/hw/test/LoadBalancerUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTestQosUtils.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/state/PortMap.h"
#include "fboss/agent/state/StateUtils.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/test/EcmpSetupHelper.h"
#include "fboss/agent/test/link_tests/LinkTest.h"
#include "fboss/agent/test/utils/CoppTestUtils.h"
#include "fboss/agent/test/utils/QosTestUtils.h"
#include "fboss/lib/CommonFileUtils.h"
#include "fboss/lib/CommonUtils.h"
#include "fboss/lib/config/PlatformConfigUtils.h"
#include "fboss/lib/phy/gen-cpp2/phy_types_custom_protocol.h"
#include "fboss/lib/thrift_service_client/ThriftServiceClient.h"
#include "fboss/qsfp_service/if/gen-cpp2/transceiver_types_custom_protocol.h"

DECLARE_bool(enable_macsec);

DECLARE_bool(skip_drain_check_for_prbs);

DEFINE_bool(
    link_stress_test,
    false,
    "enable to run stress tests (longer duration + more iterations)");

namespace {
const std::vector<std::string> kRestartQsfpService = {
    "/bin/systemctl",
    "restart",
    "qsfp_service_for_testing"};

const std::string kForceColdbootQsfpSvcFileName =
    "/dev/shm/fboss/qsfp_service/cold_boot_once_qsfp_service";

// This file stores all non-validated transceiver configurations found while
// running getAllTransceiverConfigValidationStatuses() in EmptyLinkTest. Each
// configuration will be in JSON format. This file is then downloaded by the
// Netcastle test runner to parse it and log these configurations to Scuba.
// Matching file definition is located in
// fbcode/neteng/netcastle/teams/fboss/constants.py.
const std::string kTransceiverConfigJsonsForScuba =
    "/tmp/transceiver_config_jsons_for_scuba.log";
} // namespace

namespace facebook::fboss {

void LinkTest::SetUp() {
  AgentTest::SetUp();
  initializeCabledPorts();
  // Wait for all the cabled ports to link up before finishing the setup
  // TODO(joseph5wu) Temporarily increase the timeout to 3mins because current
  // TransceiverStateMachine can only allow handle updates sequentially and for
  // Minipack xphy programming, it will take about 68s to program all 128 ports.
  // Will lower the timeout back to 1min once we can support parallel
  // programming
  waitForAllCabledPorts(true, 60, 5s);
  waitForAllTransceiverStates(true, 60, 5s);
  XLOG(DBG2) << "Link Test setup ready";
}

void LinkTest::restartQsfpService(bool coldboot) const {
  if (coldboot) {
    createFile(kForceColdbootQsfpSvcFileName);
    XLOG(DBG2) << "Restarting QSFP Service in coldboot mode";
  } else {
    XLOG(DBG2) << "Restarting QSFP Service in warmboot mode";
  }

  folly::Subprocess(kRestartQsfpService).waitChecked();
}

void LinkTest::TearDown() {
  // Expect the qsfp service to be running at the end of the tests
  auto qsfpServiceClient = utils::createQsfpServiceClient();
  EXPECT_EQ(
      facebook::fb303::cpp2::fb_status::ALIVE,
      qsfpServiceClient.get()->sync_getStatus())
      << "QSFP Service no longer alive after the test";
  EXPECT_EQ(
      QsfpServiceRunState::ACTIVE,
      qsfpServiceClient.get()->sync_getQsfpServiceRunState())
      << "QSFP Service run state no longer active after the test";
  AgentTest::TearDown();
}

void LinkTest::setCmdLineFlagOverrides() const {
  FLAGS_enable_macsec = true;
  FLAGS_skip_drain_check_for_prbs = true;
  AgentTest::setCmdLineFlagOverrides();
}

void LinkTest::overrideL2LearningConfig(bool swLearning, int ageTimer) {
  auto agentConfig = AgentConfig::fromFile(FLAGS_config);
  cfg::AgentConfig testConfig = agentConfig->thrift;
  if (swLearning) {
    testConfig.sw()->switchSettings()->l2LearningMode() =
        cfg::L2LearningMode::SOFTWARE;
  } else {
    testConfig.sw()->switchSettings()->l2LearningMode() =
        cfg::L2LearningMode::HARDWARE;
  }
  testConfig.sw()->switchSettings()->l2AgeTimerSeconds() = ageTimer;
  auto newAgentConfig = AgentConfig(
      testConfig,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(testConfig));
  newAgentConfig.dumpConfig(getTestConfigPath());
  FLAGS_config = getTestConfigPath();
  platform()->reloadConfig();
}

void LinkTest::setupTtl0ForwardingEnable() {
  if (!sw()->getHwAsicTable()->isFeatureSupportedOnAnyAsic(
          HwAsic::Feature::SAI_TTL0_PACKET_FORWARD_ENABLE)) {
    // don't configure if not supported
    return;
  }
  auto agentConfig = AgentConfig::fromFile(FLAGS_config);
  auto newAgentConfig =
      utility::setTTL0PacketForwardingEnableConfig(sw(), *agentConfig);
  newAgentConfig.dumpConfig(getTestConfigPath());
  FLAGS_config = getTestConfigPath();
  platform()->reloadConfig();
}

// Waits till the link status of the ports in cabledPorts vector reaches
// the expected state
void LinkTest::waitForAllCabledPorts(
    bool up,
    uint32_t retries,
    std::chrono::duration<uint32_t, std::milli> msBetweenRetry) const {
  waitForLinkStatus(getCabledPorts(), up, retries, msBetweenRetry);
}

void LinkTest::waitForAllTransceiverStates(
    bool up,
    uint32_t retries,
    std::chrono::duration<uint32_t, std::milli> msBetweenRetry) const {
  waitForStateMachineState(
      cabledTransceivers_,
      up ? TransceiverStateMachineState::ACTIVE
         : TransceiverStateMachineState::INACTIVE,
      retries,
      msBetweenRetry);
}

// Retrieves the config validation status for all active transceivers. Strings
// are retrieved instead of booleans because these contain the stringified JSONs
// of each non-validated transceiver config, which will be printed to stdout and
// ingested by Netcastle.
void LinkTest::getAllTransceiverConfigValidationStatuses() {
  std::vector<int32_t> expectedTransceivers(
      cabledTransceivers_.begin(), cabledTransceivers_.end());
  std::map<int32_t, std::string> responses;

  try {
    auto qsfpServiceClient = utils::createQsfpServiceClient();
    qsfpServiceClient->sync_getTransceiverConfigValidationInfo(
        responses, expectedTransceivers, true);
  } catch (const std::exception& ex) {
    XLOG(WARN)
        << "Failed to call qsfp_service getTransceiverConfigValidationInfo(). "
        << folly::exceptionStr(ex);
  }

  createFile(kTransceiverConfigJsonsForScuba);

  std::vector<int32_t> missingTransceivers, invalidTransceivers;
  std::vector<std::string> nonValidatedConfigs;
  for (auto transceiverID : expectedTransceivers) {
    if (auto transceiverStatusIt = responses.find(transceiverID);
        transceiverStatusIt != responses.end()) {
      if (transceiverStatusIt->second != "") {
        invalidTransceivers.push_back(transceiverID);
        nonValidatedConfigs.push_back(transceiverStatusIt->second);
      }
      continue;
    }
    missingTransceivers.push_back(transceiverID);
  }
  if (nonValidatedConfigs.size()) {
    writeSysfs(
        kTransceiverConfigJsonsForScuba,
        folly::join("\n", nonValidatedConfigs));
  }
  if (!missingTransceivers.empty()) {
    XLOG(DBG2) << "Transceivers [" << folly::join(",", missingTransceivers)
               << "] are missing config validation status.";
  }
  if (!invalidTransceivers.empty()) {
    XLOG(DBG2) << "Transceivers [" << folly::join(",", invalidTransceivers)
               << "] have non-validated configurations.";
  }
  if (missingTransceivers.empty() && invalidTransceivers.empty()) {
    XLOG(DBG2) << "Transceivers [" << folly::join(",", expectedTransceivers)
               << "] all have validated configurations.";
  }
}

// Wait until we have successfully fetched transceiver info (and thus know
// which transceivers are available for testing)
std::map<int32_t, TransceiverInfo> LinkTest::waitForTransceiverInfo(
    std::vector<int32_t> transceiverIds,
    uint32_t retries,
    std::chrono::duration<uint32_t, std::milli> msBetweenRetry) const {
  std::map<int32_t, TransceiverInfo> info;
  while (retries--) {
    try {
      auto qsfpServiceClient = utils::createQsfpServiceClient();
      qsfpServiceClient->sync_getTransceiverInfo(info, transceiverIds);
    } catch (const std::exception& ex) {
      XLOG(WARN) << "Failed to call qsfp_service getTransceiverInfo(). "
                 << folly::exceptionStr(ex);
    }
    // Make sure we have at least one present transceiver
    for (const auto& it : info) {
      if (*it.second.tcvrState()->present()) {
        return info;
      }
    }
    XLOG(DBG2) << "TransceiverInfo was empty";
    if (retries) {
      /* sleep override */
      std::this_thread::sleep_for(msBetweenRetry);
    }
  }

  throw FbossError("TransceiverInfo was never populated.");
}

// Initializes the vector that holds the ports that are expected to be cabled.
// If the expectedLLDPValues in the switch config has an entry, we expect
// that port to take part in the test
void LinkTest::initializeCabledPorts() {
  const auto& platformPorts = sw()->getPlatformMapping()->getPlatformPorts();

  auto swConfig = sw()->getConfig();
  const auto& chips = sw()->getPlatformMapping()->getChips();
  for (const auto& port : *swConfig.ports()) {
    if (!(*port.expectedLLDPValues()).empty()) {
      auto portID = *port.logicalID();
      cabledPorts_.push_back(PortID(portID));
      if (*port.portType() == cfg::PortType::FABRIC_PORT) {
        cabledFabricPorts_.push_back(PortID(portID));
      }
      const auto platformPortEntry = platformPorts.find(portID);
      EXPECT_TRUE(platformPortEntry != platformPorts.end())
          << "Can't find port:" << portID << " in PlatformMapping";
      auto transceiverID =
          utility::getTransceiverId(platformPortEntry->second, chips);
      if (transceiverID.has_value()) {
        cabledTransceivers_.insert(*transceiverID);
      }
    }
  }
}

std::tuple<std::vector<PortID>, std::string>
LinkTest::getOpticalCabledPortsAndNames(bool pluggableOnly) const {
  std::string opticalPortNames;
  std::vector<PortID> opticalPorts;
  std::vector<int32_t> transceiverIds;
  for (const auto& port : getCabledPorts()) {
    auto portName = getPortName(port);
    auto tcvrId = platform()->getPlatformPort(port)->getTransceiverID().value();
    transceiverIds.push_back(tcvrId);
  }

  auto transceiverInfos = waitForTransceiverInfo(transceiverIds);
  for (const auto& port : getCabledPorts()) {
    auto portName = getPortName(port);
    auto tcvrId = platform()->getPlatformPort(port)->getTransceiverID().value();
    auto tcvrInfo = transceiverInfos.find(tcvrId);

    if (tcvrInfo != transceiverInfos.end()) {
      auto tcvrState = *tcvrInfo->second.tcvrState();
      if (TransmitterTechnology::OPTICAL ==
          tcvrState.cable().value_or({}).transmitterTech()) {
        if (!pluggableOnly ||
            (pluggableOnly &&
             (tcvrState.identifier().value_or({}) !=
              TransceiverModuleIdentifier::MINIPHOTON_OBO))) {
          opticalPorts.push_back(port);
          opticalPortNames += portName + " ";
        } else {
          XLOG(DBG2) << "Transceiver: " << tcvrId + 1 << ", " << portName
                     << ", is on-board optics, skip it";
        }
      } else {
        XLOG(DBG2) << "Transceiver: " << tcvrId + 1 << ", " << portName
                   << ", is not optics, skip it";
      }
    } else {
      XLOG(DBG2) << "TransceiverInfo of transceiver: " << tcvrId + 1 << ", "
                 << portName << ", is not present, skip it";
    }
  }

  return {opticalPorts, opticalPortNames};
}

const std::vector<PortID>& LinkTest::getCabledPorts() const {
  return cabledPorts_;
}

boost::container::flat_set<PortDescriptor>
LinkTest::getSingleVlanOrRoutedCabledPorts() const {
  boost::container::flat_set<PortDescriptor> ecmpPorts;
  auto singleVlanOrRoutedPorts =
      utility::getSingleVlanOrRoutedCabledPorts(sw());
  for (auto port : getCabledPorts()) {
    if (singleVlanOrRoutedPorts.find(PortDescriptor(port)) !=
        singleVlanOrRoutedPorts.end()) {
      ecmpPorts.insert(PortDescriptor(port));
    }
  }
  return ecmpPorts;
}

void LinkTest::programDefaultRoute(
    const boost::container::flat_set<PortDescriptor>& ecmpPorts,
    utility::EcmpSetupTargetedPorts6& ecmp6) {
  ASSERT_GT(ecmpPorts.size(), 0);
  sw()->updateStateBlocking("Resolve nhops", [ecmpPorts, &ecmp6](auto state) {
    return ecmp6.resolveNextHops(state, ecmpPorts);
  });
  ecmp6.programRoutes(
      std::make_unique<SwSwitchRouteUpdateWrapper>(sw()->getRouteUpdater()),
      ecmpPorts);
}

void LinkTest::programDefaultRoute(
    const boost::container::flat_set<PortDescriptor>& ecmpPorts,
    std::optional<folly::MacAddress> dstMac) {
  utility::EcmpSetupTargetedPorts6 ecmp6(sw()->getState(), dstMac);
  programDefaultRoute(ecmpPorts, ecmp6);
}

void LinkTest::disableTTLDecrements(
    const boost::container::flat_set<PortDescriptor>& ecmpPorts) {
  std::set<SwitchID> switchIds;
  for (auto port : ecmpPorts) {
    auto switchId =
        sw()->getScopeResolver()->scope(port.phyPortID()).switchId();
    if (sw()->getHwAsicTable()->isFeatureSupported(
            switchId, HwAsic::Feature::SAI_TTL0_PACKET_FORWARD_ENABLE)) {
    } else if (sw()->getHwAsicTable()->isFeatureSupported(
                   switchId, HwAsic::Feature::PORT_TTL_DECREMENT_DISABLE)) {
      disableTTLDecrementOnPorts({port});
    } else if (sw()->getHwAsicTable()->isFeatureSupported(
                   switchId, HwAsic::Feature::NEXTHOP_TTL_DECREMENT_DISABLE)) {
      utility::EcmpSetupTargetedPorts6 ecmp6(sw()->getState());
      utility::disableTTLDecrements(
          sw(), ecmp6.getRouterId(), ecmp6.nhop(port));
    } else {
      throw FbossError("Failed to configure TTL decrement");
    }
  }
}

void LinkTest::createL3DataplaneFlood(
    const boost::container::flat_set<PortDescriptor>& ecmpPorts) {
  auto switchId = scope(ecmpPorts);
  utility::EcmpSetupTargetedPorts6 ecmp6(
      sw()->getState(), sw()->getLocalMac(switchId));
  programDefaultRoute(ecmpPorts, ecmp6);
  disableTTLDecrements(ecmpPorts);
  auto vlanID = utility::firstVlanID(sw()->getState());
  utility::pumpTraffic(
      true,
      utility::getAllocatePktFn(sw()),
      utility::getSendPktFunc(sw()),
      sw()->getLocalMac(switchId),
      vlanID);
  // TODO: Assert that traffic reached a certain rate
  XLOG(DBG2) << "Created L3 Data Plane Flood";
}

bool LinkTest::checkReachabilityOnAllCabledPorts() const {
  auto lldpDb = sw()->getLldpMgr()->getDB();
  for (const auto& port : getCabledPorts()) {
    auto portType = platform()->getPlatformPort(port)->getPortType();
    if (portType == cfg::PortType::INTERFACE_PORT &&
        !lldpDb->getNeighbors(port).size()) {
      XLOG(DBG2) << " No lldp neighbors on : " << getPortName(port);
      return false;
    }
    if (portType == cfg::PortType::FABRIC_PORT) {
      auto fabricReachabilityEntries =
          platform()->getHwSwitch()->getFabricConnectivity();
      auto fabricPortEndPoint = fabricReachabilityEntries.find(port);
      if (fabricPortEndPoint == fabricReachabilityEntries.end() ||
          !*fabricPortEndPoint->second.isAttached()) {
        XLOG(DBG2) << " No fabric end points on : " << getPortName(port);
        return false;
      }
    }
  }
  return true;
}

std::string LinkTest::getPortName(PortID portId) const {
  for (auto portMap : std::as_const(*sw()->getState()->getPorts())) {
    for (auto port : std::as_const(*portMap.second)) {
      if (port.second->getID() == portId) {
        return port.second->getName();
      }
    }
  }
  throw FbossError("No port with ID: ", portId);
}

std::vector<std::string> LinkTest::getPortName(
    const std::vector<PortID>& portIDs) const {
  std::vector<std::string> portNames;
  for (auto port : portIDs) {
    portNames.push_back(getPortName(port));
  }
  return portNames;
}

std::optional<PortID> LinkTest::getPeerPortID(PortID portId) const {
  for (auto portPair : getConnectedPairs()) {
    if (portPair.first == portId) {
      return portPair.second;
    } else if (portPair.second == portId) {
      return portPair.first;
    }
  }
  return std::nullopt;
}

std::set<std::pair<PortID, PortID>> LinkTest::getConnectedPairs() const {
  waitForLldpOnCabledPorts();
  std::set<std::pair<PortID, PortID>> connectedPairs;
  for (auto cabledPort : cabledPorts_) {
    PortID neighborPort;
    auto portType = platform()->getPlatformPort(cabledPort)->getPortType();
    if (portType == cfg::PortType::FABRIC_PORT) {
      auto fabricReachabilityEntries =
          platform()->getHwSwitch()->getFabricConnectivity();
      auto fabricPortEndpoint = fabricReachabilityEntries.find(cabledPort);
      if (fabricPortEndpoint == fabricReachabilityEntries.end() ||
          !*fabricPortEndpoint->second.isAttached()) {
        XLOG(DBG2) << " No fabric end points on : " << getPortName(cabledPort);
        continue;
      }
      neighborPort = PortID(fabricPortEndpoint->second.get_portId()) +
          getRemotePortOffset(platform()->getType());
    } else {
      auto lldpNeighbors =
          sw()->getLldpMgr()->getDB()->getNeighbors(cabledPort);
      if (lldpNeighbors.size() != 1) {
        XLOG(WARN) << "Wrong lldp neighbor size for port "
                   << getPortName(cabledPort) << ", should be 1 but got "
                   << lldpNeighbors.size();
        continue;
      }
      neighborPort = getPortID((*lldpNeighbors.begin())->getPortId());
    }
    // Insert sorted pairs, so that the same pair does not show up twice in
    // the set
    auto connectedPair = cabledPort < neighborPort
        ? std::make_pair(cabledPort, neighborPort)
        : std::make_pair(neighborPort, cabledPort);
    connectedPairs.insert(connectedPair);
  }
  return connectedPairs;
}

/*
 * getConnectedOpticalPortPairWithFeature
 *
 * Returns the set of connected port pairs with optical link and the optics
 * supporting the given feature. For feature==None, this will return set of
 * connected port pairs using optical links
 */
std::set<std::pair<PortID, PortID>>
LinkTest::getConnectedOpticalPortPairWithFeature(
    TransceiverFeature feature,
    phy::Side side) const {
  auto connectedPairs = getConnectedPairs();
  auto opticalPorts = std::get<0>(getOpticalCabledPortsAndNames(false));

  std::set<std::pair<PortID, PortID>> connectedOpticalPortPairs;
  for (auto connectedPair : connectedPairs) {
    if (std::find(
            opticalPorts.begin(), opticalPorts.end(), connectedPair.first) !=
        opticalPorts.end()) {
      connectedOpticalPortPairs.insert(connectedPair);
    }
  }

  if (feature == TransceiverFeature::NONE) {
    return connectedOpticalPortPairs;
  }

  std::vector<int32_t> transceiverIds;
  for (auto portPair : connectedOpticalPortPairs) {
    auto tcvrId =
        platform()->getPlatformPort(portPair.first)->getTransceiverID().value();
    transceiverIds.push_back(tcvrId);
    tcvrId = platform()
                 ->getPlatformPort(portPair.second)
                 ->getTransceiverID()
                 .value();
    transceiverIds.push_back(tcvrId);
  }

  auto transceiverInfos = waitForTransceiverInfo(transceiverIds);
  std::set<std::pair<PortID, PortID>> connectedOpticalFeaturedPorts;
  for (auto portPair : connectedOpticalPortPairs) {
    auto tcvrId =
        platform()->getPlatformPort(portPair.first)->getTransceiverID().value();

    auto tcvrInfo = transceiverInfos.find(tcvrId);
    if (tcvrInfo != transceiverInfos.end()) {
      auto tcvrState = *tcvrInfo->second.tcvrState();
      if (tcvrState.diagCapability().value().diagnostics().value()) {
        if (feature == TransceiverFeature::TX_DISABLE &&
            side == phy::Side::LINE) {
          bool txDisCapable =
              tcvrState.diagCapability().value().txOutputControl().value();
          if (txDisCapable) {
            connectedOpticalFeaturedPorts.insert(portPair);
          }
        } else if (
            feature == TransceiverFeature::TX_DISABLE &&
            side == phy::Side::SYSTEM) {
          bool rxDisCapable =
              tcvrState.diagCapability().value().rxOutputControl().value();
          if (rxDisCapable) {
            connectedOpticalFeaturedPorts.insert(portPair);
          }
        } else if (
            feature == TransceiverFeature::LOOPBACK &&
            side == phy::Side::LINE) {
          bool lineLoopCapable =
              tcvrState.diagCapability().value().loopbackLine().value();
          if (lineLoopCapable) {
            connectedOpticalFeaturedPorts.insert(portPair);
          }
        } else if (
            feature == TransceiverFeature::LOOPBACK &&
            side == phy::Side::SYSTEM) {
          bool sysLoopCapable =
              tcvrState.diagCapability().value().loopbackSystem().value();
          if (sysLoopCapable) {
            connectedOpticalFeaturedPorts.insert(portPair);
          }
        } else if (feature == TransceiverFeature::VDM) {
          bool vdmCapable = tcvrState.diagCapability().value().vdm().value();
          if (vdmCapable) {
            connectedOpticalFeaturedPorts.insert(portPair);
          }
        }
      }
    }
  }

  return connectedOpticalFeaturedPorts;
}

void LinkTest::waitForStateMachineState(
    const std::set<TransceiverID>& transceiversToCheck,
    TransceiverStateMachineState stateMachineState,
    uint32_t retries,
    std::chrono::duration<uint32_t, std::milli> msBetweenRetry) const {
  XLOG(DBG2) << "Checking qsfp TransceiverStateMachineState on "
             << folly::join(",", transceiversToCheck);

  std::vector<int32_t> expectedTransceiver;
  for (auto transceiverID : transceiversToCheck) {
    expectedTransceiver.push_back(transceiverID);
  }

  std::vector<int32_t> badTransceivers;
  while (retries--) {
    badTransceivers.clear();
    std::map<int32_t, TransceiverInfo> info;
    try {
      auto qsfpServiceClient = utils::createQsfpServiceClient();
      qsfpServiceClient->sync_getTransceiverInfo(info, expectedTransceiver);
    } catch (const std::exception& ex) {
      // We have retry mechanism to handle failure. No crash here
      XLOG(WARN) << "Failed to call qsfp_service getTransceiverInfo(). "
                 << folly::exceptionStr(ex);
    }
    // Check whether all expected transceivers have expected state
    for (auto transceiverID : expectedTransceiver) {
      // Only continue if the transceiver state machine matches
      if (auto transceiverInfoIt = info.find(transceiverID);
          transceiverInfoIt != info.end()) {
        if (auto state =
                transceiverInfoIt->second.tcvrState()->stateMachineState();
            state.has_value() && *state == stateMachineState) {
          continue;
        }
      }
      // Otherwise such transceiver is considered to be in a bad state
      badTransceivers.push_back(transceiverID);
    }

    if (badTransceivers.empty()) {
      XLOG(DBG2) << "All qsfp TransceiverStateMachineState on "
                 << folly::join(",", expectedTransceiver) << " match "
                 << apache::thrift::util::enumNameSafe(stateMachineState);
      return;
    } else {
      /* sleep override */
      std::this_thread::sleep_for(msBetweenRetry);
    }
  }

  throw FbossError(
      "Transceivers:[",
      folly::join(",", badTransceivers),
      "] don't have expected TransceiverStateMachineState:",
      apache::thrift::util::enumNameSafe(stateMachineState));
}

void LinkTest::waitForLldpOnCabledPorts(
    uint32_t retries,
    std::chrono::duration<uint32_t, std::milli> msBetweenRetry) const {
  WITH_RETRIES_N_TIMED(retries, msBetweenRetry, {
    ASSERT_EVENTUALLY_TRUE(checkReachabilityOnAllCabledPorts());
  });
}

// Log debug information from IPHY, XPHY and optics
void LinkTest::logLinkDbgMessage(std::vector<PortID>& portIDs) const {
  auto iPhyInfos = sw()->getIPhyInfo(portIDs);
  auto qsfpServiceClient = utils::createQsfpServiceClient();
  auto portNames = getPortNames(portIDs);
  std::map<std::string, phy::PhyInfo> xPhyInfos;
  std::map<int32_t, TransceiverInfo> tcvrInfos;

  try {
    qsfpServiceClient->sync_getInterfacePhyInfo(xPhyInfos, portNames);
  } catch (const std::exception& ex) {
    XLOG(WARN) << "Failed to call qsfp_service getInterfacePhyInfo(). "
               << folly::exceptionStr(ex);
  }

  std::vector<int32_t> tcvrIds;
  for (auto portID : portIDs) {
    tcvrIds.push_back(
        platform()->getPlatformPort(portID)->getTransceiverID().value());
  }

  try {
    qsfpServiceClient->sync_getTransceiverInfo(tcvrInfos, tcvrIds);
  } catch (const std::exception& ex) {
    XLOG(WARN) << "Failed to call qsfp_service getTransceiverInfo(). "
               << folly::exceptionStr(ex);
  }

  for (auto portID : portIDs) {
    auto portName = getPortName(portID);
    XLOG(ERR) << "Debug information for " << portName;
    if (iPhyInfos.find(portID) != iPhyInfos.end()) {
      XLOG(ERR) << "IPHY INFO: "
                << apache::thrift::debugString(iPhyInfos[portID]);
    } else {
      XLOG(ERR) << "IPHY info missing for " << portName;
    }

    if (xPhyInfos.find(portName) != xPhyInfos.end()) {
      XLOG(ERR) << "XPHY INFO: "
                << apache::thrift::debugString(xPhyInfos[portName]);
    } else {
      XLOG(ERR) << "XPHY info missing for " << portName;
    }

    auto tcvrId =
        platform()->getPlatformPort(portID)->getTransceiverID().value();
    if (tcvrInfos.find(tcvrId) != tcvrInfos.end()) {
      XLOG(ERR) << "Transceiver INFO: "
                << apache::thrift::debugString(tcvrInfos[tcvrId]);
    } else {
      XLOG(ERR) << "Transceiver info missing for " << portName;
    }
  }
}

void LinkTest::setLinkState(bool enable, std::vector<PortID>& portIds) {
  for (const auto& port : portIds) {
    setPortStatus(port, enable);
  }
  EXPECT_NO_THROW(
      waitForLinkStatus(portIds, enable, 60, std::chrono::milliseconds(1000)););
}

std::vector<std::pair<PortID, PortID>> LinkTest::getPortPairsForFecErrInj()
    const {
  auto connectedPairs = getConnectedPairs();
  std::unordered_set<phy::FecMode> supportedFecs = {
      phy::FecMode::RS528, phy::FecMode::RS544, phy::FecMode::RS544_2N};
  std::vector<std::pair<PortID, PortID>> supportedPorts;
  for (const auto& [port1, port2] : connectedPairs) {
    auto fecPort1 = platform()->getHwSwitch()->getPortFECMode(port1);
    auto fecPort2 = platform()->getHwSwitch()->getPortFECMode(port2);
    if (fecPort1 != fecPort2) {
      throw FbossError(
          "FEC different on both ends of the link: ", fecPort1, fecPort2);
    }
    if (supportedFecs.find(fecPort1) != supportedFecs.end()) {
      supportedPorts.push_back({port1, port2});
    }
  }
  return supportedPorts;
}

int linkTestMain(
    int argc,
    char** argv,
    PlatformInitFn initPlatformFn,
    std::optional<cfg::StreamType> streamType) {
  ::testing::InitGoogleTest(&argc, argv);
  initAgentTest(argc, argv, initPlatformFn, streamType);
  return RUN_ALL_TESTS();
}

} // namespace facebook::fboss
