cmake_minimum_required(VERSION 3.20)
project(ps1_runtime C)

# ps1_runtime: the PlayStation runtime (MIPS R3000A, little-endian), XBA-146. Empty for now: one function returning its version, linked
# against the host layer (xbr_host, src/host) so the layering is in place before any code lands. See
# consoles/README.md and the Plane spec « Spec · Other consoles ». Built when XBR_CONSOLE_PS1 is ON (root
# CMakeLists.txt); a project may also add this directory alone (consoles/ps1/tests does).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT TARGET xbr_host)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
endif()

if(NOT TARGET ps1_runtime)
    add_library(ps1_runtime STATIC ps1_runtime.c)
    target_include_directories(ps1_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ps1_runtime PUBLIC xbr_host)
    target_compile_options(ps1_runtime PRIVATE -Wall -Wextra)
endif()
