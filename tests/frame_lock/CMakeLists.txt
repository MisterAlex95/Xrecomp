cmake_minimum_required(VERSION 3.20)
project(frame_lock_test C)
set(CMAKE_C_STANDARD 11)
# src/kernel/frame_lock.h is header-only: the decision, driven with numbers (XBA-96).
add_executable(frame_lock_test test_main.c)
target_include_directories(frame_lock_test PRIVATE ../../src/kernel)
enable_testing()
add_test(NAME frame_lock COMMAND frame_lock_test)
