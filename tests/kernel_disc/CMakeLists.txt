cmake_minimum_required(VERSION 3.20)
project(kernel_disc C)
if(WIN32)
    message(FATAL_ERROR "This fixture exercises the POSIX file layer's disc image backing")
endif()
set(CMAKE_C_STANDARD 11)
# XBA-126: D: served from a disc image by the real file/path layer (kernel_file.c, kernel_path.c, kernel_disc.c),
# compared with the same tree as a folder. The tree and its images (bare, redump, CSO, split) are generated at
# configure time by make_fixtures.py (tools/xiso/mkxiso.py and mkcso.py; ours, no game data).
add_subdirectory(../../src/platform platform)
add_subdirectory(../../src/xiso xiso)
add_executable(kernel_disc_test test_main.c
    ../../src/kernel/kernel_file.c ../../src/kernel/kernel_path.c ../../src/kernel/kernel_disc.c
    ../../src/kernel/prep_progress.c)
target_include_directories(kernel_disc_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_disc_test PRIVATE platform xiso)
# Read throughput, folder vs image vs CSO (not a test: tools/xiso/bench_disc.sh runs it).
add_executable(kernel_disc_bench bench.c
    ../../src/kernel/kernel_file.c ../../src/kernel/kernel_path.c ../../src/kernel/kernel_disc.c
    ../../src/kernel/prep_progress.c)
target_include_directories(kernel_disc_bench PRIVATE ../../src/kernel)
target_link_libraries(kernel_disc_bench PRIVATE platform xiso)

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
set(_fx ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/xiso/mkxiso.py
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/xiso/mkcso.py
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/mkxbe.py)
set(_ok FALSE)
if(Python3_Interpreter_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py ${_fx}
                    RESULT_VARIABLE _rc)
    if(_rc EQUAL 0)
        set(_ok TRUE)
    endif()
endif()
if(_ok)
    add_test(NAME kernel_disc COMMAND kernel_disc_test ${_fx})
else()
    message(WARNING "kernel_disc: no python3, or make_fixtures.py failed: the test is skipped")
    add_test(NAME kernel_disc COMMAND ${CMAKE_COMMAND} -E false)
    set_tests_properties(kernel_disc PROPERTIES SKIP_RETURN_CODE 1)
endif()
