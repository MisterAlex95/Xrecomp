cmake_minimum_required(VERSION 3.20)
project(win32_thread_stack_posix_test C)
# The host stack floor of the POSIX CreateThread (XBA-49). Only the platform
# library is built: the floor is a property of the Win32 shim.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX thread shim")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../../src/platform platform)
add_executable(win32_thread_stack_posix_test test_main.c)
# libm for the shim's fenv calls; the full runtime pulls it in elsewhere.
target_link_libraries(win32_thread_stack_posix_test PRIVATE platform m)
enable_testing()
# Default floor: 8 MB.
add_test(NAME win32_thread_stack_posix_test COMMAND win32_thread_stack_posix_test)
set_tests_properties(win32_thread_stack_posix_test PROPERTIES
    ENVIRONMENT "RECOMP_THREAD_STACK_MB=")
# The override replaces the floor.
add_test(NAME win32_thread_stack_posix_override
         COMMAND win32_thread_stack_posix_test 16)
set_tests_properties(win32_thread_stack_posix_override PROPERTIES
    ENVIRONMENT "RECOMP_THREAD_STACK_MB=16")
