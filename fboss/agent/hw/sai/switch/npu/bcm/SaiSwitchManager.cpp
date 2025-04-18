// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"

#include "fboss/agent/hw/HwSwitchFb303Stats.h"

extern "C" {
#ifndef IS_OSS_BRCM_SAI
#include <experimental/saiswitchextensions.h>
#else
#include <saiswitchextensions.h>
#endif
}

namespace facebook::fboss {

void fillHwSwitchDramStats(
    const folly::F14FastMap<sai_stat_id_t, uint64_t>& counterId2Value,
    HwSwitchDramStats& hwSwitchDramStats) {
  for (auto counterIdAndValue : counterId2Value) {
    auto [counterId, value] = counterIdAndValue;
    switch (counterId) {
#if defined(BRCM_SAI_SDK_DNX)
      case SAI_SWITCH_STAT_DEVICE_DRAM_ENQUEUED_BYTES:
        hwSwitchDramStats.dramEnqueuedBytes() = value;
        break;
      case SAI_SWITCH_STAT_DEVICE_DRAM_DEQUEUED_BYTES:
        hwSwitchDramStats.dramDequeuedBytes() = value;
        break;
#if defined(BRCM_SAI_SDK_GTE_11_0)
      case SAI_SWITCH_STAT_DEVICE_DRAM_BLOCK_TOTAL_TIME:
        hwSwitchDramStats.dramBlockedTimeNsec() = value;
        break;
#endif
#endif
      default:
        throw FbossError("Got unexpected switch counter id: ", counterId);
    }
  }
}

void fillHwSwitchWatermarkStats(
    const folly::F14FastMap<sai_stat_id_t, uint64_t>& counterId2Value,
    HwSwitchWatermarkStats& hwSwitchWatermarkStats) {
  hwSwitchWatermarkStats = {};
  for (auto counterIdAndValue : counterId2Value) {
    auto [counterId, value] = counterIdAndValue;
    switch (counterId) {
#if defined(BRCM_SAI_SDK_DNX_GTE_11_0)
      case SAI_SWITCH_STAT_DEVICE_RCI_WATERMARK_BYTES:
        hwSwitchWatermarkStats.fdrRciWatermarkBytes() = value;
        break;
      case SAI_SWITCH_STAT_DEVICE_CORE_RCI_WATERMARK_BYTES:
        hwSwitchWatermarkStats.coreRciWatermarkBytes() = value;
        break;
      case SAI_SWITCH_STAT_HIGHEST_QUEUE_CONGESTION_LEVEL:
        hwSwitchWatermarkStats.dtlQueueWatermarkBytes() = value;
        break;
#endif
      default:
        throw FbossError("Got unexpected switch counter id: ", counterId);
    }
  }
}

} // namespace facebook::fboss
