cmake_minimum_required(VERSION 3.20)
project(console_x360_tests C)

# The Xbox 360 runtime's tests (XBA-270): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_x360) and by the root build's ctest when XBR_CONSOLE_X360 is ON. Linux only.
#
#   xenos_cp         the PM4 command processor on hand-built packets (xenos_cp_test.c)
#   xenos_draw       the GLES renderer draws a hand-built PM4 stream, read back (skipped without EGL / GLES 3.0):
#                    its frames, among them (XBA-280) eDRAM surfaces at tile bases, a 2_10_10_10 and a float target
#                    resolved to their formats, a depth resolve, mip levels, a computed-index fetch; xenos_draw_x2 the
#                    same at an internal resolution of 2
#   x360_render      the renderer through the machine: a ring, VdSwap, a front buffer frame, a save state loaded
#   x360_pad_map     the title key and a host pad as a 360 controller (x360_runtime.h's pure helpers; XBA-280)
#   x360_window      the renderer on a window's context (xenos_gl_attach at scale 2, the frame texture presented,
#                    the drawable captured): needs a display (xvfb-run -a ctest -R x360_window), 77 without
#   x360_play        the test title played by x360_play headless, frame by frame, to its end (run_play.sh;
#                    built when SDL2 is there)
#   x360_hle         the kernel HLE called directly (x360_hle_test.c): formatters, memory, objects, waits, timers,
#                    threads, files and symbolic links
#   x360_kdpc, x360_rtl, x360_crypt2, x360_vd, x360_xam2, x360_ob   the XBA-296 kernel groups called directly (DPCs, APCs,
#                    timers, queues, SLists; strings and printf; hashes and ciphers on published vectors; Vd and audio;
#                    XAM)
#   x360_hle_table   hle_coverage's reading of the sources against the runtime's real dispatch table
#   x360_hello       the synthetic test title (make_hello.py: our own PowerPC assembly in a synthetic XEX2, imports
#                    by their real ordinals) recompiled and run under x360_run: its DbgPrint text must equal
#                    hello.txt (memory, critical section, TLS, a second thread and an event, a delay, a semaphore,
#                    a jump table) and it must end with exit code 0
#   x360_hello_lzx   the same title encrypted (a made-up key) and LZX-compressed, rewritten plain by
#                    python3 -m tools.xex --plain first
#   x360_state       save states: the test title stopped by an instruction budget, a state saved, a fresh run
#                    resumed from it: the two halves of the DbgPrint text make hello.txt (run_state.sh)
#   x360_msr         two threads' shared counter inside mtmsrd (MSR[EE] clear) sections: no update lost; the same
#                    without mtmsrd loses updates (run_msr.sh, make_msr.py)
#   x360_bringup     python3 -m x360_tools.bringup on the encrypted LZX test title (run_bringup.sh)
#   x360_missed      the discovery loop: a function only a pointer reaches stops the first run (--missed), the
#                    second, recompiled with it (--discovered), ends with exit code 42 (run_missed.sh)
#   x360_hello_c     a C test title (tests/homebrew/hello_c: devkitPPC, -mpowerpc64 -maltivec; recursion, a GCC jump
#                    table, function pointers, doubles, AltiVec, two threads with a critical section and events,
#                    virtual / physical memory) packed by homebrew/elf2xex.py: hello_c.txt. Skipped (77) when not
#                    built (the linux CI job has no devkitPPC): tools/consoles/build_homebrew.sh x360 build/homebrew
# The recompiler's own tests (synthetic code) run with the Python suite: python3 -m pytest consoles/.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # macOS (tools/run_c_tests.sh configures every consoles/*/tests there too): one test that skips, nothing built
    enable_testing()
    add_test(NAME x360_linux_only COMMAND sh -c "echo 'skipped: the Xbox 360 runtime is Linux-only (memfd_create)'; exit 77")
    set_tests_properties(x360_linux_only PROPERTIES SKIP_RETURN_CODE 77)
    return()
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET x360_core)
    add_subdirectory(../runtime x360_runtime)
endif()
enable_testing()

add_executable(xenos_cp_test xenos_cp_test.c)
target_link_libraries(xenos_cp_test PRIVATE x360_core)
target_compile_options(xenos_cp_test PRIVATE -Wall -Wextra)
add_test(NAME xenos_cp COMMAND xenos_cp_test)
set_tests_properties(xenos_cp PROPERTIES TIMEOUT 60)

add_executable(xenos_draw_test xenos_draw_test.c)
target_link_libraries(xenos_draw_test PRIVATE x360_core)
target_compile_options(xenos_draw_test PRIVATE -Wall -Wextra)
add_test(NAME xenos_draw COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_draw.sh $<TARGET_FILE:xenos_draw_test>
                                    ${CMAKE_CURRENT_BINARY_DIR}/xenos_draw)
set_tests_properties(xenos_draw PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
# the same frames at an internal resolution of 2 (XBA-280): surfaces, resolves and frames read back at the guest size
add_test(NAME xenos_draw_x2 COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_draw.sh $<TARGET_FILE:xenos_draw_test>
                                       ${CMAKE_CURRENT_BINARY_DIR}/xenos_draw_x2)
set_tests_properties(xenos_draw_x2 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 ENVIRONMENT XENOS_DRAW_SCALE=2)
add_test(NAME x360_render COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_draw.sh $<TARGET_FILE:x360_hle_test>
                                     ${CMAKE_CURRENT_BINARY_DIR}/x360_render --render)
set_tests_properties(x360_render PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
# run_draw.sh starts our binaries itself: ctest's CMAKE_CROSSCOMPILING_EMULATOR wraps only its sh, so a cross build
# hands the emulator over as XBR_TEST_EMULATOR, comma separated (as consoles/gc/tests).
if(CMAKE_CROSSCOMPILING_EMULATOR)
    list(JOIN CMAKE_CROSSCOMPILING_EMULATOR "," _x360_emu)
    set_property(TEST xenos_draw xenos_draw_x2 x360_render APPEND PROPERTY ENVIRONMENT "XBR_TEST_EMULATOR=${_x360_emu}")
endif()

if(TARGET x360_pad_map)
    add_executable(x360_pad_map_test x360_pad_map_test.c)
    target_link_libraries(x360_pad_map_test PRIVATE x360_pad_map)
    add_test(NAME x360_pad_map COMMAND x360_pad_map_test)
endif()
if(TARGET x360_runtime)
    add_executable(x360_window_test x360_window_test.c)
    target_link_libraries(x360_window_test PRIVATE x360_core xbr_host)
    target_compile_options(x360_window_test PRIVATE -Wall -Wextra)
    add_test(NAME x360_window COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_window.sh $<TARGET_FILE:x360_window_test>
                                       ${CMAKE_CURRENT_BINARY_DIR}/x360_window)
    set_tests_properties(x360_window PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
if(TARGET x360_play)
    add_test(NAME x360_play COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_play.sh $<TARGET_FILE:x360_play>
                                     ${CMAKE_CURRENT_BINARY_DIR}/x360_play)
    set_tests_properties(x360_play PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# XMA through libavcodec (XBA-299): only where the runtime found it
if(X360_AV_FOUND)
    add_executable(x360_xma_av_test x360_xma_av_test.c)
    target_link_libraries(x360_xma_av_test PRIVATE x360_core)
    add_test(NAME x360_xma_av COMMAND x360_xma_av_test)
    set_tests_properties(x360_xma_av PROPERTIES TIMEOUT 120)
endif()
add_executable(x360_hle_test x360_hle_test.c)
target_link_libraries(x360_hle_test PRIVATE x360_core)
target_compile_options(x360_hle_test PRIVATE -Wall -Wextra)
add_test(NAME x360_hle COMMAND x360_hle_test)
set_tests_properties(x360_hle PROPERTIES TIMEOUT 120)
# XBA-296: the kernel groups, one test program each (x360_hle_util.h: the calling helpers)
foreach(_group kdpc rtl crypt2 vd xam2 ob)
    add_executable(x360_${_group}_test x360_${_group}_test.c)
    target_link_libraries(x360_${_group}_test PRIVATE x360_core)
    target_compile_options(x360_${_group}_test PRIVATE -Wall -Wextra)
    add_test(NAME x360_${_group} COMMAND x360_${_group}_test)
    set_tests_properties(x360_${_group} PROPERTIES TIMEOUT 120)
endforeach()
# XBA-296: hle_coverage's reading of the sources is exactly the runtime's dispatch table
add_test(NAME x360_hle_table
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_hle_table.sh $<TARGET_FILE:x360_hle_test>
            ${CMAKE_CURRENT_BINARY_DIR}/x360_hle_table)
set_tests_properties(x360_hle_table PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

if(TARGET x360_run)
    foreach(_mode basic lzx split)
        if(_mode STREQUAL basic)
            set(_name x360_hello)
        else()
            set(_name x360_hello_${_mode})
        endif()
        add_test(NAME ${_name}
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh $<TARGET_FILE:x360_run>
                    ${CMAKE_CURRENT_BINARY_DIR}/${_name} ${_mode} ${CMAKE_CURRENT_SOURCE_DIR}/hello.txt)
        set_tests_properties(${_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    endforeach()
    add_test(NAME x360_state
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_state.sh $<TARGET_FILE:x360_run>
                ${CMAKE_CURRENT_BINARY_DIR}/x360_state ${CMAKE_CURRENT_SOURCE_DIR}/hello.txt)
    set_tests_properties(x360_state PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    add_test(NAME x360_msr
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_msr.sh $<TARGET_FILE:x360_run> ${CMAKE_CURRENT_BINARY_DIR}/x360_msr)
    set_tests_properties(x360_msr PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    add_test(NAME x360_bringup
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_bringup.sh $<TARGET_FILE:x360_run>
                ${CMAKE_CURRENT_BINARY_DIR}/x360_bringup)
    set_tests_properties(x360_bringup PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    add_test(NAME x360_missed
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_missed.sh $<TARGET_FILE:x360_run>
                ${CMAKE_CURRENT_BINARY_DIR}/x360_missed)
    set_tests_properties(x360_missed PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    # XBA-282: structured exceptions (x360_seh.c): __except, __finally, a filter that continues (make_seh.py)
    add_test(NAME x360_seh
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_seh.sh $<TARGET_FILE:x360_run> ${CMAKE_CURRENT_BINARY_DIR}/x360_seh
                ${CMAKE_CURRENT_SOURCE_DIR}/seh.txt)
    set_tests_properties(x360_seh PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    get_filename_component(X360_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    set(X360_HOMEBREW_DIR ${X360_REPO_ROOT}/build/homebrew/x360 CACHE PATH
        "Built Xbox 360 test titles (build_homebrew.sh x360)")
    add_test(NAME x360_hello_c
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh $<TARGET_FILE:x360_run>
                ${CMAKE_CURRENT_BINARY_DIR}/x360_hello_c c ${CMAKE_CURRENT_SOURCE_DIR}/hello_c.txt
                ${X360_HOMEBREW_DIR}/hello_c.elf)
    set_tests_properties(x360_hello_c PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    # XBA-283: functions left out of the pack on purpose are interpreted, the run goes on and names them all at once
    add_test(NAME x360_interp
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_interp.sh $<TARGET_FILE:x360_run>
                ${CMAKE_CURRENT_BINARY_DIR}/x360_interp basic ${CMAKE_CURRENT_SOURCE_DIR}/hello.txt "" --all)
    set_tests_properties(x360_interp PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    add_test(NAME x360_interp_c
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_interp.sh $<TARGET_FILE:x360_run>
                ${CMAKE_CURRENT_BINARY_DIR}/x360_interp_c c ${CMAKE_CURRENT_SOURCE_DIR}/hello_c.txt
                ${X360_HOMEBREW_DIR}/hello_c.elf --all)
    set_tests_properties(x360_interp_c PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    # XBA-283: guest threads on host threads (x360_run --host-threads, experimental) and atomic reservations
    add_test(NAME x360_host_threads
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_host_threads.sh $<TARGET_FILE:x360_run>
                ${CMAKE_CURRENT_BINARY_DIR}/x360_host_threads ${X360_HOMEBREW_DIR})
    set_tests_properties(x360_host_threads PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()
