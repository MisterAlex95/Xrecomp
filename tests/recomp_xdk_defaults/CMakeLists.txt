cmake_minimum_required(VERSION 3.20)
project(recomp_xdk_defaults_test C)
set(CMAKE_C_STANDARD 11)
# recomp_xdk_defaults.c only (XBA-106): no dependency on the rest of the runtime.
add_executable(recomp_xdk_defaults_test test_main.c ../../src/kernel/recomp_xdk_defaults.c)
target_include_directories(recomp_xdk_defaults_test PRIVATE ../../src/kernel)
enable_testing()
add_test(NAME recomp_xdk_defaults COMMAND recomp_xdk_defaults_test)
