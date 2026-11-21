cmake_minimum_required(VERSION 3.20)
project(usb_ohci_walker_test C)
# The OHCI descriptor walker against a driver that changes the queue while it
# runs: a fake driver thread re-arms, cancels and rewrites an interrupt ED
# while the walker makes a million passes over it.
set(CMAKE_C_STANDARD 11)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
add_subdirectory(../.. runtime)

# The guest window needs an XBE header to map; the synthetic one is enough.
set(TEST_XBE ${CMAKE_CURRENT_BINARY_DIR}/test.xbe)
add_custom_command(OUTPUT ${TEST_XBE}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/mkxbe.py ${TEST_XBE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/mkxbe.py)
add_custom_target(usb_ohci_walker_xbe ALL DEPENDS ${TEST_XBE})

add_executable(usb_ohci_walker_test test_main.c)
add_dependencies(usb_ohci_walker_test usb_ohci_walker_xbe)
target_include_directories(usb_ohci_walker_test PRIVATE
    ../../src/kernel ../../src/usb)
target_link_libraries(usb_ohci_walker_test PRIVATE xboxrecomp Threads::Threads)
enable_testing()
add_test(NAME usb_ohci_walker_test COMMAND usb_ohci_walker_test ${TEST_XBE})
set_tests_properties(usb_ohci_walker_test PROPERTIES SKIP_RETURN_CODE 77)
# XBA-329: controller passes back to back against guest-side walks of the
# same list; the frame clock never moves under a walk and loses no frame.
add_test(NAME usb_ohci_walker_frame_race COMMAND usb_ohci_walker_test ${TEST_XBE} frame_race)
set_tests_properties(usb_ohci_walker_frame_race PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
