cmake_minimum_required(VERSION 3.20)
project(chihiro_mode_test C)
# Sega Chihiro mode (XBA-234): our synthetic XBE on the 128 MB map with the
# arcade kernel's XboxHardwareInfo (RECOMP_CHIHIRO=1), and the Xbox unchanged
# without it. POSIX-only: the mirror probe forks.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX host")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(chihiro_mode_test test_main.c)
target_include_directories(chihiro_mode_test PRIVATE ../../src/kernel)
target_link_libraries(chihiro_mode_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME chihiro_mode_default
         COMMAND chihiro_mode_test default
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
set_tests_properties(chihiro_mode_default PROPERTIES ENVIRONMENT RECOMP_CHIHIRO=0)
add_test(NAME chihiro_mode_chihiro
         COMMAND chihiro_mode_test chihiro
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
set_tests_properties(chihiro_mode_chihiro PROPERTIES ENVIRONMENT RECOMP_CHIHIRO=1)
