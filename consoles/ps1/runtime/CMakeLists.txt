cmake_minimum_required(VERSION 3.20)
project(ps1_runtime C)

# ps1_runtime: the PlayStation runtime (MIPS R3000A, little-endian), XBA-146. Empty for now: one function returning its version, linked
# against the host layer (xbr_host, src/host) so the layering is in place before any code lands. See
# consoles/README.md and the Plane spec « Spec · Other consoles ». Built when XBR_CONSOLE_PS1 is ON (root
# CMakeLists.txt); a project may also add this directory alone (consoles/ps1/tests does).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ps1_gpu: the GPU (P-7, XBA-165, ps1_devices.h): plain C11, no host dependency, so tests build anywhere. ps1_gpu.c
# replaces the core's ps1_gpu_stub.c when it is present.
if(NOT TARGET ps1_gpu)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ps1_gpu.c)
        set(PS1_GPU_SRC ps1_gpu.c)
    else()
        set(PS1_GPU_SRC ps1_gpu_stub.c)
    endif()
    add_library(ps1_gpu STATIC ${PS1_GPU_SRC})
    target_include_directories(ps1_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(ps1_gpu PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(ps1_gpu PRIVATE -O2) # the software renderer is far too slow unoptimised
    endif()
    set_target_properties(ps1_gpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# PS1_CORE_ONLY=ON skips ps1_runtime (no xbr_host / SDL2), so a host without SDL2 can still build and test the
# devices (like GBA_CORE_ONLY).
option(PS1_CORE_ONLY "Build only the PS1 devices, not ps1_runtime (no xbr_host / SDL2)" OFF)
if(NOT PS1_CORE_ONLY AND NOT TARGET xbr_host)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
endif()
if(NOT PS1_CORE_ONLY AND NOT TARGET ps1_runtime)
    add_library(ps1_runtime STATIC ps1_runtime.c)
    target_include_directories(ps1_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ps1_runtime PUBLIC ps1_gpu xbr_host)
    target_compile_options(ps1_runtime PRIVATE -Wall -Wextra)
endif()
