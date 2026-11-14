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
# The N64 Vulkan RDP (XBR_N64_RDP_VULKAN, ON on Android: paraLLEl-RDP, C++17, libc++ static) goes into libmain.so:
# libmain links C++, so CXX is enabled here, at the top (consoles/n64/runtime/CMakeLists.txt). Its libvulkan is
# dlopened too (its own volk, renamed prdp_*: no clash with the LSFG bridge's).
enable_language(CXX)
add_subdirectory("${XBOXRECOMP_ROOT}" xboxrecomp)

add_library(main SHARED shell_main.c pack_loader.c companion_jni.c menu_jni.c)
# companion_jni.c: the second-screen panel; menu_jni.c: the in-game menu (hold, volume, quit)
target_compile_definitions(main PRIVATE XBA_VERSION="${XBA_VERSION}")
# The whole toolkit, not only what shell_main.c calls: System.loadLibrary("main") then checks on the device that
# bionic resolves every symbol the runtime needs (pack_loader.c has weak forwarders for the four a title provides:
# a title linked in wins, otherwise they reach a dlopened title pack, pack_loader.h).
set(XBA_TOOLKIT_LIBS xbox_kernel xbox_d3d8 xbox_dsound xbox_input xbox_input_map xbox_usb xbox_apu xbox_nv2a
                     xbox_video platform host_time recomp_exit_hook recomp_cfg host_cpus
                     recomp_storage recomp_app recomp_selftest recomp_pack
                     xbr_host xbr_host_core input_ports console_pad)   # the host layer (XBA-145)
if(TARGET xbox_framegen)
    list(APPEND XBA_TOOLKIT_LIBS xbox_framegen)
endif()
target_link_libraries(main PRIVATE -Wl,--whole-archive ${XBA_TOOLKIT_LIBS} -Wl,--no-whole-archive
                                   SDL2::SDL2 GLESv3 log android m)
# The other consoles' runtimes (XBA-147, consoles/README.md): pack_loader.c hands a game whose console is gba, ps1 or
# n64 to <console>_runtime_start. All three play games (GBA XBA-157, PS1 XBA-168, N64 XBA-174): the root build turns
# them on for Android, so their targets are there already (added here otherwise).
foreach(_console gba ps1 n64)
    if(NOT TARGET ${_console}_runtime)
        add_subdirectory("${XBOXRECOMP_ROOT}/consoles/${_console}/runtime" ${_console}_runtime)
    endif()
    target_link_libraries(main PRIVATE ${_console}_runtime)
endforeach()
# A GBA title pack (libtitle.so, XBA-157 / XBA-158) is linked --no-undefined against libmain.so and calls the core
# (gba_core.h, gba_recomp_rt.h): the whole core goes in, so every symbol of it is exported.
target_link_libraries(main PRIVATE -Wl,--whole-archive gba_core -Wl,--no-whole-archive)
# The PS1 core goes in whole too (ps1_core.h, ps1_devices.h), exported for the PS1 title packs to come (XBA-168: the
# recompiler's lifted code will call into it as the GBA's does).
target_link_libraries(main PRIVATE -Wl,--whole-archive ps1_core -Wl,--no-whole-archive)
# The N64 core and its RCP devices go in whole too (n64_core.h, n64_devices.h, n64_rdp.h), exported for the N64
# title packs to come (XBA-174: the N64 recompiler's lifted code will call into them as the GBA's does).
target_link_libraries(main PRIVATE -Wl,--whole-archive n64_core n64_rsp n64_rdp -Wl,--no-whole-archive)
# A .chd PlayStation disc's title key (XBA-159): the PS1 disc reader as recomp_pack_rom_identify's disc hook.
target_link_libraries(main PRIVATE ps1_identify)
set(XBA_CONSOLE_FAST_LIBS gba_core gba_runtime gba_pack   # the interpreters: release flags below too
                          ps1_core ps1_gpu ps1_cd ps1_spu ps1_mdec ps1_runtime
                          n64_core n64_rsp n64_rdp n64_rdp_sw n64_runtime)

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
    foreach(_lib IN LISTS XBA_TOOLKIT_LIBS XBA_CONSOLE_FAST_LIBS)
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
# ps1_jni.c (Ps1Native.java, XBA-159): the title key of a .chd PlayStation disc for the library scan
# (Console.identify): recomp_pack_rom_identify with the PS1 disc reader (ps1_disc.c + the vendored libchdr) as its
# disc hook. libc only, no SDL: the launcher process stays free of the runtime.
add_library(xba_import SHARED xiso_jni.c lsfg_jni.c ps1_jni.c)
target_link_libraries(xba_import PRIVATE xiso lsfg_shaders recomp_pack ps1_identify log)
if(TARGET lsfg_dxbc)
    target_link_libraries(xba_import PRIVATE lsfg_dxbc)
    target_compile_definitions(xba_import PRIVATE XBA_LSFG_TRANSLATOR=1)
    message(STATUS "libxba_import.so: with the DXBC translator")
endif()
target_link_options(xba_import PRIVATE -Wl,--no-undefined)

# libxba_vkdriver.so: a Vulkan driver the player supplied (Mesa Turnip...) for the N64 Vulkan RDP, through the vendored
# libadrenotools (third_party/adrenotools, BSD-2, arm64 only). Not linked by libmain.so: the N64 runtime dlopens it
# only when a custom driver is selected (consoles/n64/runtime/n64_vk_driver.c), so linkernsbypass's static constructor
# never runs otherwise. adrenotools' four hook libraries (libhook_impl, libmain_hook, libfile_redirect_hook,
# libgsl_alloc_hook) are SHARED targets of its own CMakeLists: packaged next to it (no `targets` filter in gradle) and
# loaded by name from the native library directory at run time.
set(XBA_ADRENOTOOLS_DIR "${XBOXRECOMP_ROOT}/third_party/adrenotools")
if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a" AND EXISTS "${XBA_ADRENOTOOLS_DIR}/CMakeLists.txt")
    add_subdirectory("${XBA_ADRENOTOOLS_DIR}" adrenotools)
    foreach(_t adrenotools linkernsbypass hook_impl main_hook file_redirect_hook gsl_alloc_hook)
        # C++20 designated initialisers, no standard set upstream; not our code: quiet
        set_target_properties(${_t} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
        target_compile_options(${_t} PRIVATE -w)
    endforeach()
    add_library(xba_vkdriver SHARED vkdriver.c)
    set_target_properties(xba_vkdriver PROPERTIES C_VISIBILITY_PRESET hidden)
    target_link_libraries(xba_vkdriver PRIVATE adrenotools log)
    target_link_options(xba_vkdriver PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL)
    message(STATUS "libxba_vkdriver.so: custom Vulkan drivers through libadrenotools")
endif()

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
