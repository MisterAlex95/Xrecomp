cmake_minimum_required(VERSION 3.20)
project(prep_progress C)
set(CMAKE_C_STANDARD 11)
# WP7: the "preparing data" indicator. The burst logic (src/kernel/prep_progress.h, header-only) driven with numbers,
# and on POSIX the real file and path layers: writes to Z: count, writes elsewhere do not.
add_executable(prep_progress_test test_main.c)
target_include_directories(prep_progress_test PRIVATE ../../src/kernel)
if(NOT WIN32)
    add_subdirectory(../../src/platform platform)
    add_subdirectory(../../src/xiso xiso)   # kernel_disc.c (XBA-126), as in tests/kernel_content_posix
    target_sources(prep_progress_test PRIVATE
        ../../src/kernel/kernel_file.c ../../src/kernel/kernel_path.c ../../src/kernel/kernel_disc.c
        ../../src/kernel/prep_progress.c)
    target_compile_definitions(prep_progress_test PRIVATE PREP_TEST_FILES=1)
    target_link_libraries(prep_progress_test PRIVATE platform xiso)
endif()
enable_testing()
add_test(NAME prep_progress COMMAND prep_progress_test)
