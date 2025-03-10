// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include "fboss/platform/platform_manager/gen-cpp2/platform_manager_config_types.h"

namespace facebook::fboss::platform::platform_manager {

class Utils {
 public:
  PlatformConfig getConfig();
  // Recursively create directories for the given path.
  // - for given /x/y/z, directory y/z if x already exists.
  // No-op if parent directories already exist.
  // Returns true if created or already exist, otherwise false.
  bool createDirectories(const std::string& path);

  // Extract (SlotPath, DeviceName) from DevicePath.
  // Returns a pair of (SlotPath, DeviceName). Throws if DevicePath is invalid.
  // Eg: /MCB_SLOT@0/[IDPROM] will return std::pair("/MCB_SLOT@0", "IDPROM")
  std::pair<std::string, std::string> parseDevicePath(
      const std::string& devicePath);

  // Construct and Return a DevicePath from given SlotPath and DeviceName
  // Eg: SlotPath:"/MCB_SLOT@0", DeviceName:"IDPROM" will return
  // /MCB_SLOT@0/[IDPROM]
  std::string createDevicePath(
      const std::string& slotPath,
      const std::string& deviceName);

  // Explore and resolve GpioChip's CharDevicePath for given SysfsPath.
  // Throws an exception when it fails to resolve CharDevicePath
  std::string resolveGpioChipCharDevPath(const std::string& sysfsPath);
};

} // namespace facebook::fboss::platform::platform_manager
