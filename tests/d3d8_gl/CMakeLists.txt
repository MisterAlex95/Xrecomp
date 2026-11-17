cmake_minimum_required(VERSION 3.20)
project(d3d8_gl_regression C)
# The POSIX D3D8 back end (src/d3d/d3d8_gl.c) drawing on a real GL 3.3 core
# context: SDL's offscreen video driver over Mesa's surfaceless EGL
# (llvmpipe), pixels read back. Exit 77 without a context. Desktop Linux
# only: d3d8_gl.c needs desktop GL (it cannot draw on Android's GLES, see
# src/d3d/gl_host.h), Windows uses the D3D11 back end, and macOS has no
# libepoxy in CI; there the test is a stub that reports a skip.
set(CMAKE_C_STANDARD 11)
enable_testing()
if(WIN32 OR APPLE OR ANDROID)
    add_executable(d3d8_gl_test skip.c)
    add_test(NAME d3d8_gl COMMAND d3d8_gl_test)
    set_tests_properties(d3d8_gl PROPERTIES SKIP_RETURN_CODE 77)
    return()
endif()
find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED epoxy)
add_executable(d3d8_gl_test test_main.c ../../src/d3d/d3d8_gl.c)
target_include_directories(d3d8_gl_test PRIVATE ../../src/d3d ../../src ${EPOXY_INCLUDE_DIRS})
target_link_libraries(d3d8_gl_test PRIVATE SDL2::SDL2 ${EPOXY_LINK_LIBRARIES} m)
add_test(NAME d3d8_gl COMMAND d3d8_gl_test)
set_tests_properties(d3d8_gl PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
    ENVIRONMENT "SDL_VIDEODRIVER=offscreen;SDL_AUDIODRIVER=dummy;EGL_PLATFORM=surfaceless")
