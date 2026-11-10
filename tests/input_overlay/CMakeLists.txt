# The touch overlay pad (src/input/input_overlay.c, the Android companion panel) merged into the SDL backend's
# xbox_InputGetState: merge over a pad, release, the no-pad stand-in, the keyboard stand-in, a concurrent writer.
# SDL is faked (tests/input_sdl_hotplug/fake_sdl). POSIX only: on Windows the test reports skipped (exit 77).
cmake_minimum_required(VERSION 3.20)
project(input_overlay_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

if(WIN32)
    add_executable(input_overlay_test skip.c)
else()
    find_package(Threads REQUIRED)
    add_executable(input_overlay_test
        test_main.c
        ../../src/input/xinput_device.c
        ../../src/input/input_overlay.c
        ../../src/input/input_map.c)
    target_include_directories(input_overlay_test PRIVATE
        ../input_sdl_hotplug/fake_sdl
        ../../src/input
        ../../src)
    target_link_libraries(input_overlay_test PRIVATE Threads::Threads m)
endif()

add_test(NAME input_overlay COMMAND input_overlay_test)
set_tests_properties(input_overlay PROPERTIES SKIP_RETURN_CODE 77)
