cmake_minimum_required(VERSION 3.20)
project(recomp_d3d_service_test C)
set(CMAKE_C_STANDARD 11)
# src/kernel/recomp_d3d_service.c (the XDK D3D service, a toolkit default; it
# was the profile layer profiles/xdk/d3d8/d3d8.c) built against stubs of the
# runtime, once per environment it is configured with:
#   auto     no switch: the device found at run time (XBA-80), the DMA walk by
#            default (the fence event adopted), D3D8LTCG 5849 (Burnout 3, Black, Fable)
#   given    RECOMP_D3D_DEVICE / _PTR / _SW_METHODS (Fable's old params) on D3D8 5849
#   scan     RECOMP_PB_WALK=scan: the segment scan, the fence event not adopted
#   off      RECOMP_D3D_SERVICE=0: no hook, no DMA-walk default
#   x5455    an XDK 5455 title (KOTOR's layout: pointer slot at device - 8, fence event +0x19AC)
#   unknown  D3D8LTCG 5933, a build the layout table does not know (XBA-105): 5849's slot
#            rule, no event pair, every fallback said and recorded
#   x4627    D3D8 4627 with its slot and events elsewhere (found by structure)
#   devonly  RECOMP_D3D_DEVICE alone, no RECOMP_D3D_SW_METHODS: every method serviced
#   swmask   RECOMP_D3D_SW_METHODS=0x1000 on a found device: 0xD and 0xE left unserviced
foreach(variant auto given scan off x5455 unknown x4627 devonly swmask)
    add_executable(recomp_d3d_service_${variant} test_main.c ../../src/kernel/recomp_d3d_service.c)
    target_include_directories(recomp_d3d_service_${variant} PRIVATE ../../src/kernel)
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(recomp_d3d_service_${variant} PRIVATE Threads::Threads)
    endif()
endforeach()
target_compile_definitions(recomp_d3d_service_given PRIVATE TEST_GIVEN=1)
target_compile_definitions(recomp_d3d_service_scan PRIVATE TEST_SCAN=1)
target_compile_definitions(recomp_d3d_service_off PRIVATE TEST_OFF=1)
target_compile_definitions(recomp_d3d_service_x5455 PRIVATE TEST_5455=1)
target_compile_definitions(recomp_d3d_service_unknown PRIVATE TEST_UNKNOWN=1)
target_compile_definitions(recomp_d3d_service_x4627 PRIVATE TEST_4627=1)
target_compile_definitions(recomp_d3d_service_devonly PRIVATE TEST_DEVONLY=1)
target_compile_definitions(recomp_d3d_service_swmask PRIVATE TEST_SWMASK=1)
enable_testing()
foreach(variant auto given scan off x5455 unknown x4627 devonly swmask)
    add_test(NAME recomp_d3d_service_${variant} COMMAND recomp_d3d_service_${variant})
    set_tests_properties(recomp_d3d_service_${variant} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
