cmake_minimum_required(VERSION 3.20)
project(fb_present_sdl_test C)
# The Xbox presenter's picture shaders (XBA-203, src/video/fb_present_sdl.c + present_shaders.c): each preset drawn
# by the real window (xbox_HostWindowMain, SDL's opengl and opengles2 renderers) and read back with
# xbox_FramebufferWindowCapture. Needs a display (Xvfb + Mesa is enough): skipped (77) without one or under
# XBR_BUILD_ONLY. POSIX hosts.
if(WIN32)
    message(FATAL_ERROR "The SDL presenter is POSIX-only (Windows has the GDI one, tests/fb_present)")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(fb_present_sdl_test test_main.c)
target_include_directories(fb_present_sdl_test PRIVATE ../../src ../../src/video)
target_link_libraries(fb_present_sdl_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME fb_present_sdl_shaders COMMAND fb_present_sdl_test)
set_tests_properties(fb_present_sdl_shaders PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                     ENVIRONMENT "SDL_AUDIODRIVER=dummy")
