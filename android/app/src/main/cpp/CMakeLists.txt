# libmain.so of the app shell (XBA-18): SDLActivity loads libSDL2.so, then this, and calls SDL_main.
# A title's build replaces shell_main.c with its own entry and generated C; the toolkit part stays the same.
cmake_minimum_required(VERSION 3.22)
project(xboxrecomp_shell C)

set(XBA_VERSION "dev" CACHE STRING "Build version shown in the ready line (CI: the commit)")

# The toolkit, from the root of this repository (SDL2 comes in as its subproject on Android).
get_filename_component(XBOXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
add_subdirectory("${XBOXRECOMP_ROOT}" xboxrecomp)

add_library(main SHARED shell_main.c)
target_compile_definitions(main PRIVATE XBA_VERSION="${XBA_VERSION}")
# The whole toolkit, not only what shell_main.c calls: System.loadLibrary("main") then checks on the device that
# bionic resolves every symbol the runtime needs (shell_main.c stands in for the four a title provides).
set(XBA_TOOLKIT_LIBS xbox_kernel xbox_d3d8 xbox_dsound xbox_input xbox_input_map xbox_usb xbox_apu xbox_nv2a
                     xbox_video platform host_time recomp_exit_hook recomp_cfg)
target_link_libraries(main PRIVATE -Wl,--whole-archive ${XBA_TOOLKIT_LIBS} -Wl,--no-whole-archive
                                   SDL2::SDL2 GLESv3 log android m)
target_link_options(main PRIVATE -Wl,--no-undefined)
