cmake_minimum_required(VERSION 3.20)
project(apu_volume_test C)
# The output volume (apu_volume.c alone: no runtime to build): the Android in-game menu's volume and mute.
set(CMAKE_C_STANDARD 11)
add_executable(apu_volume_test test_main.c ../../src/apu/apu_volume.c)
target_include_directories(apu_volume_test PRIVATE ../../src/apu)
enable_testing()
add_test(NAME apu_volume COMMAND apu_volume_test)
