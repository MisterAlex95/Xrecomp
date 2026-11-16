# Fast-forward for Xbox titles (XBA-240).
#   xbox_fast_forward_clock   guest_clock.c alone on a fake clock (scaling, continuity, spans, decimation, the
#                             untouched 1x path) and xbox_pad_hotkeys.h (an Xbox pad's hotkey sources, swallowing)
#   xbox_fast_forward_kernel  through the runtime on the real clock: the toggle and hold -> x4, every guest time
#                             source four times as fast, a guest sleep a quarter as long, the output muted; POSIX
cmake_minimum_required(VERSION 3.20)
project(xbox_fast_forward_test C)
set(CMAKE_C_STANDARD 11)
enable_testing()

add_executable(xbox_fast_forward_clock test_clock.c ../../src/kernel/guest_clock.c)
target_include_directories(xbox_fast_forward_clock PRIVATE ../../src/kernel ../../src ../../src/input)
add_test(NAME xbox_fast_forward_clock COMMAND xbox_fast_forward_clock)

if(NOT WIN32)
    add_subdirectory(../.. runtime)
    add_executable(xbox_fast_forward_kernel test_kernel.c)
    target_include_directories(xbox_fast_forward_kernel PRIVATE ../../src/kernel ../../src)
    target_link_libraries(xbox_fast_forward_kernel PRIVATE xboxrecomp)
    add_test(NAME xbox_fast_forward_kernel COMMAND xbox_fast_forward_kernel)
    set_tests_properties(xbox_fast_forward_kernel PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()
