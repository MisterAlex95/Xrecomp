# volk and the pinned framegen library, for the LSFG bridge (src/framegen/CMakeLists.txt, tests/lsfg_bridge).
# In: XBOXRECOMP_LSFG_VK_SRC. Out: the targets volk and lsfg-vk-framegen (both PIC, so they link into libmain.so).
#
# volk comes the way the fork pins it: its thirdparty/volk submodule (zeux/volk be3dbd49), populated with
# `git -C $LSFG_VK_SRC submodule update --init thirdparty/volk` (only that one: pe-parse, dxbc and toml11 are not
# used). Vulkan headers: lsfg_vulkan_headers.cmake (the NDK sysroot on Android; elsewhere XBOXRECOMP_VULKAN_HEADERS,
# else the Khronos headers this repository carries for paraLLEl-RDP).
if(TARGET lsfg-vk-framegen)
    return()
endif()
enable_language(CXX)
set(_lsfg_src "${XBOXRECOMP_LSFG_VK_SRC}")
if(NOT EXISTS "${_lsfg_src}/framegen/public/lsfg_3_1.hpp")
    message(FATAL_ERROR "XBOXRECOMP_LSFG_VK_SRC=${_lsfg_src}: no framegen/public/lsfg_3_1.hpp there "
                        "(clone GameNative/lsfg-vk-android and check out d33caceb)")
endif()
set(XBOXRECOMP_VOLK_SRC "${_lsfg_src}/thirdparty/volk" CACHE PATH "volk sources (default: the fork's submodule)")
if(NOT EXISTS "${XBOXRECOMP_VOLK_SRC}/volk.c")
    message(FATAL_ERROR "No volk in ${XBOXRECOMP_VOLK_SRC}: run "
                        "`git -C ${_lsfg_src} submodule update --init thirdparty/volk`")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/lsfg_vulkan_headers.cmake)
if(NOT ANDROID)
    set(VULKAN_HEADERS_INSTALL_DIR "${XBR_LSFG_VULKAN_HEADERS_STAGE}" CACHE PATH "" FORCE)   # volk adds /include
endif()
if(ANDROID)
    set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)   # the NDK sysroot has the headers; libvulkan is dlopened
    # framegen's AHardwareBuffer paths (framegen/src/core/image.cpp) use the Android platform types; volk's
    # PUBLIC define carries the macro to framegen and the bridge.
    set(VOLK_STATIC_DEFINES "VK_USE_PLATFORM_ANDROID_KHR" CACHE STRING "" FORCE)
endif()

add_subdirectory("${XBOXRECOMP_VOLK_SRC}" "${CMAKE_BINARY_DIR}/lsfg-volk" EXCLUDE_FROM_ALL)
add_subdirectory("${_lsfg_src}/framegen" "${CMAKE_BINARY_DIR}/lsfg-framegen" EXCLUDE_FROM_ALL)
set_target_properties(volk lsfg-vk-framegen PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The fork's own flags for its sources (its root CMakeLists.txt); it is not our code, keep it quiet.
target_compile_options(lsfg-vk-framegen PRIVATE -Wno-deprecated-declarations -Wno-unused-template -w)
