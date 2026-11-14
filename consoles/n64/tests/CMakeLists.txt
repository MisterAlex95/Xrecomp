cmake_minimum_required(VERSION 3.20)
project(console_n64_tests C)
# The Nintendo 64 runtime's C tests (XBA-146): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_n64) and by the root build's ctest when XBR_CONSOLE_N64 is ON. POSIX hosts.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only (xbr_host)")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET n64_runtime AND NOT TARGET n64_rdp)
    add_subdirectory(../runtime n64_runtime)
endif()

enable_testing()
if(TARGET n64_runtime) # not with N64_CORE_ONLY=ON
    add_executable(n64_runtime_test test_main.c)
    target_link_libraries(n64_runtime_test PRIVATE n64_runtime)
    add_test(NAME n64_runtime COMMAND n64_runtime_test)
    set_tests_properties(n64_runtime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

# The RDP (N-5, XBA-173): feature tests on synthetic command lists (test_rdp.c); n64_rdp_bench, its benchmark, runs
# by hand (n64_rdp_bench [seconds per case]).
if(TARGET n64_rdp AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/n64_rdp.c)
    add_executable(n64_rdp_test test_rdp.c)
    target_link_libraries(n64_rdp_test PRIVATE n64_rdp m)
    target_compile_options(n64_rdp_test PRIVATE -Wall -Wextra)
    add_test(NAME n64_rdp COMMAND n64_rdp_test)
    set_tests_properties(n64_rdp PROPERTIES TIMEOUT 120)
    add_executable(n64_rdp_bench bench_rdp.c)
    target_link_libraries(n64_rdp_bench PRIVATE n64_rdp m)
    target_compile_options(n64_rdp_bench PRIVATE -Wall -Wextra -O2)
endif()
