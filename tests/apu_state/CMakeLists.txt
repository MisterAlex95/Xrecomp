cmake_minimum_required(VERSION 3.20)
project(apu_state_test C)
# Save states of the APU model (XBA-248): the voice processor against a fake
# guest RAM, mixed, captured, reset, restored and mixed again. test_main.c
# compiles apu_vp.c and apu_savestate.c itself (no APU thread, no device),
# like tests/apu_vp, but runs on every host (it links only the platform
# layer).
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)   # for platform (the Win32 primitives the VP uses on POSIX)
add_executable(apu_state_test test_main.c)
target_include_directories(apu_state_test PRIVATE ../../src ../../src/apu)
target_compile_definitions(apu_state_test PRIVATE _USE_MATH_DEFINES)
target_link_libraries(apu_state_test PRIVATE platform)
if(NOT WIN32)
    target_link_libraries(apu_state_test PRIVATE m)
endif()
enable_testing()
add_test(NAME apu_state COMMAND apu_state_test)
