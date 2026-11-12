cmake_minimum_required(VERSION 3.20)
project(kernel_wait_stats_test C)
# Cross-platform: the accounting is called directly, no guest code.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_wait_stats_test test_main.c)
target_include_directories(kernel_wait_stats_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_wait_stats_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_wait_stats COMMAND kernel_wait_stats_test)
# RECOMP_WAIT_REPORT=0: nothing counted, no line.
add_test(NAME kernel_wait_stats_off COMMAND kernel_wait_stats_test)
set_tests_properties(kernel_wait_stats_off PROPERTIES ENVIRONMENT "RECOMP_WAIT_REPORT=0")
