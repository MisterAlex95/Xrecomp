cmake_minimum_required(VERSION 3.20)
project(ctr_runtime C)

# ctr_core: the Nintendo 3DS spike (XBA-228, epic XBA-222): one Horizon user process on the ARM11 (ARMv6K + VFPv2
# interpreter + lifted code), a flat guest address space, the 3DSX loader, the kernel calls and the services a libctru
# console program needs, all high-level. Plain C11 + libm, no host layer: Linux / macOS headless only (ctr_run),
# nothing in the app. See consoles/README.md « Nintendo 3DS ».
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
else()
    add_compile_definitions(_GNU_SOURCE)
endif()

if(NOT TARGET ctr_core)
    add_library(ctr_core STATIC ctr_core.c ctr_mem.c ctr_loader.c ctr_cpu.c ctr_kernel.c ctr_services.c ctr_recomp.c)
    target_include_directories(ctr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    # VMLA and friends round twice on the VFP: no fused multiply-add (lifted code is built the same way).
    target_compile_options(ctr_core PRIVATE -Wall -Wextra -ffp-contract=off)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(ctr_core PRIVATE -O2)
    endif()
    target_link_libraries(ctr_core PUBLIC m)
    set_target_properties(ctr_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(NOT ANDROID AND NOT TARGET ctr_run)
    add_executable(ctr_run tools/ctr_run.c)
    target_link_libraries(ctr_run PRIVATE ctr_core ${CMAKE_DL_LIBS})
    target_compile_options(ctr_run PRIVATE -Wall -Wextra)
endif()
