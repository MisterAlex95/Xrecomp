cmake_minimum_required(VERSION 3.20)
project(dc_runtime C)

# dc_core: the Dreamcast spike (XBA-227, epic XBA-221): the SH-4 (interpreter + lifted code, sh4_ops.h shared by
# both), the memory map, a BIOS in C, the SH-4's TMU / INTC / SCIF / store queues, Holly's interrupts and the
# PowerVR2 framebuffer read-out; dc_naomi.c the Naomi spike (XBA-235, off unless asked for: dc_run --naomi).
# Plain C11, no host layer: Linux headless only for now (dc_run), nothing in the app.
# See consoles/README.md « Dreamcast » and docs/technical/naomi.md.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

if(NOT TARGET dc_core)
    add_library(dc_core STATIC dc_core.c dc_bus.c dc_hw.c dc_bios.c dc_naomi.c sh4_interp.c)
    target_include_directories(dc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    # -ffp-contract=off: FMUL then FADD round twice, so the interpreter and lifted code (built the same way,
    # run_hello.sh) must not fuse them (GCC does by default on arm64); FMAC / FIPR / FTRV too (XBA-255).
    target_compile_options(dc_core PRIVATE -Wall -Wextra -ffp-contract=off)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(dc_core PRIVATE -O2)
    endif()
    find_library(DC_LIBM m)
    if(DC_LIBM)
        target_link_libraries(dc_core PUBLIC ${DC_LIBM})
    endif()
    set_target_properties(dc_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# A program that loads a pack exports the core: the lifted code binds to its bus functions at dlopen.
function(dc_core_exported target)
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        target_link_options(${target} PRIVATE "LINKER:-force_load,$<TARGET_FILE:dc_core>")
    else()
        target_link_libraries(${target} PRIVATE -Wl,--whole-archive dc_core -Wl,--no-whole-archive)
    endif()
endfunction()

if(NOT ANDROID AND NOT TARGET dc_run)
    add_executable(dc_run tools/dc_run.c)
    dc_core_exported(dc_run)
    target_link_libraries(dc_run PRIVATE ${CMAKE_DL_LIBS} ${DC_LIBM})
    target_compile_options(dc_run PRIVATE -Wall -Wextra)
endif()
