#pragma once
#include <vector>
#include "fboss/agent/Main.h"
#include "fboss/agent/test/prod_agent_tests/ProdAgentTests.h"

#include "fboss/agent/SetupThrift.h"

class HwSwitch;

namespace facebook::fboss {

void verifyHwSwitchHandler();

class ProdInvariantTest : public ProdAgentTests {
 protected:
  virtual void SetUp() override;
  virtual void setupConfigFlag() override;
  virtual cfg::SwitchConfig initialConfig();
  void verifyAcl();
  void verifyCopp();
  void verifySafeDiagCommands();
  void verifyLoadBalancing();
  void verifyDscpToQueueMapping();
  void verifyQueuePerHostMapping(bool dscpMarkingTest);
  std::vector<PortDescriptor> ecmpPorts_{};
  bool checkBaseConfigPortsEmpty();
  cfg::SwitchConfig getConfigFromFlag();
  void verifyThriftHandler();
  void verifySwSwitchHandler();
  void set_mmu_lossless(bool mmu_lossless) {
    mmuLosslessMode_ = mmu_lossless;
  }
  bool is_mmu_lossless_mode() {
    return mmuLosslessMode_;
  }

 protected:
  std::optional<bool> useProdConfig_ = std::nullopt;
  PortID getDownlinkPort();

 private:
  std::vector<PortID> getEcmpPortIds();
  void sendTraffic();
  void setupAgentTestEcmp(const std::vector<PortDescriptor>& ecmpPorts);
  std::map<PortID, HwPortStats> getLatestPortStats(
      const std::vector<PortID>& ports);
  bool mmuLosslessMode_ = false;
};

int ProdInvariantTestMain(int argc, char** argv, PlatformInitFn initPlatformFn);
} // namespace facebook::fboss
