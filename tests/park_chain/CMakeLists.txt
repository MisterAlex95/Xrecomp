cmake_minimum_required(VERSION 3.20)
project(park_chain_test C)
# Xbox save states, part 3 (XBA-247): game-shaped guest workers written as the
# lifter emits code (ebp in a C local, the frame shadow stack pushed around
# every call) parked in KeDelayExecutionThread through the kernel bridge
# against a real guest window (the synthetic XBE); their call chains read
# from the frozen threads, checked against the call-site table, signed, and
# carried by the CPU section of a captured state.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX host")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(park_chain_test test_main.c)
target_include_directories(park_chain_test PRIVATE ../../src/kernel)
target_link_libraries(park_chain_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME park_chain_test
         COMMAND park_chain_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
add_test(NAME park_chain_guest_lock
         COMMAND park_chain_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
set_tests_properties(park_chain_guest_lock PROPERTIES
                     ENVIRONMENT "RECOMP_GUEST_LOCK=1;RECOMP_DISPATCH_GATE=1")
set_tests_properties(park_chain_test park_chain_guest_lock PROPERTIES TIMEOUT 120)
