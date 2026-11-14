cmake_minimum_required(VERSION 3.20)
project(console_ps1_tests C)
# The PlayStation runtime's C tests (XBA-146): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_ps1) and by the root build's ctest when XBR_CONSOLE_PS1 is ON. POSIX hosts.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only (xbr_host)")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET ps1_gpu)
    add_subdirectory(../runtime ps1_runtime)
endif()

enable_testing()
if(TARGET ps1_runtime)
    add_executable(ps1_runtime_test test_main.c)
    target_link_libraries(ps1_runtime_test PRIVATE ps1_runtime)
    add_test(NAME ps1_runtime COMMAND ps1_runtime_test)
    set_tests_properties(ps1_runtime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

# The GPU (P-7, XBA-165): feature tests on synthetic GP0 / GP1 streams; ps1_gpu_bench, its micro benchmark, runs by
# hand (ps1_gpu_bench [seconds]).
add_executable(ps1_gpu_test test_gpu.c)
target_link_libraries(ps1_gpu_test PRIVATE ps1_gpu)
target_compile_options(ps1_gpu_test PRIVATE -Wall -Wextra)
add_test(NAME ps1_gpu COMMAND ps1_gpu_test)
set_tests_properties(ps1_gpu PROPERTIES TIMEOUT 60)
add_executable(ps1_gpu_bench bench_gpu.c)
target_link_libraries(ps1_gpu_bench PRIVATE ps1_gpu)
target_compile_options(ps1_gpu_bench PRIVATE -Wall -Wextra -O2)
