cmake_minimum_required(VERSION 3.20)
project(gc_runtime C)

# gc_core: the GameCube machine (XBA-225 spike, epic XBA-219): the Gekko (PowerPC 750 interpreter + paired singles +
# lifted code), its memory map, the processor and video interfaces (NTSC / PAL timing), the DSP mailbox and ARAM
# DMA, EXI / AI that complete at once, the serial interface with standard controllers (XBA-259). Plain C11 + libm,
# no host layer: gc_run and the tests build anywhere. gc_runtime (XBA-259) plays it on the host layer: the Android
# app (gc_runtime_start) and gc_play. See consoles/README.md « GameCube ».
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

if(NOT TARGET gc_core)
    add_library(gc_core STATIC gc_core.c gc_cpu.c gc_bus.c gc_si.c gc_recomp.c)
    target_include_directories(gc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    # -ffp-contract=off: fmul then fadd round twice on the 750, so the interpreter and lifted code (built the same
    # way, run_hello.sh) must not fuse them (GCC does by default on arm64); fmadd and friends call fma() (XBA-255).
    target_compile_options(gc_core PRIVATE -Wall -Wextra -ffp-contract=off)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(gc_core PRIVATE -O2)
    endif()
    if(NOT APPLE)
        target_link_libraries(gc_core PUBLIC m)
    endif()
    set_target_properties(gc_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# A program that loads a pack exports the core: the lifted code binds to its bus functions at dlopen.
function(gc_core_exported target)
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        target_link_options(${target} PRIVATE "LINKER:-force_load,$<TARGET_FILE:gc_core>")
        target_link_libraries(${target} PRIVATE gc_core)
    else()
        target_link_libraries(${target} PRIVATE -Wl,--whole-archive gc_core -Wl,--no-whole-archive m)
    endif()
endfunction()

# gc_pad_map: a host pad to a GameCube controller and the DOL's title key (gc_runtime.h's pure helpers): libc and
# console_pad.h only, so the tests link it without SDL.
get_filename_component(GC_REPO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../src ABSOLUTE)
if(EXISTS ${GC_REPO_SRC}/input/mapping/console_pad.cmake AND NOT TARGET gc_pad_map)
    include(${GC_REPO_SRC}/input/mapping/console_pad.cmake)
    add_library(gc_pad_map STATIC gc_pad_map.c)
    target_include_directories(gc_pad_map PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${GC_REPO_SRC}/input)
    target_link_libraries(gc_pad_map PUBLIC gc_core console_pad)
    target_compile_options(gc_pad_map PRIVATE -Wall -Wextra)
    set_target_properties(gc_pad_map PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# gc_runtime: the core on the host layer (window with the XFB converted on the GPU, pads, pacing, the perf line, the
# ADPF hint; XBA-259). GC_CORE_ONLY=ON skips it (a host without SDL2 still builds and tests the core), as does a
# copy of consoles/gc without the rest of the repository (the homebrew CI job's container).
option(GC_CORE_ONLY "Build only gc_core and gc_run, not gc_runtime (no xbr_host / SDL2)" OFF)
if(NOT GC_CORE_ONLY AND NOT TARGET SDL2::SDL2)
    find_package(SDL2 QUIET)
    if(NOT TARGET SDL2::SDL2)
        message(STATUS "gc_runtime: no SDL2, the core alone (gc_core, gc_run)")
    endif()
endif()
if(NOT GC_CORE_ONLY AND TARGET SDL2::SDL2 AND TARGET gc_pad_map AND EXISTS ${GC_REPO_SRC}/host/CMakeLists.txt)
    if(NOT TARGET xbr_host)
        add_subdirectory(${GC_REPO_SRC}/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
    endif()
    if(NOT TARGET gc_runtime)
        add_library(gc_runtime STATIC gc_runtime.c gc_yuv_gl.c)
        target_include_directories(gc_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(gc_runtime PUBLIC gc_core gc_pad_map xbr_host ${CMAKE_DL_LIBS})
        target_compile_options(gc_runtime PRIVATE -Wall -Wextra)
        set_target_properties(gc_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

# gc_play: plays a DOL on a desktop through gc_runtime (window, pads).
if(NOT ANDROID AND TARGET gc_runtime AND NOT TARGET gc_play)
    add_executable(gc_play tools/gc_play.c)
    gc_core_exported(gc_play)   # it loads packs (--pack): the lifted code binds to the core's bus functions
    target_link_libraries(gc_play PRIVATE gc_runtime)
    target_compile_options(gc_play PRIVATE -Wall -Wextra)
endif()

if(NOT ANDROID AND NOT TARGET gc_run)
    add_executable(gc_run tools/gc_run.c)
    target_include_directories(gc_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    gc_core_exported(gc_run)
    target_link_libraries(gc_run PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(gc_run PRIVATE -Wall -Wextra)
endif()
