cmake_minimum_required(VERSION 3.20)
project(guest_poll_wait_test C)
# RECOMP_POLL_WAIT's runtime side (xbox_GuestPollWait): the PFB flush kick
# waits for the ack thread, any other word returns at once, and
# RECOMP_POLL_WAIT=0 turns the wait off. Same code on every host.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(guest_poll_wait_test test_main.c)
target_include_directories(guest_poll_wait_test PRIVATE ../../src/kernel)
target_link_libraries(guest_poll_wait_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: the switch is read once and cached.
add_test(NAME guest_poll_wait_on COMMAND guest_poll_wait_test on ${XBE})
add_test(NAME guest_poll_wait_off COMMAND guest_poll_wait_test off ${XBE})
set_tests_properties(guest_poll_wait_off PROPERTIES ENVIRONMENT RECOMP_POLL_WAIT=0)
set_tests_properties(guest_poll_wait_on guest_poll_wait_off PROPERTIES TIMEOUT 60)
