cmake_minimum_required(VERSION 3.20)
project(recomp_cfg_test C)
set(CMAKE_C_STANDARD 11)
# recomp_cfg.c only: no runtime to build.
add_executable(recomp_cfg_test test_main.c ../../src/platform/recomp_cfg.c)
target_include_directories(recomp_cfg_test PRIVATE ../../src/platform)
enable_testing()
add_test(NAME recomp_cfg COMMAND recomp_cfg_test)
