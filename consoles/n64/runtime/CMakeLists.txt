cmake_minimum_required(VERSION 3.20)
project(n64_runtime C)

# n64_runtime: the Nintendo 64 runtime (MIPS R4300i, big-endian, 64-bit), XBA-146. Empty for now: one function returning its version, linked
# against the host layer (xbr_host, src/host) so the layering is in place before any code lands. See
# consoles/README.md and the Plane spec « Spec · Other consoles ». Built when XBR_CONSOLE_N64 is ON (root
# CMakeLists.txt); a project may also add this directory alone (consoles/n64/tests does).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# N64_CORE_ONLY=ON skips n64_runtime (no xbr_host / SDL2), so a host without SDL2 can still build and test the devices
# (like PS1_CORE_ONLY).
option(N64_CORE_ONLY "Build only the N64 devices, not n64_runtime (no xbr_host / SDL2)" OFF)
if(NOT N64_CORE_ONLY AND NOT TARGET xbr_host)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
endif()

if(NOT N64_CORE_ONLY AND NOT TARGET n64_runtime)
    add_library(n64_runtime STATIC n64_runtime.c)
    target_include_directories(n64_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(n64_runtime PUBLIC xbr_host)
    target_compile_options(n64_runtime PRIVATE -Wall -Wextra)
endif()

# n64_rdp: the RDP (N-5, XBA-173, n64_devices.h + n64_rdp.h): plain C11 (+ pthreads for the optional scanline bands),
# no host dependency, so tests build anywhere. n64_rdp.c replaces the core's n64_rdp_stub.c when it is present.
if(NOT TARGET n64_rdp AND (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rdp.c OR EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rdp_stub.c))
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rdp.c)
        set(N64_RDP_SRC n64_rdp.c)
    else()
        set(N64_RDP_SRC n64_rdp_stub.c)
    endif()
    add_library(n64_rdp STATIC ${N64_RDP_SRC})
    target_include_directories(n64_rdp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(n64_rdp PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(n64_rdp PRIVATE -O2) # the software rasterizer is far too slow unoptimised
    endif()
    # Auto-vectorised 4-channel code reads back scalar stores as vectors (store-forwarding stalls): 1.6x slower.
    target_compile_options(n64_rdp PRIVATE $<$<C_COMPILER_ID:GNU>:-fno-tree-vectorize>
                           $<$<C_COMPILER_ID:Clang,AppleClang>:-fno-slp-vectorize>)
    set_target_properties(n64_rdp PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_package(Threads)
    if(Threads_FOUND)
        target_link_libraries(n64_rdp PUBLIC Threads::Threads)
    endif()
endif()
