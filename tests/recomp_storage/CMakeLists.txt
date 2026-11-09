cmake_minimum_required(VERSION 3.20)
project(recomp_storage_test C)
set(CMAKE_C_STANDARD 11)
# recomp_storage.c only: no runtime to build.
add_executable(recomp_storage_test test_main.c ../../src/platform/recomp_storage.c)
target_include_directories(recomp_storage_test PRIVATE ../../src/platform)
enable_testing()
add_test(NAME recomp_storage COMMAND recomp_storage_test)
