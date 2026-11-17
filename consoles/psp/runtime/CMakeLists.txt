cmake_minimum_required(VERSION 3.20)
project(psp_runtime C)

# psp_core: the PSP spike (XBA-224, epic XBA-218): an Allegrex interpreter (no VFPU) under lifted code, the PSP's
# memory map, PRX / EBOOT loading with relocations and NID import stubs, a minimal HLE kernel (threads, semaphores,
# memory, sceDisplay, sceCtrl, stdio). Plain C11, no host layer: Linux / macOS headless only for now (psp_run),
# nothing in the app. See consoles/README.md « PSP ».
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

if(NOT TARGET psp_core)
    add_library(psp_core STATIC psp_core.c psp_cpu.c psp_bus.c psp_load.c psp_hle.c psp_recomp.c)
    target_include_directories(psp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    # -ffp-contract=off: the interpreter and lifted code (built the same way, run_hello.sh) round every FPU step
    # alike (psp_alu.h).
    target_compile_options(psp_core PRIVATE -Wall -Wextra -ffp-contract=off)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(psp_core PRIVATE -O2)
    endif()
    target_link_libraries(psp_core PUBLIC m)
    set_target_properties(psp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# A program that loads a pack exports the core: the lifted code binds to its bus functions at dlopen.
function(psp_core_exported target)
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        target_link_options(${target} PRIVATE "LINKER:-force_load,$<TARGET_FILE:psp_core>")
        # -force_load is a linker option only: the plain link also gives the core's include path and build order.
        target_link_libraries(${target} PRIVATE psp_core)
    else()
        target_link_libraries(${target} PRIVATE -Wl,--whole-archive psp_core -Wl,--no-whole-archive)
    endif()
    target_link_libraries(${target} PRIVATE m)
endfunction()

if(NOT ANDROID AND NOT TARGET psp_run)
    add_executable(psp_run tools/psp_run.c)
    psp_core_exported(psp_run)
    target_link_libraries(psp_run PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(psp_run PRIVATE -Wall -Wextra)
endif()
