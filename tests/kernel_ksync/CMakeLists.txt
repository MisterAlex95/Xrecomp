cmake_minimum_required(VERSION 3.20)
project(kernel_ksync_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_ksync_test test_main.c)
target_include_directories(kernel_ksync_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_ksync_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_ksync_test COMMAND kernel_ksync_test)
# PsCreateSystemThread(Ex) ThreadId == the new thread's UniqueThread, against
# a real guest window (xbox_MemoryLayoutInit on the synthetic XBE), as in
# tests/kernel_thread_stacks. POSIX host (mkdtemp, usleep).
if(NOT WIN32)
    add_executable(kernel_ksync_thread_id test_thread_id.c)
    target_include_directories(kernel_ksync_thread_id PRIVATE ../../src/kernel)
    target_link_libraries(kernel_ksync_thread_id PRIVATE xboxrecomp)
    add_test(NAME kernel_ksync_thread_id
             COMMAND kernel_ksync_thread_id
                     ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
    set_tests_properties(kernel_ksync_thread_id PROPERTIES TIMEOUT 120)
endif()
# The A/B switch of XBA-133: no concurrency limit on KQUEUEs.
add_test(NAME kernel_ksync_nolimit_test COMMAND kernel_ksync_test)
set_tests_properties(kernel_ksync_nolimit_test PROPERTIES ENVIRONMENT "RECOMP_QUEUE_LIMIT=0")
