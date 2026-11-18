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
        x360_xam.c x360_gpu.c xenos_cp.c x360_state.c
        x360_crypt.c x360_more.c x360_input.c x360_xma.c xenos_gl.c x360_seh.c)
    target_include_directories(x360_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${X360_GC_RUNTIME})
    target_compile_definitions(x360_core PUBLIC XE_RUNTIME=1 _GNU_SOURCE)
    target_compile_options(x360_core PRIVATE -Wall -Wextra -ffp-contract=off)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(x360_core PRIVATE -O2)
    endif()
    find_package(Threads REQUIRED) # XBA-283: x360_set_host_threads
    target_link_libraries(x360_core PUBLIC xiso m ${CMAKE_DL_LIBS} Threads::Threads)
    set_target_properties(x360_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# x360_pad_map: x360_runtime.h's pure helpers (title key, a host pad as a 360 controller): console_pad only, so the
# tests link it without SDL. x360_runtime (XBA-280): the machine on the host layer (window, pads, audio, states,
# rewind, cheats, the perf line), like gc_runtime; x360_play plays a title on a desktop. X360_CORE_ONLY=ON skips them.
get_filename_component(X360_REPO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../src ABSOLUTE)
if(EXISTS ${X360_REPO_SRC}/input/mapping/console_pad.cmake AND NOT TARGET x360_pad_map)
    include(${X360_REPO_SRC}/input/mapping/console_pad.cmake)
    add_library(x360_pad_map STATIC x360_pad_map.c)
    target_include_directories(x360_pad_map PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${X360_REPO_SRC}/input)
    target_link_libraries(x360_pad_map PUBLIC x360_core console_pad)
    target_compile_options(x360_pad_map PRIVATE -Wall -Wextra)
    set_target_properties(x360_pad_map PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
option(X360_CORE_ONLY "Build only x360_core and x360_run, not x360_runtime (no xbr_host / SDL2)" OFF)
if(NOT X360_CORE_ONLY AND NOT TARGET SDL2::SDL2)
    find_package(SDL2 QUIET)
endif()
if(NOT X360_CORE_ONLY AND TARGET SDL2::SDL2 AND TARGET x360_pad_map AND EXISTS ${X360_REPO_SRC}/host/CMakeLists.txt)
    if(NOT TARGET xbr_host)
        add_subdirectory(${X360_REPO_SRC}/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
    endif()
    if(NOT TARGET x360_runtime)
        add_library(x360_runtime STATIC x360_runtime.c)
        target_include_directories(x360_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(x360_runtime PUBLIC x360_core x360_pad_map xbr_host ${CMAKE_DL_LIBS})
        target_compile_options(x360_runtime PRIVATE -Wall -Wextra)
        set_target_properties(x360_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()
if(NOT ANDROID AND TARGET x360_runtime AND NOT TARGET x360_play)
    add_executable(x360_play tools/x360_play.c)
    target_link_libraries(x360_play PRIVATE x360_runtime)
    target_compile_options(x360_play PRIVATE -Wall -Wextra)
endif()

if(NOT ANDROID AND NOT TARGET x360_run)
    add_executable(x360_run tools/x360_run.c)
    target_link_libraries(x360_run PRIVATE x360_core ${CMAKE_DL_LIBS})
    target_compile_options(x360_run PRIVATE -Wall -Wextra)
endif()
