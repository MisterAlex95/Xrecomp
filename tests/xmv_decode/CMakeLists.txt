cmake_minimum_required(VERSION 3.20)
project(xmv_decode_test C)
# The portable XMV decoder (src/video/xmv, XBA-70): pure C, every host, no
# runtime. The APU's ADPCM decoder (src/apu/apu_state.h, header-only) is the
# audio reference.
set(CMAKE_C_STANDARD 11)
add_library(xmv_under_test STATIC
    ../../src/video/xmv/xmv_demux.c
    ../../src/video/xmv/xmv_player.c
    ../../src/video/xmv/xmv_surface.c
    ../../src/video/xmv/wmv2_dec.c
    ../../src/video/xmv/wmv2_tables.c
    ../../src/video/xmv/xbox_adpcm.c)
target_include_directories(xmv_under_test PUBLIC ../../src/video/xmv)
if(NOT MSVC)
    target_compile_options(xmv_under_test PRIVATE -Wall -Wextra)
endif()
add_executable(xmv_decode_test test_main.c)
target_include_directories(xmv_decode_test PRIVATE ../../src/apu ../../src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xmv_decode_test PRIVATE xmv_under_test)
target_compile_definitions(xmv_decode_test PRIVATE
    XMV_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# xmv_dump (tools/xmv/xmv_dump.c): decode a movie to Y4M + WAV with this
# decoder. Built here so it always compiles; the test runs it on a fixture.
add_executable(xmv_dump ../../tools/xmv/xmv_dump.c)
target_link_libraries(xmv_dump PRIVATE xmv_under_test)
enable_testing()
add_test(NAME xmv_decode COMMAND xmv_decode_test)
add_test(NAME xmv_dump COMMAND xmv_dump ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/basic.xmv)
