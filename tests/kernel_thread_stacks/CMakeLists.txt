cmake_minimum_required(VERSION 3.20)
project(kernel_thread_stacks_test C)
# Worker thread stacks (XBA-104): PsCreateSystemThreadEx through the thunk
# dispatcher against a real guest window (xbox_MemoryLayoutInit on the
# synthetic XBE). 64 guest workers alive at once, then until the guest heap
# runs out; past the end a worker is refused, never run inline.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX host")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_thread_stacks_test test_main.c)
target_include_directories(kernel_thread_stacks_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_thread_stacks_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_thread_stacks_test
         COMMAND kernel_thread_stacks_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# The A/B cap at the old fixed pool: the 9th worker is refused.
add_test(NAME kernel_thread_stacks_cap8
         COMMAND kernel_thread_stacks_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
set_tests_properties(kernel_thread_stacks_cap8
                     PROPERTIES ENVIRONMENT RECOMP_THREAD_STACKS=8)
# A worker run inline on its creator would block the run for ever.
set_tests_properties(kernel_thread_stacks_test kernel_thread_stacks_cap8
                     PROPERTIES TIMEOUT 120)
