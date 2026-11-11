# volk and the pinned framegen library, for the LSFG bridge (src/framegen/CMakeLists.txt, tests/lsfg_bridge).
# In: XBOXRECOMP_LSFG_VK_SRC. Out: the targets volk and lsfg-vk-framegen (both PIC, so they link into libmain.so).
#
# volk comes the way the fork pins it: its thirdparty/volk submodule (zeux/volk be3dbd49), populated with
# `git -C $LSFG_VK_SRC submodule update --init thirdparty/volk` (only that one: pe-parse, dxbc and toml11 are not
# used). Vulkan headers: the NDK sysroot on Android; elsewhere XBOXRECOMP_VULKAN_HEADERS (a directory holding
# vulkan/vulkan_core.h, e.g. Vulkan-Headers/include or the NDK's sysroot/usr/include: only its vulkan/ and
# vk_video/ subdirectories are staged into the build tree), else a system Vulkan SDK (find_package(Vulkan)).
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

set(XBOXRECOMP_VULKAN_HEADERS "$ENV{VULKAN_HEADERS}" CACHE PATH
    "Directory holding vulkan/vulkan_core.h (not needed on Android or with a Vulkan SDK)")
if(XBOXRECOMP_VULKAN_HEADERS AND NOT ANDROID)
    if(NOT EXISTS "${XBOXRECOMP_VULKAN_HEADERS}/vulkan/vulkan_core.h")
        message(FATAL_ERROR "XBOXRECOMP_VULKAN_HEADERS=${XBOXRECOMP_VULKAN_HEADERS}: no vulkan/vulkan_core.h")
    endif()
    # Staged so that a sysroot's libc headers never shadow the host's.
    set(_vkh "${CMAKE_BINARY_DIR}/lsfg-vulkan-headers")
    file(COPY "${XBOXRECOMP_VULKAN_HEADERS}/vulkan" DESTINATION "${_vkh}/include")
    if(EXISTS "${XBOXRECOMP_VULKAN_HEADERS}/vk_video")
        file(COPY "${XBOXRECOMP_VULKAN_HEADERS}/vk_video" DESTINATION "${_vkh}/include")
    endif()
    set(VULKAN_HEADERS_INSTALL_DIR "${_vkh}" CACHE PATH "" FORCE)   # volk adds /include
endif()
if(ANDROID)
    set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)   # the NDK sysroot has the headers; libvulkan is dlopened
endif()

add_subdirectory("${XBOXRECOMP_VOLK_SRC}" "${CMAKE_BINARY_DIR}/lsfg-volk" EXCLUDE_FROM_ALL)
add_subdirectory("${_lsfg_src}/framegen" "${CMAKE_BINARY_DIR}/lsfg-framegen" EXCLUDE_FROM_ALL)
set_target_properties(volk lsfg-vk-framegen PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The fork's own flags for its sources (its root CMakeLists.txt); it is not our code, keep it quiet.
target_compile_options(lsfg-vk-framegen PRIVATE -Wno-deprecated-declarations -Wno-unused-template -w)
