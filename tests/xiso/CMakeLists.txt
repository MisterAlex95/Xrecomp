cmake_minimum_required(VERSION 3.20)
project(xiso_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# src/xiso (XBA-40): extraction of synthetic disc images. The images are generated at configure time by
# make_fixtures.py (tools/xiso/mkxiso.py, ours, no game data) into the build dir and passed on the test's command
# line, so the device bundle (tools/android/bundle_tests.py, no python on the device) copies them.
if(NOT TARGET xiso)
    add_subdirectory(../../src/xiso ${CMAKE_CURRENT_BINARY_DIR}/xiso)
endif()
add_executable(xiso_test test_main.c)
target_link_libraries(xiso_test PRIVATE xiso)   # xiso links Threads (the source test reads from 4 threads)
add_executable(xiso_extract ../../tools/xiso/xiso_extract.c)   # desktop CLI, tools/xiso/README.md
target_link_libraries(xiso_extract PRIVATE xiso)

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
    set(_files tree.iso tree.manifest small.iso small.manifest small_base30600.iso default.xbe.ref
        bad_magic.iso truncated.iso truncated_dir.iso sector_range.iso self_loop.iso dir_cycle.iso
        dotdot.iso dot.iso slash.iso backslash.iso duplicate.iso
        small_v1.cso small_v2.cso small_v2f.cso small_v2m.cso small.zso small.cci small_b16k.cso tree_v1.cso
        tree_split.1.iso tree_split.2.iso tree_split.3.iso small_x.iso.001 small_x.iso.002 small_x.iso.003
        small_x.iso.004 small_x.iso.005 small_x.iso.006 small_p.part1.iso small_p.part2.iso small_p.part3.iso
        small_p.part4.iso small_cs.1.cso small_cs.2.cso bad_block.zso short.cso)
    list(TRANSFORM _files PREPEND ${_fx}/)
    add_test(NAME xiso COMMAND xiso_test ${_files})
    add_test(NAME xiso_extract_cli COMMAND xiso_extract ${_fx}/small.iso cli_out)
else()
    message(WARNING "xiso_test: no python3, or make_fixtures.py failed: the tests are skipped")
    add_test(NAME xiso COMMAND ${CMAKE_COMMAND} -E false)
    set_tests_properties(xiso PROPERTIES SKIP_RETURN_CODE 1)
endif()
