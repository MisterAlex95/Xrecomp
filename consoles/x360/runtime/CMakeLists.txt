cmake_minimum_required(VERSION 3.20)
project(x360_runtime C)

# x360_core: the Xbox 360 runtime (XBA-270, epic XBA-229): a 4 GB guest address space with the physical mirrors, the
# XEX2 loader and import thunks, cooperative guest threads, a minimal xboxkrnl / xam HLE, the small interpreter
# behind RC_INTERP. The title's code is a pack (x360_tools.recomp's C, built with -DXE_RUNTIME). Plain C11, no host
# layer yet: Linux headless only (x360_run: memfd_create for the physical mirrors), nothing in the app. See
# consoles/README.md « Xbox 360 ».
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(X360_GC_RUNTIME ${CMAKE_CURRENT_SOURCE_DIR}/../../gc/runtime) # ppc_ops.h, shared with the GameCube

if(NOT TARGET x360_core)
    if(NOT TARGET xiso)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/xiso xiso) # disc images (XDVDFS / GDFX)
    endif()
    add_library(x360_core STATIC x360_mem.c x360_load.c x360_thread.c x360_kernel.c x360_interp.c x360_file.c
        x360_xam.c)
    target_include_directories(x360_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${X360_GC_RUNTIME})
    target_compile_definitions(x360_core PUBLIC XE_RUNTIME=1 _GNU_SOURCE)
    target_compile_options(x360_core PRIVATE -Wall -Wextra -ffp-contract=off)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(x360_core PRIVATE -O2)
    endif()
    target_link_libraries(x360_core PUBLIC xiso m)
    set_target_properties(x360_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(NOT ANDROID AND NOT TARGET x360_run)
    add_executable(x360_run tools/x360_run.c)
    target_link_libraries(x360_run PRIVATE x360_core ${CMAKE_DL_LIBS})
    target_compile_options(x360_run PRIVATE -Wall -Wextra)
endif()
