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
#   gc_rc_hello   the same DOL recompiled (gc_tools.recomp + cc, discovery loop), mostly native: same picture; then a
#                 save-state round trip with the pack (gc_run --state-check, XBA-264)
#   gc_fnma       fnmadd / fnmsub and their s / ps_ forms keep an exact zero's -0 and a NaN's sign (no homebrew)
#   gc_gx_<prog>  our libogc GX programs (homebrew gx*) under gc_run --gx: the GLES renderer on a headless EGL
#                 context (llvmpipe), the XFB compared with gx_ref/<prog>.png within a tolerance (run_gx.sh,
#                 gx_compare.py); each program also checks pixels itself by EFB peeks (green square = all passed).
#                 Skipped (77) without the corpus or without a GLES 3 context (XBA-262)
#   gc_gx_progcache  the program binary cache file format (no GL); gc_gx_programs: program binaries end to end
#                 (warm-up, start, driver change, corrupt file; XBA-264)
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

# gc_exi (XBA-264): the EXI devices driven through their registers (memory card commands, RTC, SRAM), no homebrew
add_executable(gc_exi_test gc_exi_test.c)
target_link_libraries(gc_exi_test PRIVATE gc_core)
target_compile_options(gc_exi_test PRIVATE -Wall -Wextra)
add_test(NAME gc_exi COMMAND gc_exi_test)
# gc_cardtest (XBA-264): our libogc memory card test (homebrew/cardtest) on the core: a blank card formatted, a file
# created, written and read back, found again by a second machine on the same card; 59 / 251 / 1019 blocks; the RTC
# and SRAM as libogc reads them. Skips (77) without the corpus.
add_executable(gc_cardtest gc_cardtest.c)
target_link_libraries(gc_cardtest PRIVATE gc_core)
target_compile_options(gc_cardtest PRIVATE -Wall -Wextra)
add_test(NAME gc_cardtest COMMAND gc_cardtest ${GC_HOMEBREW_DIR}/cardtest.dol)
set_tests_properties(gc_cardtest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
# gx_progcache (XBA-264): the GX program binary cache file without GL (save / load, another key, truncated, corrupt)
add_executable(gc_gx_progcache_test gx_progcache_test.c)
target_link_libraries(gc_gx_progcache_test PRIVATE gc_core)
target_compile_options(gc_gx_progcache_test PRIVATE -Wall -Wextra)
add_test(NAME gc_gx_progcache COMMAND gc_gx_progcache_test ${CMAKE_CURRENT_BINARY_DIR})
# gx_ring: the bookkeeping of RECOMP_GC_GX_STREAM=storage against a simulated GPU (no GL): no byte the GPU may still read
# is ever written, across wraps, with any alignment
add_executable(gc_gx_ring_test gx_ring_test.c)
target_include_directories(gc_gx_ring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gx)
target_compile_options(gc_gx_ring_test PRIVATE -Wall -Wextra)
add_test(NAME gc_gx_ring COMMAND gc_gx_ring_test)
# gx_batch: the renderer's draw batch on a headless GLES 3 context (llvmpipe): points and line strips (4 vertices per
# quad) flush the batch before they would overflow it (77 without a context)
add_executable(gc_gx_batch_test gx_batch_test.c)
target_link_libraries(gc_gx_batch_test PRIVATE gc_core)
target_compile_options(gc_gx_batch_test PRIVATE -Wall -Wextra)
add_test(NAME gc_gx_batch COMMAND gc_gx_batch_test)
set_tests_properties(gc_gx_batch PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 ENVIRONMENT LIBGL_ALWAYS_SOFTWARE=1)

if(TARGET gc_run)
    add_test(NAME gc_hello
        COMMAND sh ${GC_RUN_HELLO} $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/hello.dol
                ${CMAKE_CURRENT_BINARY_DIR}/hello_interp interp ${CMAKE_CURRENT_SOURCE_DIR}/hello_xfb.png)
    add_test(NAME gc_rc_hello
        COMMAND sh ${GC_RUN_HELLO} $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/hello.dol
                ${CMAKE_CURRENT_BINARY_DIR}/hello_rc rc ${CMAKE_CURRENT_SOURCE_DIR}/hello_xfb.png 60:60)
    set_tests_properties(gc_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    set_tests_properties(gc_rc_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
    # gc_relload (XBA-264): our homebrew links REL modules at run time (homebrew/relload: a small OSLink; the modules
    # made by gc_tools.elf2rel, put on a GCM); the runtime records them, gc_tools.recomp lifts them at their load
    # address (two modules at one address), the pack runs them natively with the same results (run_relload.py).
    add_test(NAME gc_relload
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/run_relload.py $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}
                ${CMAKE_CURRENT_BINARY_DIR}/relload)
    set_tests_properties(gc_relload PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
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
    # XBA-264: program binaries: warm-up from the list, all loaded at the next start, another driver / a corrupt
    # file ignored and rebuilt, programs compiled during play kept (run_gx_programs.sh)
    add_test(NAME gc_gx_programs
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_gx_programs.sh $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/gxtev.dol
                ${CMAKE_CURRENT_BINARY_DIR}/gx_programs ${CMAKE_CURRENT_SOURCE_DIR}/gx_ref/gxtev.png)
    set_tests_properties(gc_gx_gxtri_2x gc_gx_gxcopy_ram gc_gx_list gc_gx_gxtev_uber gc_gx_gxtex_uber gc_gx_programs
        PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400)
    foreach(prog ${GC_GX_PROGS})
        set_tests_properties(gc_gx_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400)
    endforeach()
endif()

# Save states (XBA-264): gc_state_format, the format on a synthetic program (round trip, title key, version, damaged /
# truncated states, unknown / missing sections, the cheats' memory access; no corpus); gc_state_<prog>, our homebrew
# saved, run on, loaded and run again with identical XFBs and state (run_state.sh, gc_run --state-check; 77 without
# the corpus, the GX one without a GLES 3 context; asnd's with the DSP's state). gc_rc_hello checks the same with its
# recompiled pack.
add_executable(gc_state_test gc_state_test.c)
target_link_libraries(gc_state_test PRIVATE gc_core)
target_compile_options(gc_state_test PRIVATE -Wall -Wextra)
add_test(NAME gc_state_format COMMAND gc_state_test)
set_tests_properties(gc_state_format PROPERTIES TIMEOUT 120)
if(TARGET gc_run)
    set(GC_RUN_STATE sh ${CMAKE_CURRENT_SOURCE_DIR}/run_state.sh $<TARGET_FILE:gc_run>)
    add_test(NAME gc_state_hello COMMAND ${GC_RUN_STATE} ${GC_HOMEBREW_DIR}/hello.dol 60:60)
    add_test(NAME gc_state_padtest COMMAND ${GC_RUN_STATE} ${GC_HOMEBREW_DIR}/padtest.dol 30:90
             --pad 0:0151:200:30:128:255:77:255)
    add_test(NAME gc_state_gxcopy COMMAND ${GC_RUN_STATE} ${GC_HOMEBREW_DIR}/gxcopy.dol 20:20 --gx)
    add_test(NAME gc_state_asnd COMMAND ${GC_RUN_STATE} ${GC_HOMEBREW_DIR}/asnd.dol 100:120)
    set_tests_properties(gc_state_hello gc_state_padtest gc_state_gxcopy gc_state_asnd
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    # Rewind (gc_run --rewind, recomp_rewind_check): 600 fields of the pad test with pseudo-random pads, a capture
    # every 4 fields, 45 steps back: the picture there and every field after it as the first time.
    add_test(NAME gc_rewind_padtest COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_state.sh $<TARGET_FILE:gc_run>
             ${GC_HOMEBREW_DIR}/padtest.dol rewind --frames 600 --rewind 4:45)
    set_tests_properties(gc_rewind_padtest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

# Cheats (XBA-264): gc_cheats_core, Gecko writes / C2 hooks / AR codes on a synthetic program run by the core (the
# CPU runs the hook and branches back; turned off, the instruction comes back); gc_cheats_padtest, codes writing into
# our pad test's report, the program counting on from their values (77 without the corpus). The engine's own tests:
# tests/recomp_cheats.
add_executable(gc_cheats_test gc_cheats_test.c ${GC_REPO_ROOT}/src/platform/recomp_cheats.c)
target_include_directories(gc_cheats_test PRIVATE ${GC_REPO_ROOT}/src/platform)
target_link_libraries(gc_cheats_test PRIVATE gc_core)
target_compile_options(gc_cheats_test PRIVATE -Wall -Wextra)
add_test(NAME gc_cheats_core COMMAND gc_cheats_test)
add_test(NAME gc_cheats_padtest COMMAND gc_cheats_test ${GC_HOMEBREW_DIR}/padtest.dol)
set_tests_properties(gc_cheats_core gc_cheats_padtest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

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
    # XBA-264: the memory card test through gc_runtime with a save dir: cardA.raw across two launches, written at exit
    # and two seconds after the game's writes, 59 / 251 / 1019 blocks, slot B, a foreign file left alone
    add_test(NAME gc_play_card
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_card.sh $<TARGET_FILE:gc_play> ${GC_HOMEBREW_DIR}/cardtest.dol
                ${CMAKE_CURRENT_BINARY_DIR}/play_card)
    set_tests_properties(gc_play_card PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    # XBA-274: frame generation of GX's GPU picture (read back for the pacer), the same as drawn; needs a display
    add_test(NAME gc_framegen
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_framegen.sh $<TARGET_FILE:gc_play> ${GC_HOMEBREW_DIR}/gxtri.dol
                ${CMAKE_CURRENT_BINARY_DIR}/framegen)
    set_tests_properties(gc_framegen PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

# Disc images (XBA-261): gc_disc generates a GCM of our DVD-interface test program and its ISO / CISO / GCZ / RVZ /
# WIA conversions (gc_tools.disc, no toolchain, no game data): same title key (gc_disc and recomp_pack_rom_identify),
# DOL and FST from each, and the GCM boots headless through its apploader with the right DVD results.
# gc_hello_disc packs our libogc hello world into a GCM: same XFB as the DOL (77 without the corpus).
# gc_disc_bad: damaged images built in memory (a DOL section end past 4 GB, WIA tables larger than the file,
# garbage behind each magic): refused or read, never a crash or a huge allocation
add_executable(gc_disc_bad_test gc_disc_bad_test.c)
target_link_libraries(gc_disc_bad_test PRIVATE gc_core)
target_compile_options(gc_disc_bad_test PRIVATE -Wall -Wextra)
add_test(NAME gc_disc_bad COMMAND gc_disc_bad_test)
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

# Sound (XBA-263): gc_audio needs no toolchain and no game data (unit checks of the stream ADPCM, the accelerator,
# the resampler; AX voices rendered through the DSP interface's mailboxes and played by the audio DMA; a disc stream
# through the DVD and audio interfaces; WAVs against audio_check.py's references). gc_asnd: our libasnd homebrew
# under gc_run, its output scene by scene (77 without the corpus); gc_play_asnd: the same through gc_runtime and the
# host audio path.
add_executable(gc_audio_test gc_audio_test.c)
target_include_directories(gc_audio_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)
target_link_libraries(gc_audio_test PRIVATE gc_core)
target_compile_options(gc_audio_test PRIVATE -Wall -Wextra)
set(GC_RUN_AUDIO ${CMAKE_CURRENT_SOURCE_DIR}/run_audio.sh)
add_test(NAME gc_audio COMMAND sh ${GC_RUN_AUDIO} unit $<TARGET_FILE:gc_audio_test> ${CMAKE_CURRENT_BINARY_DIR}/audio)
set_tests_properties(gc_audio PROPERTIES TIMEOUT 600)
# Zelda / JAudio ucode and AESND (XBA-266): gc_zelda needs no toolchain and no game data (images of ours forged to the
# versions' hashes; the protocols through the mailboxes, voices checked sample by sample against our models of the
# formats, zelda.wav against audio_check.py). gc_aesnd / gc_play_aesnd: our libaesnd homebrew (77 without the corpus).
add_executable(gc_zelda_test gc_zelda_test.c)
target_include_directories(gc_zelda_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)
target_link_libraries(gc_zelda_test PRIVATE gc_core)
target_compile_options(gc_zelda_test PRIVATE -Wall -Wextra)
add_test(NAME gc_zelda COMMAND sh ${GC_RUN_AUDIO} zelda $<TARGET_FILE:gc_zelda_test> ${CMAKE_CURRENT_BINARY_DIR}/zelda)
set_tests_properties(gc_zelda PROPERTIES TIMEOUT 600)
if(TARGET gc_run)
    add_test(NAME gc_aesnd COMMAND sh ${GC_RUN_AUDIO} aesnd $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/aesnd.dol
                                   ${CMAKE_CURRENT_BINARY_DIR}/aesnd)
    set_tests_properties(gc_aesnd PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
if(TARGET gc_play)
    add_test(NAME gc_play_aesnd COMMAND sh ${GC_RUN_AUDIO} play $<TARGET_FILE:gc_play> ${GC_HOMEBREW_DIR}/aesnd.dol
                                        ${CMAKE_CURRENT_BINARY_DIR}/play_aesnd aesnd)
    set_tests_properties(gc_play_aesnd PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
if(TARGET gc_run)
    add_test(NAME gc_asnd COMMAND sh ${GC_RUN_AUDIO} asnd $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/asnd.dol
                                  ${CMAKE_CURRENT_BINARY_DIR}/asnd)
    set_tests_properties(gc_asnd PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
if(TARGET gc_play)
    add_test(NAME gc_play_asnd COMMAND sh ${GC_RUN_AUDIO} play $<TARGET_FILE:gc_play> ${GC_HOMEBREW_DIR}/asnd.dol
                                       ${CMAKE_CURRENT_BINARY_DIR}/play_asnd)
    set_tests_properties(gc_play_asnd PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

# The helper scripts above start our binaries themselves: ctest's CMAKE_CROSSCOMPILING_EMULATOR wraps only a test's
# COMMAND (here sh or python3), so a cross build (qemu-aarch64 in the arm64 CI) hands them the emulator as
# XBR_TEST_EMULATOR, comma separated (as tests/park_resume), and each script runs its binaries under it.
if(CMAKE_CROSSCOMPILING_EMULATOR)
    list(JOIN CMAKE_CROSSCOMPILING_EMULATOR "," _gc_emu)
    get_property(_gc_tests DIRECTORY PROPERTY TESTS)
    set_property(TEST ${_gc_tests} APPEND PROPERTY ENVIRONMENT "XBR_TEST_EMULATOR=${_gc_emu}")
endif()
