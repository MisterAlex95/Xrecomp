cmake_minimum_required(VERSION 3.20)
project(xbox_cheats_test C)
# An Xbox title's cheat codes (XBA-241, src/kernel/xbox_cheats.h) against a real guest window: xbox_MemoryLayoutInit
# on the synthetic tools/conformance/test.xbe, codes changing values in guest memory (heap), writes into the XBE's
# code section, the low page and unmapped addresses refused without a fault, a conditional on one failing, and the
# cheat file read from RECOMP_CHEATS_FILE at the first vblank and read again when it changes. POSIX (the engine is
# part of the POSIX host core); skipped (77) on Windows.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(xbox_cheats_test test_main.c)
target_include_directories(xbox_cheats_test PRIVATE ../../src/kernel)
target_link_libraries(xbox_cheats_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME xbox_cheats_test
         COMMAND xbox_cheats_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe
                 ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(xbox_cheats_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
