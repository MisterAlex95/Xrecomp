cmake_minimum_required(VERSION 3.20)
project(gc_runtime C)

# gc_core: the GameCube spike (XBA-225, epic XBA-219): the Gekko (PowerPC 750 interpreter + paired singles + lifted
# code), its memory map, the processor and video interfaces, the DSP mailbox and ARAM DMA, EXI / SI / AI that
# complete at once. Plain C11 + libm, no host layer: Linux / macOS headless only for now (gc_run), nothing in the app.
# See consoles/README.md « GameCube ».
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

if(NOT TARGET gc_core)
    add_library(gc_core STATIC gc_core.c gc_cpu.c gc_bus.c gc_recomp.c)
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

if(NOT ANDROID AND NOT TARGET gc_run)
    add_executable(gc_run tools/gc_run.c)
    target_include_directories(gc_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    gc_core_exported(gc_run)
    target_link_libraries(gc_run PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(gc_run PRIVATE -Wall -Wextra)
endif()
