cmake_minimum_required(VERSION 3.20)
project(recomp_cheats_test C)
# Cheats of the console runtimes, the shared engine (XBA-197, src/platform/recomp_cheats.h): every code format on a
# fake memory (GBA raw, GameShark / AR v1-v2 and AR v3 encrypted and decrypted, CodeBreaker; PS1 and N64 GameShark;
# the Xbox format with its refused addresses, XBA-241),
# the GSA / AR v3 encryption (round trips, fixed vectors, the seeds), the format detection, conditionals and blocks,
# once-codes, cartridge patches put in and back, writes only when the value differs, the file format and its
# reload. The consoles run cheats on their homebrew (gba_cheats_hb_*, ps1_cheats_hb_*, n64_cheats_hb_*). libc only.
set(CMAKE_C_STANDARD 11)

add_executable(recomp_cheats_test test_main.c ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform/recomp_cheats.c)
target_include_directories(recomp_cheats_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform)
target_compile_options(recomp_cheats_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME recomp_cheats COMMAND recomp_cheats_test ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(recomp_cheats PROPERTIES TIMEOUT 60)
