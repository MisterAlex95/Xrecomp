cmake_minimum_required(VERSION 3.20)
project(nic_posix_test C)
# The Ethernet controller with no cable (XBA-117) behind a real POSIX fault:
# a PROT_NONE page at 0xFEF00000, a SIGBUS/SIGSEGV handler and an arch
# decoder (A64 on arm64, x86-64 on Linux). The test drives it the way a
# forcedeth-style driver does and asserts it concludes "no link" quickly.
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
add_custom_target(nic_posix_xbe ALL DEPENDS ${TEST_XBE})

add_executable(nic_posix_test test_main.c)
add_dependencies(nic_posix_test nic_posix_xbe)
target_include_directories(nic_posix_test PRIVATE ../../src/kernel)
target_link_libraries(nic_posix_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME nic_posix_test COMMAND nic_posix_test ${TEST_XBE})
set_tests_properties(nic_posix_test PROPERTIES SKIP_RETURN_CODE 77)
# The A/B switch: RECOMP_NIC=0 is the old behaviour (plain memory, link up).
add_test(NAME nic_posix_off COMMAND nic_posix_test ${TEST_XBE})
set_tests_properties(nic_posix_off PROPERTIES SKIP_RETURN_CODE 77
                     ENVIRONMENT RECOMP_NIC=0)
