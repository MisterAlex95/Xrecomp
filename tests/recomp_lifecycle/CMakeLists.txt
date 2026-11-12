cmake_minimum_required(VERSION 3.20)
project(recomp_lifecycle_test C)
set(CMAKE_C_STANDARD 11)
# recomp_lifecycle.c and host_cpus.c (the resume restores the cores): no runtime to build.
find_package(Threads REQUIRED)
add_executable(recomp_lifecycle_test test_main.c ../../src/platform/recomp_lifecycle.c
               ../../src/platform/host_cpus.c)   # recomp_host_cpus_restore on resume (XBA-50)
target_include_directories(recomp_lifecycle_test PRIVATE ../../src/platform)
target_link_libraries(recomp_lifecycle_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME recomp_lifecycle COMMAND recomp_lifecycle_test)
