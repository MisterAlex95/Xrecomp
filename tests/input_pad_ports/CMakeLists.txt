# Four pads from the connected controllers (XBA-123): input_ports.c's rules, usb_hotplug.c's decisions, and the SDL
# backend over a fake SDL with several controllers (which one each port reads, rumble, a disconnect keeping its port,
# the port mask with the touch overlay). POSIX only: on Windows the test reports skipped (exit 77).
cmake_minimum_required(VERSION 3.20)
project(input_pad_ports_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

if(WIN32)
    add_executable(input_pad_ports_test skip.c)
else()
    find_package(Threads REQUIRED)
    add_executable(input_pad_ports_test
        test_main.c
        ../../src/input/xinput_device.c
        ../../src/input/input_overlay.c
        ../../src/input/input_map.c
        ../../src/input/input_ports.c
        ../../src/usb/usb_hotplug.c)
    target_include_directories(input_pad_ports_test PRIVATE
        ../input_sdl_hotplug/fake_sdl
        ../../src/input
        ../../src/usb
        ../../src)
    target_link_libraries(input_pad_ports_test PRIVATE Threads::Threads m)
endif()

add_test(NAME input_pad_ports COMMAND input_pad_ports_test)
set_tests_properties(input_pad_ports PROPERTIES SKIP_RETURN_CODE 77)
