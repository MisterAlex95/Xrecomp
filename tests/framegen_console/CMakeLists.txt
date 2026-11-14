cmake_minimum_required(VERSION 3.20)
project(framegen_console_test C)
# The console window's frame generation front end (src/framegen/framegen_console.c, XBA-186) over the real pacer,
# the fake generator and a fake clock: C and pthreads only, no SDL, no XBOXRECOMP_LSFG needed.
if(WIN32)
    message(FATAL_ERROR "The frame generation pacer is POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
add_executable(framegen_console_test test_main.c
    ../../src/framegen/framegen_console.c
    ../../src/framegen/framegen_pacer.c
    ../../src/framegen/framegen_fake.c)
target_include_directories(framegen_console_test PRIVATE ../../src/framegen)
target_link_libraries(framegen_console_test PRIVATE Threads::Threads)
target_compile_options(framegen_console_test PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME framegen_console COMMAND framegen_console_test)
