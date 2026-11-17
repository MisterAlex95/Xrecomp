cmake_minimum_required(VERSION 3.20)
project(ps2_runtime C)

# ps2_core: the PS2 spike (XBA-226, epic XBA-220): an EE (R5900) interpreter under lifted code, 32 MB of RAM, the
# scratchpad, the EE's SIO, latched IO registers, ELF loading, a minimal HLE of the EE kernel (start-up, threads,
# semaphores, SetSyscall, the SIF stubbed: no IOP). Plain C11, no host layer: Linux / macOS headless only for now
# (ps2_run), nothing in the app. See consoles/README.md « PS2 ».
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

if(NOT TARGET ps2_core)
    add_library(ps2_core STATIC ps2_core.c ps2_cpu.c ps2_bus.c ps2_load.c ps2_hle.c ps2_recomp.c)
    target_include_directories(ps2_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    # -ffp-contract=off: the FPU helpers (ps2_alu.h) compute in double; the interpreter and lifted code (built the
    # same way, run_hello.sh) must round every step alike.
    target_compile_options(ps2_core PRIVATE -Wall -Wextra -ffp-contract=off)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(ps2_core PRIVATE -O2)
    endif()
    target_link_libraries(ps2_core PUBLIC m)
    set_target_properties(ps2_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# A program that loads a pack exports the core: the lifted code binds to its bus functions at dlopen.
function(ps2_core_exported target)
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        target_link_options(${target} PRIVATE "LINKER:-force_load,$<TARGET_FILE:ps2_core>")
        # -force_load is a linker option only: the plain link also gives the core's include path and build order.
        target_link_libraries(${target} PRIVATE ps2_core)
    else()
        target_link_libraries(${target} PRIVATE -Wl,--whole-archive ps2_core -Wl,--no-whole-archive)
    endif()
    target_link_libraries(${target} PRIVATE m)
endfunction()

if(NOT ANDROID AND NOT TARGET ps2_run)
    add_executable(ps2_run tools/ps2_run.c)
    ps2_core_exported(ps2_run)
    target_link_libraries(ps2_run PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(ps2_run PRIVATE -Wall -Wextra)
endif()
