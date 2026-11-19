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
# XBA-309: a disc image holding movie/basic.xmv (tools/xiso/mkxiso.py, our own
# fixture), for the movie read in place from an image.
find_package(Python3 COMPONENTS Interpreter)
set(_disc_tree ${CMAKE_CURRENT_BINARY_DIR}/disc_tree)
set(_disc_iso ${CMAKE_CURRENT_BINARY_DIR}/movie_disc.iso)
set(_disc_arg "")
if(Python3_Interpreter_FOUND)
    file(REMOVE_RECURSE ${_disc_tree})
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/../xmv_decode/fixtures/basic.xmv DESTINATION ${_disc_tree}/movie)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/xiso/mkxiso.py
                            ${_disc_tree} ${_disc_iso}
                    RESULT_VARIABLE _rc OUTPUT_QUIET)
    if(_rc EQUAL 0)
        set(_disc_arg ${_disc_iso})
    endif()
endif()
add_test(NAME xmv_hle
         COMMAND xmv_hle_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe
                 ${CMAKE_CURRENT_SOURCE_DIR}/../xmv_decode/fixtures/basic.xmv
                 ${CMAKE_CURRENT_SOURCE_DIR}/fixtures ${_disc_arg})
