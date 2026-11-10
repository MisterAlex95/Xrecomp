# libmain.so of the app shell (XBA-18): SDLActivity loads libSDL2.so, then this, and calls SDL_main.
# A title is linked next to shell_main.c (XBA-52): -DXBA_TITLE_DIR=<title project> (gradle -PxbaTitleDir=...).
cmake_minimum_required(VERSION 3.22)
project(xboxrecomp_shell C)

set(XBA_VERSION "dev" CACHE STRING "Build version shown in the ready line (CI: the commit)")
set(XBA_TITLE_DIR "" CACHE PATH
    "A title project (a copy of templates/new-game with its generated C) to link into libmain.so; empty = shell only")

# The toolkit, from the root of this repository (SDL2 comes in as its subproject on Android).
get_filename_component(XBOXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
add_subdirectory("${XBOXRECOMP_ROOT}" xboxrecomp)

add_library(main SHARED shell_main.c pack_loader.c companion_jni.c menu_jni.c)
# companion_jni.c: the second-screen panel; menu_jni.c: the in-game menu (hold, volume, quit)
target_compile_definitions(main PRIVATE XBA_VERSION="${XBA_VERSION}")
# The whole toolkit, not only what shell_main.c calls: System.loadLibrary("main") then checks on the device that
# bionic resolves every symbol the runtime needs (pack_loader.c has weak forwarders for the four a title provides:
# a title linked in wins, otherwise they reach a dlopened title pack, pack_loader.h).
set(XBA_TOOLKIT_LIBS xbox_kernel xbox_d3d8 xbox_dsound xbox_input xbox_input_map xbox_usb xbox_apu xbox_nv2a
                     xbox_video platform host_time recomp_exit_hook recomp_cfg host_cpus
                     recomp_storage recomp_app recomp_selftest recomp_pack)
target_link_libraries(main PRIVATE -Wl,--whole-archive ${XBA_TOOLKIT_LIBS} -Wl,--no-whole-archive
                                   SDL2::SDL2 GLESv3 log android m)
target_link_options(main PRIVATE -Wl,--no-undefined)

# libxba_import.so (XBA-40, XBA-54): the game picker's ISO import, JNI around src/xiso. A library of its own: the
# picker runs in its own process before SDL, and must not load libSDL2.so and the whole runtime that libmain.so
# brings. The toolkit's root CMakeLists adds src/xiso (target xiso, libc only).
if(NOT TARGET xiso)
    add_subdirectory("${XBOXRECOMP_ROOT}/src/xiso" xiso)
endif()
add_library(xba_import SHARED xiso_jni.c)
target_link_libraries(xba_import PRIVATE xiso log)
target_link_options(xba_import PRIVATE -Wl,--no-undefined)

# The title (XBA-52). Its CMakeLists sees XBOXRECOMP_SHELL_TITLE and, instead of an executable, defines the object
# library xba_title (its main.c then defines recomp_title_main, which shell_main.c calls). Objects, not an archive:
# every one of them is linked, as an executable would link them.
if(XBA_TITLE_DIR)
    get_filename_component(_xba_title_dir "${XBA_TITLE_DIR}" ABSOLUTE)
    if(NOT EXISTS "${_xba_title_dir}/CMakeLists.txt")
        message(FATAL_ERROR "XBA_TITLE_DIR=${XBA_TITLE_DIR}: no CMakeLists.txt there (copy templates/new-game)")
    endif()
    set(XBOXRECOMP_SHELL_TITLE ON)
    # A cache entry: the template declares XBOXRECOMP_DIR with set(... CACHE PATH), which under its
    # cmake_minimum_required(3.20) would drop a plain variable of that name.
    set(XBOXRECOMP_DIR "${XBOXRECOMP_ROOT}" CACHE PATH "The xboxrecomp toolkit (set by the app shell)" FORCE)
    add_subdirectory("${_xba_title_dir}" title)
    if(NOT TARGET xba_title)
        message(FATAL_ERROR "${_xba_title_dir}/CMakeLists.txt defines no xba_title target "
                            "(see templates/new-game/CMakeLists.txt, XBOXRECOMP_SHELL_TITLE)")
    endif()
    target_link_libraries(main PRIVATE xba_title)
    message(STATUS "libmain.so: title from ${_xba_title_dir}")
endif()
