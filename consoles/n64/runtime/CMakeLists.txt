cmake_minimum_required(VERSION 3.20)
project(n64_runtime C)

# n64_runtime: the Nintendo 64 runtime (MIPS R4300i, big-endian, 64-bit), XBA-146. Empty for now: one function returning its version, linked
# against the host layer (xbr_host, src/host) so the layering is in place before any code lands. See
# consoles/README.md and the Plane spec « Spec · Other consoles ». Built when XBR_CONSOLE_N64 is ON (root
# CMakeLists.txt); a project may also add this directory alone (consoles/n64/tests does).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT TARGET xbr_host)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
endif()

if(NOT TARGET n64_runtime)
    add_library(n64_runtime STATIC n64_runtime.c)
    target_include_directories(n64_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(n64_runtime PUBLIC xbr_host)
    target_compile_options(n64_runtime PRIVATE -Wall -Wextra)
endif()
