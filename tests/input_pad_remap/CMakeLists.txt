# RECOMP_PAD_MAP through the SDL backend (XBA-131): a fake SDL game controller (tests/input_sdl_hotplug/fake_sdl)
# whose buttons and axes the test sets, read by src/input/xinput_device.c with and without a remapping. POSIX only:
# on Windows the test reports skipped (exit 77).
cmake_minimum_required(VERSION 3.20)
project(input_pad_remap_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

if(WIN32)
    add_executable(input_pad_remap_test skip.c)
else()
    find_package(Threads REQUIRED)
    add_executable(input_pad_remap_test
        test_main.c
        ../../src/input/xinput_device.c
        ../../src/input/input_overlay.c
        ../../src/input/input_map.c
        ../../src/input/input_ports.c
        ../input_sdl_hotplug/fake_sdl/fake_sdl_ids.c)
    target_include_directories(input_pad_remap_test PRIVATE
        ../input_sdl_hotplug/fake_sdl
        ../../src/input
        ../../src)
    target_link_libraries(input_pad_remap_test PRIVATE Threads::Threads m)
endif()

add_test(NAME input_pad_remap COMMAND input_pad_remap_test)
set_tests_properties(input_pad_remap PROPERTIES SKIP_RETURN_CODE 77)
