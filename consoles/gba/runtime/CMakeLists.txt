cmake_minimum_required(VERSION 3.20)
project(gba_runtime C)

# gba_runtime: the Game Boy Advance runtime (ARM7TDMI (ARM + Thumb), little-endian), XBA-146. Empty for now: one function returning its version, linked
# against the host layer (xbr_host, src/host) so the layering is in place before any code lands. See
# consoles/README.md and the Plane spec « Spec · Other consoles ». Built when XBR_CONSOLE_GBA is ON (root
# CMakeLists.txt); a project may also add this directory alone (consoles/gba/tests does).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT TARGET xbr_host)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
endif()

if(NOT TARGET gba_runtime)
    add_library(gba_runtime STATIC gba_runtime.c)
    target_include_directories(gba_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(gba_runtime PUBLIC xbr_host)
    target_compile_options(gba_runtime PRIVATE -Wall -Wextra)
endif()
