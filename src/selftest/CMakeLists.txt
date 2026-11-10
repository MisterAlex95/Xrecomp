cmake_minimum_required(VERSION 3.20)
project(recomp_selftest C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# The in-app self-test (XBA-51, recomp_selftest.h): the memory layout, its mirrors, trapped MMIO and the guest's
# execute protections, run in the calling process. The Android app shell runs it under RECOMP_SHELL_SELFTEST=1;
# tests/recomp_selftest runs it on Linux. POSIX hosts.
add_library(recomp_selftest STATIC recomp_selftest.c)
target_include_directories(recomp_selftest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_selftest PUBLIC xbox_kernel platform)
if(NOT MSVC)
    target_compile_options(recomp_selftest PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
