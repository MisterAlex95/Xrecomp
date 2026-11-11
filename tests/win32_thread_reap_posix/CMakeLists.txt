cmake_minimum_required(VERSION 3.20)
project(win32_thread_reap_posix_test C)
# Ended CreateThread threads give their stack back (XBA-56). Only the platform
# library: the thread shim is all this covers.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX thread shim")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../../src/platform platform)
add_executable(win32_thread_reap_posix_test test_main.c)
# libm for the shim's fenv calls; the full runtime pulls it in elsewhere.
target_link_libraries(win32_thread_reap_posix_test PRIVATE platform m)
enable_testing()
add_test(NAME win32_thread_reap_posix_test COMMAND win32_thread_reap_posix_test)
# The default 8 MB floor: 2000 kept stacks would be 16 GB of address space.
set_tests_properties(win32_thread_reap_posix_test PROPERTIES
    ENVIRONMENT "RECOMP_THREAD_STACK_MB=")
