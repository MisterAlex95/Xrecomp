# The console-agnostic core of the host layer (src/platform/HOST_LAYER.md, XBA-145): libc / pthreads only, no SDL
# and no Xbox source. Included by src/platform/CMakeLists.txt (the Xbox runtime) and by src/host/CMakeLists.txt
# (xbr_host, what every console runtime links), so either can be configured alone; the guard keeps one definition.
#   recomp_exit_hook  work to run when a window ends the process with ExitProcess (recomp_exit_hook.h)
#   host_time         the monotonic clock and the timer thread's sleep (host_time.h)
#   recomp_cfg        RECOMP_* options from recomp.cfg on hosts without a shell (recomp_cfg.h)
#   host_cpus         run on the fast cores of a big.LITTLE host (host_cpus.h)
#   host_perf_hint    a console runtime's frame loop as an Android performance hint session (host_perf_hint.h),
#                     POSIX; libandroid's ADPF looked up at run time, a no-op elsewhere
#   host_perf_line    the performance line every console logs every 10 s and at exit, and the overlay's live
#                     figures (host_perf_line.h, XBA-243), POSIX
#   recomp_storage    a title's data and save directories on a host without a shell (recomp_storage.h), POSIX
#   xbr_host_core     the log pump (android_log.c), the app lifecycle (recomp_lifecycle.c), the quit path
#                     (recomp_quit.c), the consoles' save states / fast-forward (recomp_states.c, XBA-182) and
#                     hotkeys (recomp_hotkeys.c, XBA-190), rewind (recomp_rewind.c, XBA-194), cheats
#                     (recomp_cheats.c, XBA-197),
#                     POSIX; links the first four, host_perf_hint and host_perf_line (xbr_host adds recomp_storage)
if(NOT TARGET host_time)
    get_filename_component(_xbr_core_src "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

    add_library(recomp_exit_hook STATIC ${CMAKE_CURRENT_LIST_DIR}/recomp_exit_hook.c)
    target_include_directories(recomp_exit_hook PUBLIC ${CMAKE_CURRENT_LIST_DIR})

    add_library(host_time STATIC ${CMAKE_CURRENT_LIST_DIR}/host_time.c)
    target_include_directories(host_time PUBLIC ${_xbr_core_src} ${CMAKE_CURRENT_LIST_DIR})

    add_library(recomp_cfg STATIC ${CMAKE_CURRENT_LIST_DIR}/recomp_cfg.c)
    target_include_directories(recomp_cfg PUBLIC ${CMAKE_CURRENT_LIST_DIR})

    add_library(host_cpus STATIC ${CMAKE_CURRENT_LIST_DIR}/host_cpus.c)
    target_include_directories(host_cpus PUBLIC ${CMAKE_CURRENT_LIST_DIR})

    if(NOT WIN32)
        find_package(Threads REQUIRED)
        add_library(host_perf_hint STATIC ${CMAKE_CURRENT_LIST_DIR}/host_perf_hint.c)
        target_include_directories(host_perf_hint PUBLIC ${CMAKE_CURRENT_LIST_DIR})
        target_link_libraries(host_perf_hint PUBLIC host_cpus Threads::Threads ${CMAKE_DL_LIBS})
        target_compile_options(host_perf_hint PRIVATE -Wall -Wextra)
        set_target_properties(host_perf_hint PROPERTIES POSITION_INDEPENDENT_CODE ON)

        add_library(host_perf_line STATIC ${CMAKE_CURRENT_LIST_DIR}/host_perf_line.c)
        target_include_directories(host_perf_line PUBLIC ${CMAKE_CURRENT_LIST_DIR})
        target_link_libraries(host_perf_line PUBLIC Threads::Threads)
        target_compile_options(host_perf_line PRIVATE -Wall -Wextra)
        set_target_properties(host_perf_line PROPERTIES POSITION_INDEPENDENT_CODE ON)

        # POSIX hosts (dirent); the Android app shell links it.
        add_library(recomp_storage STATIC ${CMAKE_CURRENT_LIST_DIR}/recomp_storage.c)
        target_include_directories(recomp_storage PUBLIC ${CMAKE_CURRENT_LIST_DIR})

        # The three were part of `platform` (next to win32_compat.c) until XBA-145; platform links this now.
        add_library(xbr_host_core STATIC
            ${CMAKE_CURRENT_LIST_DIR}/android_log.c
            ${CMAKE_CURRENT_LIST_DIR}/recomp_lifecycle.c
            ${CMAKE_CURRENT_LIST_DIR}/recomp_quit.c
            ${CMAKE_CURRENT_LIST_DIR}/recomp_states.c
            ${CMAKE_CURRENT_LIST_DIR}/recomp_hotkeys.c
            ${CMAKE_CURRENT_LIST_DIR}/recomp_rewind.c
            ${CMAKE_CURRENT_LIST_DIR}/recomp_cheats.c)
        target_include_directories(xbr_host_core PUBLIC ${_xbr_core_src} ${CMAKE_CURRENT_LIST_DIR})
        find_package(Threads REQUIRED)
        target_link_libraries(xbr_host_core PUBLIC Threads::Threads recomp_exit_hook host_time recomp_cfg host_cpus
                              host_perf_hint host_perf_line)
        if(NOT APPLE)
            target_link_libraries(xbr_host_core PUBLIC m)   # libSystem has libm on macOS
        endif()
        if(ANDROID)
            target_link_libraries(xbr_host_core PUBLIC log)   # android_log.c: stdout / stderr -> logcat
        endif()
        target_compile_options(xbr_host_core PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
    endif()
endif()
