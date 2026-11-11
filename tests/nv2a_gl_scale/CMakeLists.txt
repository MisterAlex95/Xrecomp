cmake_minimum_required(VERSION 3.20)
project(nv2a_gl_scale_regression C)
enable_testing()
# Internal resolution policy and rectangle arithmetic (src/gpu/nv2a_gl_scale.c):
# no GL, runs anywhere.
add_executable(nv2a-gl-scale-test nv2a_gl_scale_test.c ../../src/gpu/nv2a_gl_scale.c)
target_include_directories(nv2a-gl-scale-test PRIVATE ../../src/gpu ../../src/kernel ../../src)
if(MSVC)
    target_compile_definitions(nv2a-gl-scale-test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME nv2a-gl-scale COMMAND nv2a-gl-scale-test)
