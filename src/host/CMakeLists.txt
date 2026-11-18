cmake_minimum_required(VERSION 3.20)
project(xbr_host C)

# xbr_host: the console-agnostic host layer every runtime links (XBA-145, src/platform/HOST_LAYER.md): window + GL
# present, pads, audio out, pacing and frame generation, storage, lifecycle, config, host time / cpus, logging.
# It builds from src/platform, src/video, src/input, src/audio and src/framegen without any Xbox source (no XBE,
# NV2A, kernel, win32_compat or MMIO decode): this directory only defines the console-agnostic targets, so a
# project that adds it alone (tests/host_layer, consoles/*) compiles nothing else.
#
#   xbr_host_core   libc + pthreads: log pump, lifecycle, quit (src/platform/host_core.cmake), with host_time,
#                   host_cpus, recomp_cfg, recomp_exit_hook
#   recomp_storage  data and save directories (recomp_storage_resolve_entry)
#   input_ports     which controller is which port (src/input/mapping/input_ports.cmake)
#   console_pad     a host pad to a console's pad, the player's mapping (src/input/mapping/console_pad.cmake)
#   host_framegen   frame generation pacer + fake generator (src/framegen/host_framegen.cmake)
#   xbr_host        the SDL parts: host_window (video), host_pad (input), host_audio (audio), perf_overlay,
#                   the Android display clock (display_vsync_android.c)
#
# POSIX hosts (Linux, macOS, Android). Added by the root CMakeLists.txt after src/input; a project may also add
# this directory alone.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Windows (XBA-278): built for the console runtimes there (MinGW-w64; the POSIX calls through xbr_posix_win32,
# src/platform/host_core.cmake), never for the Xbox Windows build.

get_filename_component(XBR_HOST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

include(${XBR_HOST_SRC}/platform/host_core.cmake)
include(${XBR_HOST_SRC}/input/mapping/input_ports.cmake)
include(${XBR_HOST_SRC}/input/mapping/console_pad.cmake)
include(${XBR_HOST_SRC}/framegen/host_framegen.cmake)

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 REQUIRED)
endif()

if(NOT TARGET xbr_host)
    add_library(xbr_host STATIC
        ${XBR_HOST_SRC}/video/host_window.c
        ${XBR_HOST_SRC}/video/present_shaders.c
        ${XBR_HOST_SRC}/video/perf_overlay.c
        ${XBR_HOST_SRC}/input/host_pad.c
        ${XBR_HOST_SRC}/audio/host_audio.c)
    if(ANDROID)
        # The display's refresh grid from AChoreographer (XBA-136); its sink is the caller's.
        target_sources(xbr_host PRIVATE ${XBR_HOST_SRC}/video/display_vsync_android.c)
        target_link_libraries(xbr_host PUBLIC android ${CMAKE_DL_LIBS})
    endif()
    # The host headers: host_window.h, perf_overlay.h, display_clock.h, frame_times.h (video); host_pad.h,
    # input_ports.h (input); host_audio.h (audio); the platform ones come with xbr_host_core.
    target_include_directories(xbr_host PUBLIC
        ${XBR_HOST_SRC}/video
        ${XBR_HOST_SRC}/input
        ${XBR_HOST_SRC}/audio)
    target_link_libraries(xbr_host PUBLIC xbr_host_core recomp_storage input_ports console_pad host_framegen SDL2::SDL2)
    if(WIN32)
        # XBA-278: the players are console programs with their own main() (no SDL2main: SDL.h leaves main alone with
        # SDL_MAIN_HANDLED, and SDL_Init needs no SDL_SetMainReady on Windows).
        target_compile_definitions(xbr_host PUBLIC SDL_MAIN_HANDLED)
        target_link_options(xbr_host INTERFACE "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-mconsole>")
    endif()
    set_target_properties(xbr_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(xbr_host PRIVATE -Wall -Wextra -O2)
endif()
