cmake_minimum_required(VERSION 3.20)
project(kernel_io_log_test C)
# The per-call [READ] / [FILE] lines of the NtReadFile and NtCreateFile
# bridges are bounded (KRN-5). Cross-platform: the bridge seam, as
# tests/kernel_bridge uses it.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_io_log_test test_main.c)
target_include_directories(kernel_io_log_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_io_log_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_io_log_test COMMAND kernel_io_log_test)
add_test(NAME kernel_io_log_test_all COMMAND kernel_io_log_test)
set_tests_properties(kernel_io_log_test_all PROPERTIES ENVIRONMENT "RECOMP_READ_LOG=1")
