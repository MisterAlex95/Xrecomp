cmake_minimum_required(VERSION 3.20)
project(xmv_hle_test C)
# The runtime's XMV hook (src/video/xmv_hle.c, XBA-70) on guest memory. POSIX
# hosts (nanosleep, setenv).
set(CMAKE_C_STANDARD 11)
if(WIN32)
    message(STATUS "xmv_hle: POSIX hosts only")
    return()
endif()
add_subdirectory(../.. runtime)
add_executable(xmv_hle_test test_main.c)
target_include_directories(xmv_hle_test PRIVATE ../../src)
target_link_libraries(xmv_hle_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME xmv_hle
         COMMAND xmv_hle_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe
                 ${CMAKE_CURRENT_SOURCE_DIR}/../xmv_decode/fixtures/basic.xmv
                 ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
