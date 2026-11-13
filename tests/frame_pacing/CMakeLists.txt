cmake_minimum_required(VERSION 3.20)
project(frame_pacing_test C)
set(CMAKE_C_STANDARD 11)
# src/kernel/frame_pacing.h is header-only: RECOMP_PACING=auto's decision on a fake clock, with
# synthetic titles of 10, 20, 40 ms, varying and flip-locked frames (XBA-107, XBA-135).
add_executable(frame_pacing_test test_main.c)
target_include_directories(frame_pacing_test PRIVATE ../../src/kernel ../../src/video)
if(NOT MSVC)
    target_link_libraries(frame_pacing_test PRIVATE m)
endif()
enable_testing()
add_test(NAME frame_pacing COMMAND frame_pacing_test)
