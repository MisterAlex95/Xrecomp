cmake_minimum_required(VERSION 3.20)
project(ac97_trap_posix_test C)
# The AC'97 bus-master write trap behind a real POSIX fault: two threads set
# RR in different channel control bytes and must never read it back set.
# Hosts without a store decoder (A64 on arm64, x86-64 on Linux) exit 77.
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
add_custom_target(ac97_trap_posix_xbe ALL DEPENDS ${TEST_XBE})

add_executable(ac97_trap_posix_test test_main.c)
add_dependencies(ac97_trap_posix_test ac97_trap_posix_xbe)
target_include_directories(ac97_trap_posix_test PRIVATE ../../src/kernel)
target_link_libraries(ac97_trap_posix_test PRIVATE xboxrecomp)
find_package(Threads REQUIRED)
target_link_libraries(ac97_trap_posix_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME ac97_trap_posix_test COMMAND ac97_trap_posix_test ${TEST_XBE})
set_tests_properties(ac97_trap_posix_test PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME ac97_trap_posix_fallback COMMAND ac97_trap_posix_test ${TEST_XBE} fallback)
set_tests_properties(ac97_trap_posix_fallback PROPERTIES SKIP_RETURN_CODE 77)
