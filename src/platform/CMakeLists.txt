cmake_minimum_required(VERSION 3.20)
project(platform C)

# Platform layer
#   xbox_winnt.h   - Win32 / NT type vocabulary (header only)
#   win32_compat.* - POSIX implementation of generic Win32 host primitives
#
# On Windows the real <windows.h> is used, so this is an INTERFACE library
# that only contributes the src/ include path. On Linux it is a real static
# library carrying the POSIX primitive implementations.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# src/, the directory this one lives in. Resolved relative to here rather than
# via CMAKE_SOURCE_DIR, which points at the *consuming* project's root when a
# game project pulls xboxrecomp in with add_subdirectory() -- the documented
# integration pattern -- and would send "platform/xbox_winnt.h" looking in the
# game's own src/ tree. PROJECT_SOURCE_DIR is no good either: this file
# declares its own project().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# The console-agnostic core (recomp_exit_hook, host_time, recomp_cfg, host_cpus, recomp_storage, xbr_host_core):
# host_core.cmake, shared with src/host (xbr_host, XBA-145). See HOST_LAYER.md.
include(${CMAKE_CURRENT_SOURCE_DIR}/host_core.cmake)

if(NOT WIN32)
    # recomp_app: a title's start-up on a host without a shell, in one call
    # (recomp_app.h, XBA-52): logcat, recomp.cfg, host defaults, storage,
    # fast cores, fault handler. Calls the kernel (xbox_path_init, the fault
    # handler), hence a library of its own; the Android app shell links it.
    add_library(recomp_app STATIC recomp_app.c)
    target_include_directories(recomp_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(recomp_app PUBLIC recomp_storage recomp_cfg host_cpus platform xbox_kernel)

    # recomp_pack: a title pack, the loader's side (recomp_pack.h): the
    # XBE's title ID, pack.json (its env_force through recomp_cfg), dlopen.
    # libc + libdl + recomp_cfg only; the Android app
    # shell links it (pack_loader.c), tests/recomp_pack on Linux.
    add_library(recomp_pack STATIC recomp_pack.c)
    target_include_directories(recomp_pack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(recomp_pack PUBLIC recomp_cfg ${CMAKE_DL_LIBS})
endif()

if(WIN32)
    add_library(platform INTERFACE)
    target_link_libraries(platform INTERFACE host_time recomp_cfg host_cpus)
    target_include_directories(platform INTERFACE ${XBOXRECOMP_SRC_DIR})
    target_link_libraries(platform INTERFACE recomp_exit_hook)
else()
    # win32_compat.c here; the log pump, the lifecycle and the quit path are the host layer's (xbr_host_core).
    add_library(platform STATIC win32_compat.c)
    target_include_directories(platform PUBLIC
        ${XBOXRECOMP_SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    find_package(Threads REQUIRED)
    target_link_libraries(platform PUBLIC Threads::Threads xbr_host_core recomp_exit_hook host_time recomp_cfg host_cpus)
    if(NOT APPLE)
        target_link_libraries(platform PUBLIC m)   # feclearexcept etc.; libSystem has them on macOS
    endif()
    target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
