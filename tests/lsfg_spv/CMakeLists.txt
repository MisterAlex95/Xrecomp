cmake_minimum_required(VERSION 3.20)
project(lsfg_spv_test C)
# src/framegen/lsfg_spv.c (frame generation, docs/technical/framegen.md): SPIR-V checks and binding renumbering on
# hand-assembled modules (spv_build.h). libc only, no Vulkan, no DLL: builds on every POSIX host whether or not the
# runtime is configured with XBOXRECOMP_LSFG.
set(CMAKE_C_STANDARD 11)
include(../../src/framegen/lsfg_shaders.cmake)
add_executable(lsfg_spv_test test_main.c)
target_link_libraries(lsfg_spv_test PRIVATE lsfg_shaders)
if(NOT MSVC)
    target_compile_options(lsfg_spv_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME lsfg_spv COMMAND lsfg_spv_test)
