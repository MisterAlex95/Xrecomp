# libmain.so of the app shell (XBA-18): SDLActivity loads libSDL2.so, then this, and calls SDL_main.
# A title is linked next to shell_main.c (XBA-52): -DXBA_TITLE_DIR=<title project> (gradle -PxbaTitleDir=...).
cmake_minimum_required(VERSION 3.22)
project(xboxrecomp_shell C)

set(XBA_VERSION "dev" CACHE STRING "Build version shown in the ready line (CI: the commit)")
set(XBA_TITLE_DIR "" CACHE PATH
    "A title project (a copy of templates/new-game with its generated C) to link into libmain.so; empty = shell only")

# The toolkit, from the root of this repository (SDL2 comes in as its subproject on Android).
get_filename_component(XBOXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
# Frame generation (gradle -PxbaLsfg=1, docs/technical/framegen.md): the toolkit's XBOXRECOMP_LSFG, with the Vulkan
# bridge (C++20, libc++ static: ANDROID_STL c++_static, gradle's default) when XBOXRECOMP_LSFG_VK_SRC is given.
# libvulkan.so is dlopened by volk, never linked: the APK still starts on a device without Vulkan.
if(XBOXRECOMP_LSFG)
    enable_language(CXX)
endif()
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
if(TARGET xbox_framegen)
    list(APPEND XBA_TOOLKIT_LIBS xbox_framegen)
endif()
target_link_libraries(main PRIVATE -Wl,--whole-archive ${XBA_TOOLKIT_LIBS} -Wl,--no-whole-archive
                                   SDL2::SDL2 GLESv3 log android m)

# Release flags for the runtime (the software GPU, the kernel, audio): what a frame on the device spends its time in.
# Every toolkit library pins -O2 in its own CMakeLists, after the NDK's per-configuration flags, so the NDK's -O3 for
# Release never reached them; and the NDK adds -fstack-protector-strong and -D_FORTIFY_SOURCE=2 to everything. In a
# release build, -O2 becomes -O3 and both checks go, for these libraries only (SDL2 and the title keep theirs).
# XBA_NATIVE_CPU (gradle -PxbaNativeCpu=...) picks the instruction set: empty (the default) keeps the arm64-v8a
# baseline, so the APK still runs on any arm64 device, and only tunes scheduling for the AYN Thor's big cores
# (Snapdragon 8 Gen 2: Cortex-X3, A715, A710); a core name (cortex-a715) compiles for that core's ISA (ARMv9 with
# SVE2), which a Thor runs and an ARMv8 phone does not. Debug builds are left alone.
set(XBA_NATIVE_CPU "" CACHE STRING "Release: -mcpu for the runtime libraries; empty = arm64 baseline, tuned for Cortex-A715")
if(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(XBA_NATIVE_CPU)
        set(_xba_cpu "-mcpu=${XBA_NATIVE_CPU}")
    else()
        set(_xba_cpu "-mtune=cortex-a715")
    endif()
    foreach(_lib IN LISTS XBA_TOOLKIT_LIBS)
        get_target_property(_type ${_lib} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()
        get_target_property(_opts ${_lib} COMPILE_OPTIONS)
        if(_opts)
            list(REMOVE_ITEM _opts -O2)
            set_property(TARGET ${_lib} PROPERTY COMPILE_OPTIONS ${_opts})
        endif()
        target_compile_options(${_lib} PRIVATE -O3 ${_xba_cpu} -fno-stack-protector -U_FORTIFY_SOURCE)
    endforeach()
    message(STATUS "libmain.so: runtime libraries -O3 ${_xba_cpu}, no stack protector or fortify (${CMAKE_BUILD_TYPE})")
endif()
target_link_options(main PRIVATE -Wl,--no-undefined)

# libxba_import.so (XBA-40, XBA-54): the game picker's ISO import, JNI around src/xiso. A library of its own: the
# picker runs in its own process before SDL, and must not load libSDL2.so and the whole runtime that libmain.so
# brings. The toolkit's root CMakeLists adds src/xiso (target xiso, libc only).
if(NOT TARGET xiso)
    add_subdirectory("${XBOXRECOMP_ROOT}/src/xiso" xiso)
endif()
# It also imports Lossless.dll for frame generation (lsfg_jni.c, LsfgNative.java): the shader-cache helpers are C,
# libc only, and always built. With -PxbaLsfg=1 and a DXVK checkout (XBOXRECOMP_LSFG_DXVK_SRC) the DXBC translator
# is linked in too (C++17, libc++ static), so the phone translates the shaders itself.
include("${XBOXRECOMP_ROOT}/src/framegen/lsfg_shaders.cmake")
add_library(xba_import SHARED xiso_jni.c lsfg_jni.c)
target_link_libraries(xba_import PRIVATE xiso lsfg_shaders log)
if(TARGET lsfg_dxbc)
    target_link_libraries(xba_import PRIVATE lsfg_dxbc)
    target_compile_definitions(xba_import PRIVATE XBA_LSFG_TRANSLATOR=1)
    message(STATUS "libxba_import.so: with the DXBC translator")
endif()
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
