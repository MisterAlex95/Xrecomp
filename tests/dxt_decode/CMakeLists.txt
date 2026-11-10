cmake_minimum_required(VERSION 3.20)
project(dxt_decode_test C)
# d3d8_dxt_decode_block against d3d8_dxt_decode_texel: header-only, no runtime.
set(CMAKE_C_STANDARD 11)
add_executable(dxt_decode_test test_main.c)
target_include_directories(dxt_decode_test PRIVATE ../../src/d3d)
enable_testing()
add_test(NAME dxt_decode_test COMMAND dxt_decode_test)
