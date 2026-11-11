cmake_minimum_required(VERSION 3.20)
project(lsfg_dll_test C)
# src/framegen/lsfg_dll.c + lsfg_cache.c (frame generation, docs/technical/framegen.md): the PE resource walker and
# the shader cache on synthetic PE files the test builds itself (no Lossless.dll, no shader from it). libc only:
# builds on every POSIX host whether or not the runtime is configured with XBOXRECOMP_LSFG.
if(WIN32)
    message(FATAL_ERROR "lsfg_dll_test is POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)
include(../../src/framegen/lsfg_shaders.cmake)
add_executable(lsfg_dll_test test_main.c)
target_link_libraries(lsfg_dll_test PRIVATE lsfg_shaders)
if(NOT MSVC)
    target_compile_options(lsfg_dll_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME lsfg_dll COMMAND lsfg_dll_test)
