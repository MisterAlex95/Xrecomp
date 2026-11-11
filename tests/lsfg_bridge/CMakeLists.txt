cmake_minimum_required(VERSION 3.20)
project(lsfg_bridge_test C)
# src/framegen/lsfg_bridge.cpp (frame generation, docs/technical/framegen.md): the Vulkan bridge on a real device
# with the user's own shaders: a moving square between two synthetic frames, plus a benchmark mode.
#
# Needs XBOXRECOMP_LSFG_VK_SRC / $LSFG_VK_SRC (the pinned framegen checkout) to build the bridge; without it, or at
# run time without a Vulkan driver or shaders (RECOMP_LSFG_DIR = a translated cache, or RECOMP_LSFG_DLL plus a
# translator built in with XBOXRECOMP_LSFG_DXVK_SRC), the test is skipped (exit 77). The shaders never come from
# this repository.
set(CMAKE_C_STANDARD 11)
enable_testing()
set(XBOXRECOMP_LSFG_VK_SRC "$ENV{LSFG_VK_SRC}" CACHE PATH "GameNative/lsfg-vk-android at d33caceb")
set(XBOXRECOMP_LSFG_DXVK_SRC "$ENV{DXVK_SRC}" CACHE PATH "DXVK v2.6.2 (optional: translate RECOMP_LSFG_DLL)")
if(NOT XBOXRECOMP_LSFG_VK_SRC)
    message(STATUS "lsfg_bridge_test: no XBOXRECOMP_LSFG_VK_SRC / LSFG_VK_SRC: the test is skipped")
    add_test(NAME lsfg_bridge COMMAND ${CMAKE_COMMAND} -E false)
    set_tests_properties(lsfg_bridge PROPERTIES SKIP_RETURN_CODE 1)
    return()
endif()
include(../../src/framegen/lsfg_shaders.cmake)
include(../../src/framegen/lsfg_vulkan.cmake)
add_executable(lsfg_bridge_test test_main.c ../../src/framegen/lsfg_bridge.cpp)
set_target_properties(lsfg_bridge_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(lsfg_bridge_test PRIVATE ../../src/framegen)
target_link_libraries(lsfg_bridge_test PRIVATE lsfg_shaders lsfg-vk-framegen volk)
if(XBOXRECOMP_LSFG_DXVK_SRC)
    include(../../src/framegen/lsfg_dxbc.cmake)
    target_link_libraries(lsfg_bridge_test PRIVATE lsfg_dxbc)
    target_compile_definitions(lsfg_bridge_test PRIVATE LSFG_TEST_TRANSLATOR=1)
endif()
add_test(NAME lsfg_bridge COMMAND lsfg_bridge_test)
set_tests_properties(lsfg_bridge PROPERTIES SKIP_RETURN_CODE 77)
