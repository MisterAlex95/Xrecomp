cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.14.0 LANGUAGES C)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (147 imports → Win32)
#   xbox_d3d8      - D3D8 → D3D11 graphics compatibility
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a      - NV2A GPU emulation (extracted from xemu)

# Frame generation (LSFG, docs/technical/framegen.md): the pacer (C) and the Vulkan bridge (C++20, over the pinned
# MIT framegen that src/framegen fetches when no checkout is given). On by default on POSIX hosts (XBA-187) when
# this is the top-level project or the project adding the toolkit enabled CXX (templates/new-game, the Android app):
# a C project cannot link the bridge, so the test projects under tests/ (C only, add_subdirectory of the toolkit)
# keep building without it. -DXBOXRECOMP_LSFG=OFF leaves it out (offline builds). ON also defines XBOXRECOMP_LSFG on
# the executor and the GL back end, which then apply the cap and present-mode rules under RECOMP_LSFG=1
# (src/framegen/framegen_policy.h).
get_property(_xbr_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
if(NOT WIN32 AND (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR OR "CXX" IN_LIST _xbr_languages))
    set(_xbr_lsfg_default ON)
else()
    set(_xbr_lsfg_default OFF)
endif()
option(XBOXRECOMP_LSFG "Build frame generation (LSFG pacer and bridge)" ${_xbr_lsfg_default})
if(XBOXRECOMP_LSFG AND NOT WIN32)
    # The bridge is C++20: CXX is enabled here, before any subdirectory, so that every directory below (the console
    # runtimes' executables link xbr_host, hence the bridge) links with the C++ driver.
    enable_language(CXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Option to build examples

# ── Android host ──────────────────────────────────────────────

# Android (NDK toolchain): SDL2 is built from pinned sources as a subproject
# instead of found on the system, as a shared libSDL2.so that SDLActivity
# loads. The Android toolchain image sets SDL2_SRC (docs/android/toolchain.md).
if(ANDROID AND NOT TARGET SDL2::SDL2)
    set(XBOXRECOMP_SDL2_SOURCE_DIR "$ENV{SDL2_SRC}" CACHE PATH
        "SDL2 sources for the Android build (release-2.32.10)")
    if(NOT EXISTS "${XBOXRECOMP_SDL2_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Android build: set XBOXRECOMP_SDL2_SOURCE_DIR (or SDL2_SRC) to the SDL2 sources")
    endif()
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory("${XBOXRECOMP_SDL2_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/SDL2" EXCLUDE_FROM_ALL)
endif()

# ── Runtime Libraries ─────────────────────────────────────────

# Platform: Win32/NT type vocabulary + POSIX implementation of generic
# Win32 host primitives. Carries no Xbox semantics; provides the foundation
# the kernel HLE and the graphics/audio/input layers build on.
add_subdirectory(src/platform)

# Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
add_subdirectory(src/kernel)

# Graphics: bundled D3D8 compatibility layer. Embedding projects that use the
# pushbuffer executor directly may omit it, or define their own xbox_d3d8
# target before adding xboxrecomp.
option(XBOXRECOMP_BUILD_D3D8 "Build the bundled xbox_d3d8 compatibility layer" ON)
if(NOT TARGET xbox_d3d8)
    if(XBOXRECOMP_BUILD_D3D8)
        add_subdirectory(src/d3d)
    elseif(WIN32)
        message(FATAL_ERROR
            "XBOXRECOMP_BUILD_D3D8=OFF on Windows requires a replacement xbox_d3d8 target")
    endif()
endif()

# Audio: DirectSound → software mixer compatibility
add_subdirectory(src/audio)

# Audio: MCPX APU hardware emulation (voice processor, DSP)
add_subdirectory(src/apu)

# GPU: NV2A hardware emulation (register handlers, MMIO, push buffer)
add_subdirectory(src/nv2a)

# USB: MCPX OHCI host controllers. A title's XAPI is statically linked and
# drives these registers directly, so a pad only exists if they do.
add_subdirectory(src/usb)

# Input: Xbox gamepad → XInput
add_subdirectory(src/input)

# The host layer (XBA-145, src/platform/HOST_LAYER.md): xbr_host, the console-agnostic window + GL present, pads,
# audio out, pacing, frame generation, storage, lifecycle and config every runtime links (the Xbox libraries and
# consoles/*). POSIX hosts.
if(NOT WIN32)
    add_subdirectory(src/host)
endif()

# Other consoles (XBA-146, consoles/README.md): each runtime links xbr_host only. ON by default on Linux and macOS,
# OFF on Android and Windows until each one is ready there. The GBA (XBA-157), the PS1 (XBA-168) and the N64
# (XBA-174) are ready on Android: the app plays them.
if(ANDROID OR WIN32)
    set(_xbr_console_default OFF)
else()
    set(_xbr_console_default ON)
endif()
if(WIN32)
    set(_xbr_gba_default OFF)
else()
    set(_xbr_gba_default ON)
endif()
option(XBR_CONSOLE_GBA "Build the Game Boy Advance runtime (consoles/gba)" ${_xbr_gba_default})
option(XBR_CONSOLE_PS1 "Build the PlayStation runtime (consoles/ps1)" ${_xbr_gba_default})
option(XBR_CONSOLE_N64 "Build the Nintendo 64 runtime (consoles/n64)" ${_xbr_gba_default})
# The Nintendo DS (XBA-223) is a spike: Linux / macOS headless only (nds_run), not in the app yet.
option(XBR_CONSOLE_NDS "Build the Nintendo DS spike (consoles/nds)" ${_xbr_console_default})
set(XBR_CONSOLES "")
foreach(_c gba ps1 n64 nds)
    string(TOUPPER ${_c} _C)
    if(XBR_CONSOLE_${_C})
        if(WIN32)
            message(FATAL_ERROR "XBR_CONSOLE_${_C}: the console runtimes are POSIX-only (xbr_host)")
        endif()
        add_subdirectory(consoles/${_c}/runtime)
        list(APPEND XBR_CONSOLES ${_c})
    endif()
endforeach()

# The GPU render back end (XBA-27): POSIX hosts, selected with RECOMP_GPU=1.
option(XBOXRECOMP_BUILD_GPU "Build the GL render back end for the pushbuffer executor" ON)
if(XBOXRECOMP_BUILD_GPU AND NOT WIN32)
    add_subdirectory(src/gpu)
endif()

# Shared FMV player (Media Foundation -> D3D8 texture)
add_subdirectory(src/video)

# The in-app self-test (XBA-51): the Android app shell runs it under
# RECOMP_SHELL_SELFTEST=1, tests/recomp_selftest on Linux. POSIX hosts.
if(NOT WIN32)
    add_subdirectory(src/selftest)
endif()

# Xbox disc image (XISO) extraction (XBA-40): the Android app extracts a picked
# disc image into the game folder. libc only, POSIX hosts.
if(NOT WIN32 AND NOT TARGET xiso)
    add_subdirectory(src/xiso)
endif()

# Frame generation (LSFG): option and CXX at the top (XBOXRECOMP_LSFG above); the libraries below.
if(XBOXRECOMP_LSFG AND NOT WIN32)
    add_subdirectory(src/framegen)
    target_compile_definitions(xbox_kernel PRIVATE XBOXRECOMP_LSFG=1)
    if(TARGET xbox_gpu)
        target_compile_definitions(xbox_gpu PRIVATE XBOXRECOMP_LSFG=1)
    endif()
    # The SDL presenter (fb_present_sdl.c) feeds the pacer and shows its frames; xbox_framegen carries
    # XBOXRECOMP_LSFG_BRIDGE=1 when the bridge is built.
    target_compile_definitions(xbox_video PRIVATE XBOXRECOMP_LSFG=1)
    target_link_libraries(xbox_video PUBLIC xbox_framegen)
    # The console window (src/video/host_window.c, XBA-186) does the same for the GBA, PS1 and N64 runtimes.
    if(TARGET xbr_host)
        target_compile_definitions(xbr_host PRIVATE XBOXRECOMP_LSFG=1)
        target_link_libraries(xbr_host PUBLIC xbox_framegen)
    endif()
endif()

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
set(XBOXRECOMP_LIBS xbox_kernel)
if(TARGET xbox_d3d8)
    list(APPEND XBOXRECOMP_LIBS xbox_d3d8)
endif()
list(APPEND XBOXRECOMP_LIBS
    xbox_dsound
    xbox_input
    xbox_usb
    xbox_apu
    xbox_nv2a
    xbox_video
)
if(TARGET xbox_gpu)
    list(APPEND XBOXRECOMP_LIBS xbox_gpu)
endif()
if(TARGET xbox_framegen)
    list(APPEND XBOXRECOMP_LIBS xbox_framegen)
endif()
add_library(xboxrecomp INTERFACE)
if(UNIX AND NOT APPLE)
    # The archives call each other both ways (the APU reaches the kernel's
    # memory map, the kernel raises the APU's interrupt). GNU ld scans each
    # archive once, so it needs the group; MSVC and ld64 rescan on their own.
    target_link_libraries(xboxrecomp INTERFACE
        -Wl,--start-group ${XBOXRECOMP_LIBS} -Wl,--end-group)
else()
    target_link_libraries(xboxrecomp INTERFACE ${XBOXRECOMP_LIBS})
endif()

# ── C regression tests ────────────────────────────────────────
#
# The tests under tests/ are self-contained CMake projects that can also be
# built alone; listing them here puts them under the root build's ctest, so
# one `cmake --build && ctest` covers them alongside whatever else a
# workspace builds. The Python side (lifter unit tests, conformance) stays
# with pytest -- see CONTRIBUTING.md.
# Off when a game pulls the toolkit in with add_subdirectory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_xbr_tests_default ON)
else()
    set(_xbr_tests_default OFF)
endif()
option(XBOXRECOMP_BUILD_TESTS "Build the C regression tests" ${_xbr_tests_default})
if(XBOXRECOMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/mmx_helpers)
    add_subdirectory(tests/xmm_helpers)
    foreach(_c IN LISTS XBR_CONSOLES)
        add_subdirectory(consoles/${_c}/tests)
    endforeach()
endif()

