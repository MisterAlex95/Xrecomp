cmake_minimum_required(VERSION 3.20)
project(kernel_io_apc_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_io_apc_test test_main.c)
target_include_directories(kernel_io_apc_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_io_apc_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_io_apc_test COMMAND kernel_io_apc_test)
# The A/B switch: the old inline delivery.
add_test(NAME kernel_io_apc_inline_test COMMAND kernel_io_apc_test)
set_tests_properties(kernel_io_apc_inline_test PROPERTIES ENVIRONMENT "RECOMP_INLINE_APC=1")
# The A/B switch of XBA-133: an APC queued from another thread does not end
# an alertable wait, it runs at the next one.
add_test(NAME kernel_io_apc_nowake_test COMMAND kernel_io_apc_test)
set_tests_properties(kernel_io_apc_nowake_test PROPERTIES ENVIRONMENT "RECOMP_APC_WAKE=0")
