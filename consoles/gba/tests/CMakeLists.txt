cmake_minimum_required(VERSION 3.20)
project(console_gba_tests C)
# The Game Boy Advance runtime's C tests (XBA-146): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_gba) and by the root build's ctest when XBR_CONSOLE_GBA is ON. POSIX hosts.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only (xbr_host)")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET gba_runtime)
    add_subdirectory(../runtime gba_runtime)
endif()

add_executable(gba_runtime_test test_main.c)
target_link_libraries(gba_runtime_test PRIVATE gba_runtime)
enable_testing()
add_test(NAME gba_runtime COMMAND gba_runtime_test)
set_tests_properties(gba_runtime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# The PPU (XBA-154): feature tests on synthetic scenes and a micro benchmark (run by hand: gba_ppu_bench [seconds]).
add_executable(gba_ppu_test test_ppu.c)
target_link_libraries(gba_ppu_test PRIVATE gba_runtime)
target_compile_options(gba_ppu_test PRIVATE -Wall -Wextra)
add_test(NAME gba_ppu COMMAND gba_ppu_test)
set_tests_properties(gba_ppu PROPERTIES TIMEOUT 60)
add_executable(gba_ppu_bench bench_ppu.c)
target_link_libraries(gba_ppu_bench PRIVATE gba_runtime)
target_compile_options(gba_ppu_bench PRIVATE -Wall -Wextra)
