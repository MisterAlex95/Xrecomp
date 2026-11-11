cmake_minimum_required(VERSION 3.20)
project(kernel_reboot_test C)
# Quick reboot (kernel_reboot.c): the launch data file round trip, the
# launch-path match and the restart cap. Cross-platform.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_reboot_test test_main.c)
target_include_directories(kernel_reboot_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_reboot_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_reboot_test COMMAND kernel_reboot_test)
