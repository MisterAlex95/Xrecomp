cmake_minimum_required(VERSION 3.20)
project(recomp_selftest_test C)
# The app shell's self-test (XBA-51, src/selftest/recomp_selftest.h) as a CMake test: the same checks the app runs
# under RECOMP_SHELL_SELFTEST=1, here on Linux (x86-64, arm64 under qemu-user) and from adb shell (device bundle),
# so that a failure in the app is an Android difference. The guest window is held as on Android.
if(WIN32)
    message(FATAL_ERROR "The self-test covers the POSIX runtime")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(recomp_selftest_test test_main.c)
target_link_libraries(recomp_selftest_test PRIVATE recomp_selftest xboxrecomp)
enable_testing()
set(_xbe ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# Every check, the SIMD load included.
add_test(NAME recomp_selftest COMMAND recomp_selftest_test ${_xbe} ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(recomp_selftest PROPERTIES ENVIRONMENT "RECOMP_GUEST_WINDOW=1")
# Without mmio_simd: under qemu-user a trapped ldr q fails (its fpsimd_context write-back, XBA-45), and the arm64 job
# lists recomp_selftest as an expected failure (tools/ci/arm64-expected.txt); this one keeps the rest gated there.
add_test(NAME recomp_selftest_no_simd COMMAND recomp_selftest_test ${_xbe} ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(recomp_selftest_no_simd PROPERTIES ENVIRONMENT "RECOMP_GUEST_WINDOW=1;RECOMP_SELFTEST_SKIP=mmio_simd")
