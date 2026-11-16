cmake_minimum_required(VERSION 3.20)
project(apu_health_test C)
# The always-on audio health line ([APU] health, apu_core.c): a front-end
# trap held 30 ms against SDL's dummy driver reports exactly one device
# underrun, counted at submit time; a persistent stream voice with nothing
# to play reports dry time.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_health_test test_main.c)
target_include_directories(apu_health_test PRIVATE ../../src ../../src/apu)
# No fake xa2_*: the archive's SDL2 backend (apu_sdl2.c) is linked, and the
# test forces SDL's dummy driver, which drains the queue in real time with
# no device.
target_link_libraries(apu_health_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_health COMMAND apu_health_test)
# Windows: XAudio2 would open a real device, so the trap case skips (77).
set_tests_properties(apu_health PROPERTIES SKIP_RETURN_CODE 77)
