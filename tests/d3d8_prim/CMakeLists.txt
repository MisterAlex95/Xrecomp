cmake_minimum_required(VERSION 3.20)
project(d3d8_prim_test C)
# d3d8_fan_quad_indices (src/d3d/d3d8_prim.h): header-only, no runtime, every host.
set(CMAKE_C_STANDARD 11)
add_executable(d3d8_prim_test test_main.c)
target_include_directories(d3d8_prim_test PRIVATE ../../src/d3d)
enable_testing()
add_test(NAME d3d8_prim_test COMMAND d3d8_prim_test)
