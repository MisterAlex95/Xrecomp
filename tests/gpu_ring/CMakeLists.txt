cmake_minimum_required(VERSION 3.20)
project(gpu_ring_test C)
# The streaming rings' bookkeeping (src/video/gpu_ring.h: the GameCube's RECOMP_GC_GX_STREAM=storage rings, the NV2A
# back end's vertex and index streams) against a simulated GPU, no GL: no byte the GPU may still read is ever written,
# across wraps, with any alignment or stride, with two rings behind the same draws. Pure C, every host.
set(CMAKE_C_STANDARD 11)
add_executable(gpu_ring_test gpu_ring_test.c)
target_include_directories(gpu_ring_test PRIVATE ../../src/video)
if(NOT MSVC)
    target_compile_options(gpu_ring_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME gpu_ring COMMAND gpu_ring_test)
