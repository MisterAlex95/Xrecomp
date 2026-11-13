cmake_minimum_required(VERSION 3.20)
project(apu_dsp_watch_test C)
# XBA-122: what a title asks of the GP/EP DSPs is recorded (commands, images,
# mixbins), and each DSP handshake has a bounded answer. Synthetic inputs only.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_dsp_watch_test test_main.c)
target_include_directories(apu_dsp_watch_test PRIVATE ../../src ../../src/apu)
target_link_libraries(apu_dsp_watch_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_dsp_watch COMMAND apu_dsp_watch_test)
