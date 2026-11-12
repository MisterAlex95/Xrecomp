cmake_minimum_required(VERSION 3.20)
project(guest_cpu_ops_test C)
# The runtime side of lifted instructions that call into it: the divide
# error (recomp_divide_error) and cli/sti (the interrupt flag).
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(guest_cpu_ops_test test_main.c)
target_include_directories(guest_cpu_ops_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(guest_cpu_ops_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME guest_cpu_ops_test COMMAND guest_cpu_ops_test)
set_tests_properties(guest_cpu_ops_test PROPERTIES SKIP_RETURN_CODE 77)
