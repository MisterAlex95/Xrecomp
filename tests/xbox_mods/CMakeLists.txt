cmake_minimum_required(VERSION 3.20)
project(xbox_mods_test C)
# Mods on the Xbox runtime (XBA-288, docs/technical/mods.md): the patched XBE Prepare made is what
# xbox_MemoryLayoutInit loads (RECOMP_MOD_XBE: a data byte only the patch changes is in guest memory; a copy of
# another title is refused), and the display hint (RECOMP_MOD_FRAMES / RECOMP_MOD_WIDE_WRITES) follows
# RECOMP_ASPECT: the writes are made when the frames are presented 16:9, not at 4:3. On the synthetic XBE.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX host (fork per run)")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(xbox_mods_test test_main.c)
target_include_directories(xbox_mods_test PRIVATE ../../src/kernel)
target_link_libraries(xbox_mods_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME xbox_mods_test
         COMMAND xbox_mods_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
set_tests_properties(xbox_mods_test PROPERTIES TIMEOUT 60)
