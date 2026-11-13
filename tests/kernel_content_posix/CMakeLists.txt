cmake_minimum_required(VERSION 3.20)
project(kernel_content_posix C)
if(WIN32)
    message(FATAL_ERROR "This fixture exercises the POSIX file backend")
endif()
set(CMAKE_C_STANDARD 11)
# Downloadable content (T:\$c) and title updates (T:\$u) on the save side
# (XBA-124): the real path and file code, no title, no guest memory.
add_subdirectory(../../src/platform platform)
add_executable(kernel_content_posix_test test_main.c
    ../../src/kernel/kernel_file.c ../../src/kernel/kernel_path.c)
target_include_directories(kernel_content_posix_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_content_posix_test PRIVATE platform)
enable_testing()
add_test(NAME kernel_content_posix COMMAND kernel_content_posix_test)
