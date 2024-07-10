load("@fbcode_macros//build_defs:cpp_library.bzl", "cpp_library")
load("//fboss/agent/hw/sai/impl:impl.bzl", "SAI_PHY_IMPLS", "to_sdk_suffix")

_CREDO_SRCS = [
    "facebook/credo/cloudripper/CloudRipperPhyManager.cpp",
    "facebook/credo/elbert/ElbertPhyManager.cpp",
]

_MRVL_SRCS = [
    "facebook/mvl/sandia/SandiaPhyManager.cpp",
]

_ALL_SRCS = _CREDO_SRCS + _MRVL_SRCS

def _get_srcs(sai_impl):
    # TODO: actually split up sources once we support doing so
    if sai_impl.name == "credo":
        return _ALL_SRCS
    elif sai_impl.name == "mrvl":
        return _ALL_SRCS
    else:
        return _ALL_SRCS

def _sai_phy_management_lib(sai_impl):
    impl_suffix = to_sdk_suffix(sai_impl)
    return cpp_library(
        name = "sai-phy-management{}".format(impl_suffix),
        srcs = _get_srcs(sai_impl) + [
            "SaiPhyManager.cpp",
        ],
        headers = [
            "SaiPhyManager.h",
        ],
        exported_deps = [
            "//fboss/lib/phy/facebook:sai_xphy{}".format(impl_suffix),
            "//fboss/lib/phy/facebook/credo:recursive_glob_headers",  # @manual
        ],
    )

def all_sai_phy_management_libs():
    for sai_impl in SAI_PHY_IMPLS:
        _sai_phy_management_lib(sai_impl)
