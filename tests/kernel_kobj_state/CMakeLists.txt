cmake_minimum_required(VERSION 3.20)
project(kernel_kobj_state_test C)
# Kernel objects in a save state (XBA-246): events, semaphores, mutants,
# timers, ports, files, KE shadows, kernel timers and threads made through
# the thunk dispatcher against a real guest window (the synthetic XBE),
# serialised, torn down and made again under the same handle values.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX host")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_kobj_state_test test_main.c)
target_include_directories(kernel_kobj_state_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_kobj_state_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_kobj_state_test
         COMMAND kernel_kobj_state_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
set_tests_properties(kernel_kobj_state_test PROPERTIES TIMEOUT 120)
