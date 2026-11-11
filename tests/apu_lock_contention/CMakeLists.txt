cmake_minimum_required(VERSION 3.20)
project(apu_lock_contention_test C)
# A thread hammering d->lock through mcpx_apu_lock_guest while the frame
# thread runs flat out: every acquisition within 2 ms, and the frame thread
# yields the lock only while someone waits for it.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_lock_contention_test test_main.c)
target_include_directories(apu_lock_contention_test PRIVATE ../../src ../../src/apu)
# test_main.c defines every xa2_* function, so no host audio device is opened.
target_link_libraries(apu_lock_contention_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_lock_contention COMMAND apu_lock_contention_test)
