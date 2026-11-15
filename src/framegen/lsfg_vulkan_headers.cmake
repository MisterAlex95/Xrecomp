# The Vulkan headers the bridge, volk and the DXBC translator compile against, off Android (where the NDK sysroot has
# them): XBOXRECOMP_VULKAN_HEADERS (or $VULKAN_HEADERS), a directory holding vulkan/vulkan_core.h (Vulkan-Headers'
# include, a Vulkan SDK's, the NDK's sysroot/usr/include), else the Khronos headers this repository carries for
# paraLLEl-RDP (third_party/parallel-rdp/vulkan-headers, Apache-2.0), so no Vulkan SDK is needed. Only vulkan/ and
# vk_video/ are staged into the build tree, so that a sysroot's libc headers never shadow the host's.
# Out: XBR_LSFG_VULKAN_HEADERS_STAGE (its include/ holds vulkan/).
set(XBR_LSFG_VULKAN_HEADERS_STAGE "${CMAKE_BINARY_DIR}/lsfg-vulkan-headers")
include_guard(GLOBAL)
set(XBOXRECOMP_VULKAN_HEADERS "$ENV{VULKAN_HEADERS}" CACHE PATH
    "Directory holding vulkan/vulkan_core.h (empty: third_party/parallel-rdp/vulkan-headers/include)")
set(_xbr_vkh_from "${XBOXRECOMP_VULKAN_HEADERS}")
if(NOT _xbr_vkh_from)
    get_filename_component(_xbr_vkh_from "${CMAKE_CURRENT_LIST_DIR}/../../third_party/parallel-rdp/vulkan-headers/include"
                           ABSOLUTE)
endif()
if(NOT EXISTS "${_xbr_vkh_from}/vulkan/vulkan_core.h")
    message(FATAL_ERROR "XBOXRECOMP_VULKAN_HEADERS=${_xbr_vkh_from}: no vulkan/vulkan_core.h")
endif()
file(REMOVE_RECURSE "${XBR_LSFG_VULKAN_HEADERS_STAGE}/include")
file(COPY "${_xbr_vkh_from}/vulkan" DESTINATION "${XBR_LSFG_VULKAN_HEADERS_STAGE}/include")
if(EXISTS "${_xbr_vkh_from}/vk_video")
    file(COPY "${_xbr_vkh_from}/vk_video" DESTINATION "${XBR_LSFG_VULKAN_HEADERS_STAGE}/include")
endif()
