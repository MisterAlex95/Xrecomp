cmake_minimum_required(VERSION 3.20)
project(nds_runtime C)

# nds_core: the Nintendo DS spike (XBA-223, epic XBA-217): the ARM9 (ARMv5TE interpreter + lifted code), its memory
# map, CP15, IRQs, timers, DMA, IPC with a stub ARM7, an HLE BIOS, the 2D engines' text backgrounds. Plain C11, no
# host layer: Linux headless only for now (nds_run), nothing in the app. See consoles/README.md « Nintendo DS ».
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

if(NOT TARGET nds_core)
    add_library(nds_core STATIC nds_core.c nds_cpu.c nds_bus.c nds_arm7.c nds_gpu.c nds_recomp.c)
    target_include_directories(nds_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(nds_core PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(nds_core PRIVATE -O2)
    endif()
    set_target_properties(nds_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# A program that loads a pack exports the core: the lifted code binds to its bus functions at dlopen.
function(nds_core_exported target)
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        target_link_options(${target} PRIVATE "LINKER:-force_load,$<TARGET_FILE:nds_core>")
    else()
        target_link_libraries(${target} PRIVATE -Wl,--whole-archive nds_core -Wl,--no-whole-archive)
    endif()
endfunction()

if(NOT ANDROID AND NOT TARGET nds_run)
    add_executable(nds_run tools/nds_run.c)
    nds_core_exported(nds_run)
    target_link_libraries(nds_run PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(nds_run PRIVATE -Wall -Wextra)
endif()
