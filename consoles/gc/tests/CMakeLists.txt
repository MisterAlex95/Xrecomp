cmake_minimum_required(VERSION 3.20)
project(console_gc_tests C)

if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()
# The GameCube spike's tests (XBA-225): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_gc) and by the root build's ctest when XBR_CONSOLE_GC is ON. POSIX hosts.
#
#   gc_hello      our libogc hello world (consoles/gc/tests/homebrew/hello) under gc_run, interpreter only: the XFB
#                 after 120 fields must equal hello_xfb.png
#   gc_rc_hello   the same DOL recompiled (gc_tools.recomp + cc, discovery loop), mostly native: same picture
#   gc_fnma       fnmadd / fnmsub and their s / ps_ forms keep an exact zero's -0 and a NaN's sign (no homebrew)
#   gc_gx_<prog>  our libogc GX programs (homebrew gx*) under gc_run --gx: the GLES renderer on a headless EGL
#                 context (llvmpipe), the XFB compared with gx_ref/<prog>.png within a tolerance (run_gx.sh,
#                 gx_compare.py); each program also checks pixels itself by EFB peeks (green square = all passed).
#                 Skipped (77) without the corpus or without a GLES 3 context (XBA-262)
# Both skip (exit 77) when the homebrew is not built (the linux CI job has no corpus; the homebrew job builds it):
#   tools/consoles/build_homebrew.sh gc build/homebrew    -> GC_HOMEBREW_DIR (default build/homebrew/gc)
# The frontend's own tests (synthetic code, no toolchain; one runs a program both interpreted and recompiled) run
# with the Python suite: python3 -m pytest tools/ppc consoles/gc.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET gc_core)
    add_subdirectory(../runtime gc_runtime)
endif()
enable_testing()

get_filename_component(GC_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(GC_HOMEBREW_DIR ${GC_REPO_ROOT}/build/homebrew/gc CACHE PATH "Built GameCube homebrew (build_homebrew.sh gc)")
set(GC_RUN_HELLO ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh)

add_executable(gc_fnma gc_fnma.c)
target_include_directories(gc_fnma PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)
target_compile_options(gc_fnma PRIVATE -Wall -Wextra)
if(NOT APPLE)
    target_link_libraries(gc_fnma PRIVATE m)
endif()
add_test(NAME gc_fnma COMMAND gc_fnma)

if(TARGET gc_run)
    add_test(NAME gc_hello
        COMMAND sh ${GC_RUN_HELLO} $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/hello.dol
                ${CMAKE_CURRENT_BINARY_DIR}/hello_interp interp ${CMAKE_CURRENT_SOURCE_DIR}/hello_xfb.png)
    add_test(NAME gc_rc_hello
        COMMAND sh ${GC_RUN_HELLO} $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/hello.dol
                ${CMAKE_CURRENT_BINARY_DIR}/hello_rc rc ${CMAKE_CURRENT_SOURCE_DIR}/hello_xfb.png)
    set_tests_properties(gc_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    set_tests_properties(gc_rc_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
    # GX (XBA-262): one test per program, gx_ref/<prog>.png the expected XFB; variants: the ubershader alone must
    # give the same pictures (gxtev, gxtex), 2x internal resolution (gxtri, its own reference), EFB copies encoded
    # into RAM too (gxcopy: the program checks the bytes), the per-title shader list written then compiled ahead.
    set(GC_GX_PROGS gxtri gxtex gxtev gxcopy gxind gxfog)
    set(GC_GX_RUN sh ${CMAKE_CURRENT_SOURCE_DIR}/run_gx.sh $<TARGET_FILE:gc_run>)
    foreach(prog ${GC_GX_PROGS})
        add_test(NAME gc_gx_${prog}
            COMMAND ${GC_GX_RUN} ${GC_HOMEBREW_DIR}/${prog}.dol ${CMAKE_CURRENT_BINARY_DIR}/gx_${prog}
                    ${CMAKE_CURRENT_SOURCE_DIR}/gx_ref/${prog}.png)
    endforeach()
    foreach(prog gxtev gxtex)
        add_test(NAME gc_gx_${prog}_uber
            COMMAND ${GC_GX_RUN} ${GC_HOMEBREW_DIR}/${prog}.dol ${CMAKE_CURRENT_BINARY_DIR}/gx_${prog}_uber
                    ${CMAKE_CURRENT_SOURCE_DIR}/gx_ref/${prog}.png 40 --gx-shaders uber)
    endforeach()
    add_test(NAME gc_gx_gxtri_2x
        COMMAND ${GC_GX_RUN} ${GC_HOMEBREW_DIR}/gxtri.dol ${CMAKE_CURRENT_BINARY_DIR}/gx_gxtri_2x
                ${CMAKE_CURRENT_SOURCE_DIR}/gx_ref/gxtri_2x.png 40 --gx-scale 2)
    add_test(NAME gc_gx_gxcopy_ram
        COMMAND ${GC_GX_RUN} ${GC_HOMEBREW_DIR}/gxcopy.dol ${CMAKE_CURRENT_BINARY_DIR}/gx_gxcopy_ram
                ${CMAKE_CURRENT_SOURCE_DIR}/gx_ref/gxcopy_ram.png 40 --gx-copy-ram)
    add_test(NAME gc_gx_list
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_gx_list.sh $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/gxtev.dol
                ${CMAKE_CURRENT_BINARY_DIR}/gx_list)
    set_tests_properties(gc_gx_gxtri_2x gc_gx_gxcopy_ram gc_gx_list gc_gx_gxtev_uber gc_gx_gxtex_uber
        PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400)
    foreach(prog ${GC_GX_PROGS})
        set_tests_properties(gc_gx_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400)
    endforeach()
endif()

# gc_padtest (XBA-259): our libogc pad test (homebrew/padtest) with controller input injected on the serial
# interface: what PAD_Read gave the program (its report in memory) must be what went in, on all four ports, with
# hot plug and rumble; then the same program in PAL (50 Hz fields, 576 lines). Skips (77) without the corpus.
add_executable(gc_padtest gc_padtest.c)
target_link_libraries(gc_padtest PRIVATE gc_core)
target_compile_options(gc_padtest PRIVATE -Wall -Wextra)
add_test(NAME gc_padtest COMMAND gc_padtest ${GC_HOMEBREW_DIR}/padtest.dol)
set_tests_properties(gc_padtest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

# The runtime (XBA-259), where it builds (SDL2 and the host layer: the root build, the linux job):
#   gc_pad_map     a host pad through the player's mapping as a GameCube controller, the DOL's key (no corpus)
#   gc_play_hello  the hello world through gc_runtime, headless: its XFB after 120 fields is hello_xfb.png
#   gc_play_pad    the pad test through gc_runtime with port 1 held (A Z L <, sticks, triggers): padtest_xfb.png
if(TARGET gc_pad_map)
    add_executable(gc_pad_map_test gc_pad_map_test.c)
    target_link_libraries(gc_pad_map_test PRIVATE gc_pad_map)
    target_compile_options(gc_pad_map_test PRIVATE -Wall -Wextra)
    add_test(NAME gc_pad_map COMMAND gc_pad_map_test)
endif()
if(TARGET gc_play)
    set(GC_RUN_PLAY ${CMAKE_CURRENT_SOURCE_DIR}/run_play.sh)
    add_test(NAME gc_play_hello
        COMMAND sh ${GC_RUN_PLAY} $<TARGET_FILE:gc_play> ${GC_HOMEBREW_DIR}/hello.dol
                ${CMAKE_CURRENT_BINARY_DIR}/play_hello ${CMAKE_CURRENT_SOURCE_DIR}/hello_xfb.png --frames 120)
    add_test(NAME gc_play_pad
        COMMAND sh ${GC_RUN_PLAY} $<TARGET_FILE:gc_play> ${GC_HOMEBREW_DIR}/padtest.dol
                ${CMAKE_CURRENT_BINARY_DIR}/play_pad ${CMAKE_CURRENT_SOURCE_DIR}/padtest_xfb.png --frames 120
                --pad 0151:200:30:128:255:77:255)
    # XBA-261: the hello world packed as a zstd RVZ disc, booted by gc_runtime through its apploader
    add_test(NAME gc_play_disc
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_disc.sh play $<TARGET_FILE:gc_play> ${GC_HOMEBREW_DIR}/hello.dol
                ${CMAKE_CURRENT_BINARY_DIR}/play_disc ${CMAKE_CURRENT_SOURCE_DIR}/hello_xfb.png)
    set_tests_properties(gc_play_hello gc_play_pad gc_play_disc PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

# Disc images (XBA-261): gc_disc generates a GCM of our DVD-interface test program and its ISO / CISO / GCZ / RVZ /
# WIA conversions (gc_tools.disc, no toolchain, no game data): same title key (gc_disc and recomp_pack_rom_identify),
# DOL and FST from each, and the GCM boots headless through its apploader with the right DVD results.
# gc_hello_disc packs our libogc hello world into a GCM: same XFB as the DOL (77 without the corpus).
add_executable(gc_disc_info gc_disc_info.c ${GC_REPO_ROOT}/src/platform/recomp_pack.c
               ${GC_REPO_ROOT}/src/platform/recomp_cfg.c)
target_link_libraries(gc_disc_info PRIVATE gc_identify ${CMAKE_DL_LIBS})
target_compile_options(gc_disc_info PRIVATE -Wall -Wextra)
if(TARGET gc_run)
    add_test(NAME gc_disc
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_disc.sh formats $<TARGET_FILE:gc_run> $<TARGET_FILE:gc_disc_info>
                ${CMAKE_CURRENT_BINARY_DIR}/disc)
    add_test(NAME gc_hello_disc
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_disc.sh hello $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/hello.dol
                ${CMAKE_CURRENT_BINARY_DIR}/hello_disc ${CMAKE_CURRENT_SOURCE_DIR}/hello_xfb.png)
    set_tests_properties(gc_disc gc_hello_disc PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()
