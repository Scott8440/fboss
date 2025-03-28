/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/lib/platforms/PlatformProductInfo.h"
#include "fboss/agent/FbossError.h"

#include <boost/algorithm/string.hpp>
#include <folly/FileUtil.h>
#include <folly/MacAddress.h>
#include <folly/json/dynamic.h>
#include <folly/json/json.h>
#include <folly/logging/xlog.h>
#include <folly/testing/TestUtil.h>

namespace {
constexpr auto kInfo = "Information";
constexpr auto kSysMfgDate = "System Manufacturing Date";
constexpr auto kSysMfg = "System Manufacturer";
constexpr auto kSysAmbPartNum = "System Assembly Part Number";
constexpr auto kAmbAt = "Assembled At";
constexpr auto kPcbMfg = "PCB Manufacturer";
constexpr auto kProdAssetTag = "Product Asset Tag";
constexpr auto kProdName = "Product Name";
constexpr auto kProdVersion = "Product Version";
constexpr auto kProductionState = "Product Production State";
constexpr auto kProdPartNum = "Product Part Number";
constexpr auto kSerialNum = "Product Serial Number";
constexpr auto kSubVersion = "Product Sub-Version";
constexpr auto kOdmPcbaPartNum = "ODM PCBA Part Number";
constexpr auto kOdmPcbaSerialNum = "ODM PCBA Serial Number";
constexpr auto kFbPcbaPartNum = "Facebook PCBA Part Number";
constexpr auto kFbPcbPartNum = "Facebook PCB Part Number";
constexpr auto kExtMacSize = "Extended MAC Address Size";
constexpr auto kExtMacBase = "Extended MAC Base";
constexpr auto kLocalMac = "Local MAC";
constexpr auto kVersion = "Version";
constexpr auto kFabricLocation = "Location on Fabric";
} // namespace

DEFINE_string(
    mode,
    "",
    "The mode the FBOSS controller is running as, wedge, lc, or fc");
DEFINE_string(
    fruid_filepath,
    "/var/facebook/fboss/fruid.json",
    "File for storing the fruid data");

namespace facebook::fboss {

using folly::dynamic;
using folly::MacAddress;
using folly::parseJson;
using folly::StringPiece;

PlatformProductInfo::PlatformProductInfo(StringPiece path) : path_(path) {}

void PlatformProductInfo::initialize() {
  try {
    std::string data;
    folly::readFile(path_.str().c_str(), data);
    parse(data);
  } catch (const std::exception& err) {
    XLOG(ERR) << "Failed initializing ProductInfo from " << path_
              << ", fall back to use fbwhoami: " << err.what();
    // if fruid info fails fall back to fbwhoami
    initFromFbWhoAmI();
  }
  initMode();
}

std::string PlatformProductInfo::getFabricLocation() {
  return *productInfo_.fabricLocation();
}

std::string PlatformProductInfo::getProductName() {
  return *productInfo_.product();
}

void PlatformProductInfo::initMode() {
  if (FLAGS_mode.empty()) {
    auto modelName = getProductName();
    if (modelName.find("MINIPACK2") == 0) {
      type_ = PlatformType::PLATFORM_FUJI;
    } else if (
        modelName.find("Wedge100") == 0 || modelName.find("WEDGE100") == 0) {
      // Wedge100 comes from fruid.json, WEDGE100 comes from fbwhoami
      type_ = PlatformType::PLATFORM_WEDGE100;
    } else if (
        modelName.find("Wedge400c") == 0 || modelName.find("WEDGE400C") == 0) {
      type_ = PlatformType::PLATFORM_WEDGE400C;
    } else if (
        modelName.find("Wedge400") == 0 || modelName.find("WEDGE400") == 0) {
      type_ = PlatformType::PLATFORM_WEDGE400;
    } else if (
        modelName.find("Darwin") == 0 || modelName.find("DARWIN") == 0 ||
        modelName.find("DCS-7060") == 0 || modelName.find("Rackhawk") == 0) {
      type_ = PlatformType::PLATFORM_DARWIN;
    } else if (modelName.find("Wedge") == 0 || modelName.find("WEDGE") == 0) {
      type_ = PlatformType::PLATFORM_WEDGE;
    } else if (modelName.find("SCM-LC") == 0 || modelName.find("LC") == 0) {
      // TODO remove LC once fruid.json is fixed on Galaxy Linecards
      type_ = PlatformType::PLATFORM_GALAXY_LC;
    } else if (
        modelName.find("SCM-FC") == 0 || modelName.find("SCM-FAB") == 0 ||
        modelName.find("FAB") == 0) {
      // TODO remove FAB once fruid.json is fixed on Galaxy fabric cards
      type_ = PlatformType::PLATFORM_GALAXY_FC;
    } else if (
        modelName.find("Montblanc") == 0 || modelName.find("MONTBLANC") == 0 ||
        modelName.find("MINIPACK3_CHASSIS_BUNDLE") == 0) {
      type_ = PlatformType::PLATFORM_MONTBLANC;
    } else if (
        modelName.find("MINIPACK") == 0 || modelName.find("MINIPHOTON") == 0) {
      type_ = PlatformType::PLATFORM_MINIPACK;
    } else if (modelName.find("DCS-7368") == 0 || modelName.find("YAMP") == 0) {
      type_ = PlatformType::PLATFORM_YAMP;
    } else if (
        modelName.find("DCS-7388") == 0 || modelName.find("ELBERT") == 0) {
      type_ = PlatformType::PLATFORM_ELBERT;
    } else if (modelName.find("fake_wedge40") == 0) {
      type_ = PlatformType::PLATFORM_FAKE_WEDGE40;
    } else if (modelName.find("fake_wedge") == 0) {
      type_ = PlatformType::PLATFORM_FAKE_WEDGE;
    } else if (modelName.find("CLOUDRIPPER") == 0) {
      type_ = PlatformType::PLATFORM_CLOUDRIPPER;
    } else if (
        modelName.find("Sandia") == 0 || modelName.find("SANDIA") == 0 ||
        modelName.find("8508-F-SYS-HV") == 0) {
      type_ = PlatformType::PLATFORM_SANDIA;
    } else if (
        modelName.find("Meru400biu") == 0 ||
        modelName.find("S9710-76D-BB12") == 0) {
      type_ = PlatformType::PLATFORM_MERU400BIU;
    } else if (modelName.find("Meru400bia") == 0) {
      type_ = PlatformType::PLATFORM_MERU400BIA;
    } else if (
        modelName.find("Meru400bfu") == 0 ||
        modelName.find("S9705-48D-4B4") == 0) {
      type_ = PlatformType::PLATFORM_MERU400BFU;
    } else if (
        modelName.find("Meru800bia") == 0 ||
        modelName.find("MERU800BIA") == 0 ||
        modelName.find("ASY-92458-101") == 0 ||
        modelName.find("ASY-92493-104") == 0 ||
        modelName.find("ASY-92458-104") == 0 ||
        modelName.find("DCS-DL-7700R4C-38PE-AC-F") == 0 ||
        modelName.find("DCS-DL-7700R4C-38PE-DC-F") == 0) {
      type_ = PlatformType::PLATFORM_MERU800BIA;
    } else if (
        modelName.find("Meru800bfa") == 0 ||
        modelName.find("MERU800BFA") == 0 ||
        modelName.find("ASY-57651-102") == 0 ||
        modelName.find("DCS-DS-7720R4-128PE-AC-F") == 0) {
      type_ = PlatformType::PLATFORM_MERU800BFA;
    } else if (
        modelName.find("MORGAN800CC") == 0 ||
        modelName.find("8501-SYS-MT") == 0) {
      type_ = PlatformType::PLATFORM_MORGAN800CC;
    } else if (modelName.find("FAKE_SAI") == 0) {
      type_ = PlatformType::PLATFORM_FAKE_SAI;
    } else if (
        modelName.find("JANGA800BIC") == 0 || modelName.find("JANGA") == 0) {
      type_ = PlatformType::PLATFORM_JANGA800BIC;
    } else if (
        modelName.find("TAHAN800BC") == 0 ||
        modelName.find("R4063-F9001-01") == 0) {
      type_ = PlatformType::PLATFORM_TAHAN800BC;
    } else {
      throw std::runtime_error("invalid model name " + modelName);
    }
  } else {
    if (FLAGS_mode == "wedge") {
      type_ = PlatformType::PLATFORM_WEDGE;
    } else if (FLAGS_mode == "wedge100") {
      type_ = PlatformType::PLATFORM_WEDGE100;
    } else if (FLAGS_mode == "galaxy_lc") {
      type_ = PlatformType::PLATFORM_GALAXY_LC;
    } else if (FLAGS_mode == "galaxy_fc") {
      type_ = PlatformType::PLATFORM_GALAXY_FC;
    } else if (FLAGS_mode == "minipack") {
      type_ = PlatformType::PLATFORM_MINIPACK;
    } else if (FLAGS_mode == "yamp") {
      type_ = PlatformType::PLATFORM_YAMP;
    } else if (FLAGS_mode == "fake_wedge40") {
      type_ = PlatformType::PLATFORM_FAKE_WEDGE40;
    } else if (FLAGS_mode == "wedge400") {
      type_ = PlatformType::PLATFORM_WEDGE400;
    } else if (FLAGS_mode == "wedge400_grandteton") {
      type_ = PlatformType::PLATFORM_WEDGE400_GRANDTETON;
    } else if (FLAGS_mode == "fuji") {
      type_ = PlatformType::PLATFORM_FUJI;
    } else if (FLAGS_mode == "elbert") {
      type_ = PlatformType::PLATFORM_ELBERT;
    } else if (FLAGS_mode == "darwin") {
      type_ = PlatformType::PLATFORM_DARWIN;
    } else if (FLAGS_mode == "sandia") {
      type_ = PlatformType::PLATFORM_SANDIA;
    } else if (FLAGS_mode == "meru400biu") {
      type_ = PlatformType::PLATFORM_MERU400BIU;
    } else if (FLAGS_mode == "meru800bia") {
      type_ = PlatformType::PLATFORM_MERU800BIA;
    } else if (FLAGS_mode == "meru800bfa") {
      type_ = PlatformType::PLATFORM_MERU800BFA;
    } else if (FLAGS_mode == "meru800bfa_p1") {
      type_ = PlatformType::PLATFORM_MERU800BFA_P1;
    } else if (FLAGS_mode == "meru400bia") {
      type_ = PlatformType::PLATFORM_MERU400BIA;
    } else if (FLAGS_mode == "meru400bfu") {
      type_ = PlatformType::PLATFORM_MERU400BFU;
    } else if (FLAGS_mode == "wedge400c") {
      type_ = PlatformType::PLATFORM_WEDGE400C;
    } else if (FLAGS_mode == "wedge400c_voq") {
      type_ = PlatformType::PLATFORM_WEDGE400C_VOQ;
    } else if (FLAGS_mode == "wedge400c_fabric") {
      type_ = PlatformType::PLATFORM_WEDGE400C_FABRIC;
    } else if (FLAGS_mode == "cloudripper_voq") {
      type_ = PlatformType::PLATFORM_CLOUDRIPPER_VOQ;
    } else if (FLAGS_mode == "cloudripper_fabric") {
      type_ = PlatformType::PLATFORM_CLOUDRIPPER_FABRIC;
    } else if (FLAGS_mode == "montblanc") {
      type_ = PlatformType::PLATFORM_MONTBLANC;
    } else if (FLAGS_mode == "fake_sai") {
      type_ = PlatformType::PLATFORM_FAKE_SAI;
    } else if (FLAGS_mode == "janga800bic") {
      type_ = PlatformType::PLATFORM_JANGA800BIC;
    } else if (FLAGS_mode == "tahan800bc") {
      type_ = PlatformType::PLATFORM_TAHAN800BC;
    } else if (FLAGS_mode == "morgan800cc") {
      type_ = PlatformType::PLATFORM_MORGAN800CC;
    } else {
      throw std::runtime_error("invalid mode " + FLAGS_mode);
    }
  }
}

void PlatformProductInfo::parse(std::string data) {
  dynamic info;
  try {
    info = parseJson(data).at(kInfo);
  } catch (const std::exception& err) {
    XLOG(ERR) << err.what();
    // Handle fruid data present outside of "Information" i.e.
    // {
    //   "Information" : fruid json
    // }
    // vs
    // {
    //  Fruid json
    // }
    info = parseJson(data);
  }
  productInfo_.oem() = folly::to<std::string>(info[kSysMfg].asString());
  productInfo_.product() = folly::to<std::string>(info[kProdName].asString());
  productInfo_.serial() = folly::to<std::string>(info[kSerialNum].asString());
  productInfo_.mfgDate() = folly::to<std::string>(info[kSysMfgDate].asString());
  productInfo_.systemPartNumber() =
      folly::to<std::string>(info[kSysAmbPartNum].asString());
  productInfo_.assembledAt() = folly::to<std::string>(info[kAmbAt].asString());
  productInfo_.pcbManufacturer() =
      folly::to<std::string>(info[kPcbMfg].asString());
  productInfo_.assetTag() =
      folly::to<std::string>(info[kProdAssetTag].asString());
  productInfo_.partNumber() =
      folly::to<std::string>(info[kProdPartNum].asString());
  productInfo_.odmPcbaPartNumber() =
      folly::to<std::string>(info[kOdmPcbaPartNum].asString());
  productInfo_.odmPcbaSerial() =
      folly::to<std::string>(info[kOdmPcbaSerialNum].asString());
  productInfo_.fbPcbaPartNumber() =
      folly::to<std::string>(info[kFbPcbaPartNum].asString());
  productInfo_.fbPcbPartNumber() =
      folly::to<std::string>(info[kFbPcbPartNum].asString());

  productInfo_.fabricLocation() =
      folly::to<std::string>(info[kFabricLocation].asString());
  // FB only - we apply custom logic to construct unique SN for
  // cases where we create multiple assets for a single physical
  // card in chassis.
  setFBSerial();
  productInfo_.version() = info[kVersion].asInt();
  productInfo_.subVersion() = info[kSubVersion].asInt();
  productInfo_.productionState() = info[kProductionState].asInt();
  productInfo_.productVersion() = info[kProdVersion].asInt();
  productInfo_.bmcMac() = folly::to<std::string>(info[kLocalMac].asString());
  productInfo_.mgmtMac() = folly::to<std::string>(info[kExtMacBase].asString());
  auto macBase = MacAddress(info[kExtMacBase].asString()).u64HBO() + 1;
  productInfo_.macRangeStart() = MacAddress::fromHBO(macBase).toString();
  productInfo_.macRangeSize() = info[kExtMacSize].asInt() - 1;
}

std::unique_ptr<PlatformProductInfo> fakeProductInfo() {
  // Dummy Fruid for fake platform
  static const std::string kFakeFruidJson = R"<json>({"Information": {
      "PCB Manufacturer" : "Facebook",
      "System Assembly Part Number" : "42",
      "ODM PCBA Serial Number" : "SN",
      "Product Name" : "fake_wedge",
      "Location on Fabric" : "",
      "ODM PCBA Part Number" : "PN",
      "CRC8" : "0xcc",
      "Version" : "1",
      "Product Asset Tag" : "42",
      "Product Part Number" : "42",
      "Assembled At" : "Facebook",
      "System Manufacturer" : "Facebook",
      "Product Production State" : "42",
      "Facebook PCB Part Number" : "42",
      "Product Serial Number" : "SN",
      "Local MAC" : "42:42:42:42:42:42",
      "Extended MAC Address Size" : "1",
      "Extended MAC Base" : "42:42:42:42:42:42",
      "System Manufacturing Date" : "01-01-01",
      "Product Version" : "42",
      "Product Sub-Version" : "22",
      "Facebook PCBA Part Number" : "42"
    }, "Actions": [], "Resources": []})<json>";

  folly::test::TemporaryDirectory tmpDir;
  auto fruidFilename = tmpDir.path().string() + "fruid.json";
  folly::writeFile(kFakeFruidJson, fruidFilename.c_str());
  auto productInfo =
      std::make_unique<facebook::fboss::PlatformProductInfo>(fruidFilename);
  productInfo->initialize();
  return productInfo;
}
} // namespace facebook::fboss
