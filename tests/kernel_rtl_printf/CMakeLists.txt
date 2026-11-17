cmake_minimum_required(VERSION 3.20)
project(kernel_rtl_printf_test C)
# RtlSprintf / RtlSnprintf / RtlVsprintf format the guest's x86 cdecl varargs: a double and an
# __int64 are two consecutive 4-byte slots (no 8-byte alignment), a double is taken by its bits.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_rtl_printf_test test_main.c)
target_include_directories(kernel_rtl_printf_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_rtl_printf_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_rtl_printf_test COMMAND kernel_rtl_printf_test)
set_tests_properties(kernel_rtl_printf_test PROPERTIES TIMEOUT 60)
