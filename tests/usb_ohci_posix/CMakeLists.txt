cmake_minimum_required(VERSION 3.20)
project(usb_ohci_posix_test C)
# The OHCI register model behind a real POSIX fault: a PROT_NONE aperture, a
# SIGBUS/SIGSEGV handler and an arch decoder (A64 on arm64, x86-64 on Linux).
# Elsewhere the model stays off by design and the test exits 77 (skipped).
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX trap path")
endif()
set(CMAKE_C_STANDARD 11)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_subdirectory(../.. runtime)

# The guest window needs an XBE header to map; the synthetic one is enough.
set(TEST_XBE ${CMAKE_CURRENT_BINARY_DIR}/test.xbe)
add_custom_command(OUTPUT ${TEST_XBE}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/mkxbe.py ${TEST_XBE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/mkxbe.py)
add_custom_target(usb_ohci_posix_xbe ALL DEPENDS ${TEST_XBE})

add_executable(usb_ohci_posix_test test_main.c)
add_dependencies(usb_ohci_posix_test usb_ohci_posix_xbe)
target_include_directories(usb_ohci_posix_test PRIVATE
    ../../src/kernel ../../src/usb)
target_link_libraries(usb_ohci_posix_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME usb_ohci_posix_test COMMAND usb_ohci_posix_test ${TEST_XBE})
set_tests_properties(usb_ohci_posix_test PROPERTIES SKIP_RETURN_CODE 77)
# The same run with the guest-side catch-up switched off (its A/B switch):
# the list and the USB line must be left to the controller thread.
add_test(NAME usb_ohci_posix_catchup_off COMMAND usb_ohci_posix_test ${TEST_XBE})
set_tests_properties(usb_ohci_posix_catchup_off PROPERTIES SKIP_RETURN_CODE 77
                     ENVIRONMENT RECOMP_OHCI_CATCHUP=0)
# The old configuration: no TIB per thread, so the controller thread never
# runs (XBA-133 made the default configuration the first entry's).
add_test(NAME usb_ohci_posix_no_tib COMMAND usb_ohci_posix_test ${TEST_XBE})
set_tests_properties(usb_ohci_posix_no_tib PROPERTIES SKIP_RETURN_CODE 77
                     ENVIRONMENT RECOMP_TIB_PER_THREAD=0)
# Save states (XBA-248): the USB section captured, the model reset, restored, the same pad report.
add_test(NAME usb_ohci_posix_state COMMAND usb_ohci_posix_test ${TEST_XBE} state)
set_tests_properties(usb_ohci_posix_state PROPERTIES SKIP_RETURN_CODE 77
                     ENVIRONMENT RECOMP_USB_PADS=1)
