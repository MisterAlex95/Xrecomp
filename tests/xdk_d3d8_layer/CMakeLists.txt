cmake_minimum_required(VERSION 3.20)
project(xdk_d3d8_layer_test C)
set(CMAKE_C_STANDARD 11)
# profiles/xdk/d3d8/d3d8.c (the XDK D3D profile layer) built twice against
# stubs of the runtime: as a title with no params (the device found at run
# time, XBA-80), as Fable's params, and as a D3D8LTCG 5849 title with only
# the D3D8LTCG layer's DMA walk (the fence event adopted), and as an XDK 5455
# title (KOTOR's layout: pointer slot at device - 8, fence event +0x19AC).
foreach(variant auto given ltcg x5455)
    add_executable(xdk_d3d8_${variant} test_main.c ../../profiles/xdk/d3d8/d3d8.c)
    target_include_directories(xdk_d3d8_${variant} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${variant})
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(xdk_d3d8_${variant} PRIVATE Threads::Threads)
    endif()
endforeach()
target_compile_definitions(xdk_d3d8_given PRIVATE TEST_GIVEN=1)
target_compile_definitions(xdk_d3d8_ltcg PRIVATE TEST_LTCG=1)
target_compile_definitions(xdk_d3d8_x5455 PRIVATE TEST_5455=1)
enable_testing()
add_test(NAME xdk_d3d8_auto COMMAND xdk_d3d8_auto)
add_test(NAME xdk_d3d8_given COMMAND xdk_d3d8_given)
add_test(NAME xdk_d3d8_ltcg COMMAND xdk_d3d8_ltcg)
add_test(NAME xdk_d3d8_x5455 COMMAND xdk_d3d8_x5455)
set_tests_properties(xdk_d3d8_auto xdk_d3d8_given xdk_d3d8_ltcg xdk_d3d8_x5455 PROPERTIES SKIP_RETURN_CODE 77)
