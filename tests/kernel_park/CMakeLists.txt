cmake_minimum_required(VERSION 3.20)
project(kernel_park_test C)
# The safe-point park detector (XBA-244): guest workers through the thunk
# dispatcher against a real guest window (xbox_MemoryLayoutInit on the
# synthetic XBE), parked in their kernel calls, frozen and thawed; the
# all-parked share and the guest pages a snapshot would carry, measured.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX host")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_park_test test_main.c)
target_include_directories(kernel_park_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_park_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_park_test
         COMMAND kernel_park_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# The same with the guest CPU lock: a freeze must not deadlock against it.
add_test(NAME kernel_park_guest_lock
         COMMAND kernel_park_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
set_tests_properties(kernel_park_guest_lock PROPERTIES ENVIRONMENT RECOMP_GUEST_LOCK=1)
set_tests_properties(kernel_park_test kernel_park_guest_lock PROPERTIES TIMEOUT 120)
