cmake_minimum_required(VERSION 3.20)
project(framegen_pacer_test C)
# The frame generation pacer (src/framegen) with its fake generator and a fake
# clock: C and pthreads only, so it builds on every POSIX host whether or not
# the runtime is configured with XBOXRECOMP_LSFG.
if(WIN32)
    message(FATAL_ERROR "The frame generation pacer is POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
add_executable(framegen_pacer_test test_main.c
    ../../src/framegen/framegen_pacer.c
    ../../src/framegen/framegen_fake.c)
target_include_directories(framegen_pacer_test PRIVATE ../../src/framegen)
target_link_libraries(framegen_pacer_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME framegen_pacer COMMAND framegen_pacer_test)
