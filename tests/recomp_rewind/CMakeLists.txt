cmake_minimum_required(VERSION 3.20)
project(recomp_rewind_test C)
# Rewind of the console runtimes, the shared part (XBA-194, src/platform/recomp_rewind.h): the XOR-delta codec (sizes
# that change, corrupt deltas refused), the ring (push / pop round trips, the memory cap, the oldest dropped), the
# settings (RECOMP_REWIND, _MB, _FRAMES per console), the runtimes' capture schedule and steps, and the round trip
# check on a fake machine. The consoles run the same check on their homebrew (gba_rewind_hb_*, ps1_rewind_hb_*,
# n64_rewind_hb_*). libc only.
set(CMAKE_C_STANDARD 11)

add_executable(recomp_rewind_test test_main.c ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform/recomp_rewind.c)
target_include_directories(recomp_rewind_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform)
target_compile_options(recomp_rewind_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME recomp_rewind COMMAND recomp_rewind_test)
set_tests_properties(recomp_rewind PROPERTIES TIMEOUT 120)
