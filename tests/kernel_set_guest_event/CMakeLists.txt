cmake_minimum_required(VERSION 3.20)
project(kernel_set_guest_event_test C)
# xbox_KeSetGuestEvent from a host thread releases a guest thread blocked in
# KeWaitForSingleObject on a KEVENT the title built itself. POSIX: the waiter
# is a pthread.
if(WIN32)
    message(FATAL_ERROR "This regression uses pthreads")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_set_guest_event_test test_main.c)
target_include_directories(kernel_set_guest_event_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_set_guest_event_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: RECOMP_TITLE_KEVENTS is read once and cached.
add_test(NAME kernel_set_guest_event_default COMMAND kernel_set_guest_event_test default ${XBE})
add_test(NAME kernel_set_guest_event_title_kevents COMMAND kernel_set_guest_event_test title-kevents ${XBE})
set_tests_properties(kernel_set_guest_event_default kernel_set_guest_event_title_kevents
                     PROPERTIES TIMEOUT 30)
