cmake_minimum_required(VERSION 3.20)
project(gba_runtime C)

# gba_runtime: the Game Boy Advance runtime (ARM7TDMI (ARM + Thumb), little-endian), XBA-146. The core (XBA-152
# interpreter, XBA-153 bus / timers / DMA / IRQ / keypad / BIOS) is gba_core.h; the PPU is gba_ppu.h. Linked against
# the host layer (xbr_host, src/host). See consoles/README.md and the Plane spec « Spec · Other consoles ». Built when
# XBR_CONSOLE_GBA is ON (root CMakeLists.txt); a project may also add this directory alone (consoles/gba/tests does).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# gba_core: the emulated machine alone (CPU, bus, IO, BIOS, and the PPU gba_ppu.c of XBA-154), plain C11 + libm, no
# host dependency, so the tests and gba_run build anywhere (no SDL).
if(NOT TARGET gba_core)
    add_library(gba_core STATIC gba_core.c gba_cpu.c gba_bus.c gba_io.c gba_bios.c gba_ppu.c gba_audio.c gba_save.c)
    target_include_directories(gba_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(gba_core PUBLIC m) # sin / pow for the HLE BIOS
    target_compile_options(gba_core PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(gba_core PRIVATE -O2) # the interpreter is far too slow unoptimised
    endif()
    set_target_properties(gba_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# gba_runtime: the core + the host layer (gba_runtime_run plays a game: window, pads, audio, saves, pacing; XBA-157).
# GBA_CORE_ONLY=ON skips it (a host without SDL2 can still build and test the
# core).
option(GBA_CORE_ONLY "Build only gba_core and gba_run, not gba_runtime (no xbr_host / SDL2)" OFF)
if(NOT GBA_CORE_ONLY)
    if(NOT TARGET xbr_host)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
    endif()
    if(NOT TARGET gba_runtime)
        add_library(gba_runtime STATIC gba_runtime.c)
        target_include_directories(gba_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(gba_runtime PUBLIC gba_core xbr_host)
        target_compile_options(gba_runtime PRIVATE -Wall -Wextra)
    endif()
endif()

# gba_play: plays a ROM on a desktop through gba_runtime (window, pads, audio, saves; XBA-157).
if(NOT ANDROID AND TARGET gba_runtime AND NOT TARGET gba_play)
    add_executable(gba_play tools/gba_play.c)
    target_link_libraries(gba_play PRIVATE gba_runtime)
    target_compile_options(gba_play PRIVATE -Wall -Wextra)
endif()

# gba_run: the headless runner (desktop hosts only).
if(NOT ANDROID AND NOT TARGET gba_run)
    add_executable(gba_run tools/gba_run.c)
    target_link_libraries(gba_run PRIVATE gba_core)
    target_compile_options(gba_run PRIVATE -Wall -Wextra)
endif()
