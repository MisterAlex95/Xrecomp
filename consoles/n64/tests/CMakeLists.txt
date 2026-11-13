cmake_minimum_required(VERSION 3.20)
project(console_n64_tests C)
# The Nintendo 64 runtime's C tests (XBA-146): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_n64) and by the root build's ctest when XBR_CONSOLE_N64 is ON. POSIX hosts.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only (xbr_host)")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET n64_runtime)
    add_subdirectory(../runtime n64_runtime)
endif()

add_executable(n64_runtime_test test_main.c)
target_link_libraries(n64_runtime_test PRIVATE n64_runtime)
enable_testing()
add_test(NAME n64_runtime COMMAND n64_runtime_test)
set_tests_properties(n64_runtime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
