# Frame generation (LSFG, docs/technical/framegen.md). Built by default on POSIX hosts (root CMakeLists.txt,
# XBOXRECOMP_LSFG; -DXBOXRECOMP_LSFG=OFF leaves it out).
#
# xbox_framegen holds:
#   - the pacer and the fake generator (C, libc + pthreads), always, through host_framegen (the host layer's);
#   - the shader-cache helpers (lsfg_shaders, C, libc only: lsfg_shaders.cmake), always;
#   - the Vulkan bridge (lsfg_bridge.cpp, C++20) and its generator (lsfg_generator.c), over the pinned MIT framegen
#     (GameNative/lsfg-vk-android d33caceb with volk): XBOXRECOMP_LSFG_VK_SRC (or $LSFG_VK_SRC) when it names a
#     checkout, else the pinned tarballs fetched by lsfg_sources.cmake (XBOXRECOMP_LSFG_FETCH, ON). With neither
#     the build has the fake generator only. The pacer reaches the bridge through the FramegenGenerator vtable
#     (framegen_pacer.h).
#   - the DXBC translator (lsfg_dxbc, C++17 over DXVK's DXBC compiler, lsfg_dxbc.cmake): XBOXRECOMP_LSFG_DXVK_SRC (or
#     $DXVK_SRC), else on Android the pinned DXVK v2.6.2 fetched (libxba_import.so translates at import). Desktops
#     translate with tools/lsfg/lsfg_translate, which fetches DXVK itself.
# The pacer and the fake generator are the host layer's (host_framegen.cmake, XBA-145): xbox_framegen links them,
# and is a real archive only with the bridge (an interface target over host_framegen and lsfg_shaders otherwise).
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
include(${CMAKE_CURRENT_SOURCE_DIR}/host_framegen.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/lsfg_shaders.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/lsfg_sources.cmake)
option(XBOXRECOMP_LSFG_FETCH "Fetch the pinned framegen / DXVK sources when no checkout is given" ON)

# ── The Vulkan bridge ─────────────────────────────────────────
set(XBOXRECOMP_LSFG_VK_SRC "$ENV{LSFG_VK_SRC}" CACHE PATH
    "Checkout of GameNative/lsfg-vk-android at d33caceb (MIT framegen/), with thirdparty/volk; empty = the pinned tarball")
if(NOT XBOXRECOMP_LSFG_VK_SRC AND XBOXRECOMP_LSFG_FETCH)
    xbr_lsfg_fetch_framegen(XBOXRECOMP_LSFG_VK_SRC)   # a normal variable: the cache entry stays empty
endif()
if(XBOXRECOMP_LSFG_VK_SRC)
    include(${CMAKE_CURRENT_SOURCE_DIR}/lsfg_vulkan.cmake)   # volk + framegen targets
    add_library(xbox_framegen STATIC lsfg_bridge.cpp lsfg_generator.c)
    target_include_directories(xbox_framegen PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${XBOXRECOMP_SRC_DIR}
    )
    target_link_libraries(xbox_framegen PUBLIC host_framegen lsfg_shaders)
    set_target_properties(xbox_framegen PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
                                                   POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(xbox_framegen PRIVATE lsfg-vk-framegen volk)
    target_compile_definitions(xbox_framegen PUBLIC XBOXRECOMP_LSFG_BRIDGE=1)
    message(STATUS "LSFG: bridge built against ${XBOXRECOMP_LSFG_VK_SRC}")
else()
    add_library(xbox_framegen INTERFACE)
    target_include_directories(xbox_framegen INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${XBOXRECOMP_SRC_DIR}
    )
    target_link_libraries(xbox_framegen INTERFACE host_framegen lsfg_shaders)
    message(STATUS "LSFG: no framegen sources (XBOXRECOMP_LSFG_FETCH=OFF, no XBOXRECOMP_LSFG_VK_SRC): pacer and fake "
                   "generator only")
endif()

# ── The DXBC translator ───────────────────────────────────────
set(XBOXRECOMP_LSFG_DXVK_SRC "$ENV{DXVK_SRC}" CACHE PATH
    "DXVK checkout (v2.6.2, with include/spirv) for the DXBC -> SPIR-V translator; empty = the pinned tarball on Android")
if(NOT XBOXRECOMP_LSFG_DXVK_SRC AND XBOXRECOMP_LSFG_FETCH AND ANDROID)
    xbr_lsfg_fetch_dxvk(XBOXRECOMP_LSFG_DXVK_SRC)
endif()
if(XBOXRECOMP_LSFG_DXVK_SRC)
    include(${CMAKE_CURRENT_SOURCE_DIR}/lsfg_dxbc.cmake)
endif()
