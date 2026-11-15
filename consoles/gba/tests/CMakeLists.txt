cmake_minimum_required(VERSION 3.20)
project(console_gba_tests C)

# macOS: _POSIX_C_SOURCE hides the BSD / C99 extras (snprintf, mkdtemp...) unless _DARWIN_C_SOURCE is set.
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()
# The Game Boy Advance runtime's C tests (XBA-146, XBA-152, XBA-153): a self-contained project like those under tests/,
# run by tools/run_c_tests.sh (as console_gba) and by the root build's ctest when XBR_CONSOLE_GBA is ON. POSIX hosts.
#
#   gba_runtime        the runtime links and answers (needs xbr_host / SDL2; not built with GBA_CORE_ONLY=ON)
#   gba_cpu            unit tests of the core (test_cpu.c)
#   gba_ppu            PPU feature tests (test_ppu.c); gba_ppu_bench, its micro benchmark, runs by hand
#   gba_apu            APU unit tests (test_apu.c, XBA-155)
#   gba_save           save memory unit tests (test_save.c, XBA-156)
#   gba_pack           a title pack, the runtime's side (test_pack.c + the fake pack fake_pack.c, XBA-157 / XBA-158)
#   gba_hb_<prog>      our homebrew under gba_run --result, built-in BIOS (HLE)
#   gba_hb_lle_<prog>  the same through the Cult-of-GBA BIOS (LLE)
#   gba_jsmolka_<t>    jsmolka/gba-tests arm / thumb / memory / bios, and save/sram, flash64, flash128, none
#   gba_rewind_*       rewind round trips on the homebrew (gba_run --rewind, XBA-194)
#   gba_cheats_*       cheat codes on the homebrew (gba_run --cheats, XBA-197), interpreted and recompiled
#   gba_mgba_<group>   the mGBA test suite's groups (mgba_suite.sh, XBA-201): pass counts at least the ones reached
#                      (a regression fails), built-in BIOS; gba_mgba_lle_<group> through the Cult-of-GBA BIOS
#   gba_rc_*           the same ROMs recompiled (gba_tools.recomp + cc, discovery loop), in lockstep with the
#                      interpreter; gba_rc_fuzz: random ARM / Thumb programs, same comparison (needs python3, cc)
#   gba_lockref_*      the interpreter's predecoded blocks against its reference loop (gba_run --lockstep-ref,
#                      XBA-208)
# The ROM and BIOS tests skip (exit 77) when their inputs are absent, so a CI job without Docker or network stays
# green. They come from (repo root):
#   tools/consoles/build_homebrew.sh gba build/homebrew     -> GBA_HOMEBREW_DIR (default build/homebrew/gba)
#   tools/consoles/fetch_gba_tests.sh build/gba-tests       -> GBA_TESTS_DIR (default build/gba-tests)
#   tools/consoles/build_gba_bios.sh build/gba-bios         -> GBA_BIOS (default build/gba-bios/cult-of-gba-bios.bin)
#   tools/consoles/build_gba_suite.sh build/gba-suite       -> GBA_SUITE (default build/gba-suite/suite.gba)
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only (xbr_host)")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET gba_core)
    add_subdirectory(../runtime gba_runtime)
endif()
enable_testing()

if(TARGET gba_runtime)
    add_executable(gba_runtime_test test_main.c)
    target_link_libraries(gba_runtime_test PRIVATE gba_runtime)
    add_test(NAME gba_runtime COMMAND gba_runtime_test)
    set_tests_properties(gba_runtime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

add_executable(gba_cpu_test test_cpu.c)
target_link_libraries(gba_cpu_test PRIVATE gba_core)
target_compile_options(gba_cpu_test PRIVATE -Wall -Wextra)
add_test(NAME gba_cpu COMMAND gba_cpu_test)
set_tests_properties(gba_cpu PROPERTIES TIMEOUT 60)

# The PPU (XBA-154): feature tests on synthetic scenes and a micro benchmark (run by hand: gba_ppu_bench [seconds]).
add_executable(gba_ppu_test test_ppu.c)
target_link_libraries(gba_ppu_test PRIVATE gba_core)
target_compile_options(gba_ppu_test PRIVATE -Wall -Wextra)
add_test(NAME gba_ppu COMMAND gba_ppu_test)
set_tests_properties(gba_ppu PROPERTIES TIMEOUT 60)
foreach(t apu save)
    add_executable(gba_${t}_test test_${t}.c)
    target_link_libraries(gba_${t}_test PRIVATE gba_core)
    target_compile_options(gba_${t}_test PRIVATE -Wall -Wextra)
    add_test(NAME gba_${t} COMMAND gba_${t}_test)
    set_tests_properties(gba_${t} PROPERTIES TIMEOUT 60)
endforeach()
# A GBA title pack (XBA-157 / XBA-158): fake_gba_pack is built like tools/prepare's libtitle.so (its core calls
# resolved against the program that loads it), gba_pack_test exports the core and checks gba_pack.h.
add_library(fake_gba_pack SHARED fake_pack.c)
target_include_directories(fake_gba_pack PRIVATE ../runtime)
if(APPLE)
    target_link_options(fake_gba_pack PRIVATE -undefined dynamic_lookup)
endif()
add_executable(gba_pack_test test_pack.c)
target_link_libraries(gba_pack_test PRIVATE gba_pack)
gba_core_exported(gba_pack_test)
target_compile_options(gba_pack_test PRIVATE -Wall -Wextra)
add_test(NAME gba_pack COMMAND gba_pack_test $<TARGET_FILE:fake_gba_pack>)
set_tests_properties(gba_pack PROPERTIES TIMEOUT 60)
add_executable(gba_ppu_bench bench_ppu.c)
target_link_libraries(gba_ppu_bench PRIVATE gba_core)
target_compile_options(gba_ppu_bench PRIVATE -Wall -Wextra)

get_filename_component(GBA_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(GBA_HOMEBREW_DIR ${GBA_REPO_ROOT}/build/homebrew/gba CACHE PATH "Built GBA homebrew (build_homebrew.sh gba)")
set(GBA_TESTS_DIR ${GBA_REPO_ROOT}/build/gba-tests CACHE PATH "jsmolka/gba-tests ROMs (fetch_gba_tests.sh)")
set(GBA_BIOS ${GBA_REPO_ROOT}/build/gba-bios/cult-of-gba-bios.bin CACHE FILEPATH "Cult-of-GBA BIOS (build_gba_bios.sh)")
set(GBA_SUITE ${GBA_REPO_ROOT}/build/gba-suite/suite.gba CACHE FILEPATH "mGBA test suite ROM (build_gba_suite.sh)")
set(GBA_RUN_ROM ${CMAKE_CURRENT_SOURCE_DIR}/run_rom.sh)
set(GBA_EXPECT sh ${GBA_REPO_ROOT}/tools/consoles/expect_output.sh)   # XBA-197: output checks
set(GBA_CHEATS ${CMAKE_CURRENT_SOURCE_DIR}/cheats)

if(TARGET gba_run)
    foreach(prog hello cpu timer_irq iwram_copy gfx pad sound)
        add_test(NAME gba_hb_${prog}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba --result --frames 900)
        add_test(NAME gba_hb_lle_${prog}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba --result --frames 900
                    --bios ${GBA_BIOS})
        set_tests_properties(gba_hb_${prog} gba_hb_lle_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
    endforeach()
    # bios.gba checks the opcodes the Nintendo BIOS leaves in its read latch: it passes with our built-in BIOS
    # (laid out to match) and not with Cult-of-GBA's, so it only runs HLE.
    foreach(t arm thumb memory bios sram flash64 flash128 none)
        add_test(NAME gba_jsmolka_${t}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_TESTS_DIR}/${t}.gba --frames 300 --reg-result 12)
        set_tests_properties(gba_jsmolka_${t} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
    endforeach()

    # Static recompilation (XBA-150 / 151 / 152): each ROM above lifted to C (gba_tools.recomp), built with the host
    # cc, the discovery loop run to convergence, then played in lockstep with the interpreter (recomp_rom.sh).
    # gba_rc_fuzz: random ARM / Thumb programs with timer IRQs, lifted and compared the same way (recomp_fuzz.py).
    set(GBA_RC_ROM ${CMAKE_CURRENT_SOURCE_DIR}/recomp_rom.sh)
    set(GBA_RC_WORK ${CMAKE_CURRENT_BINARY_DIR}/recomp)
    add_test(NAME gba_rc_cheats_hb_hello
        COMMAND ${GBA_EXPECT} "LOCKSTEP OK|peek 0x0203FFB0 = 0xC0DE0001|peek 0x0203FFC0 = 0x00111234"
                sh ${GBA_RC_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/hello.gba ${GBA_RC_WORK}/cheats_hello
                --result --frames 900 --cheats ${GBA_CHEATS}/ram.cheats --peek 0x0203FFB0 --peek 0x0203FFC0)
    add_test(NAME gba_rc_cheats_hb_hello_rompatch
        COMMAND ${GBA_EXPECT} "LOCKSTEP OK|recompiled code set aside|peek 0x08000004 = 0x51AEABCD"
                sh ${GBA_RC_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/hello.gba
                ${GBA_RC_WORK}/cheats_hello_rompatch --result --frames 900 --cheats ${GBA_CHEATS}/hello.cheats
                --peek 0x08000004)
    set_tests_properties(gba_rc_cheats_hb_hello gba_rc_cheats_hb_hello_rompatch PROPERTIES SKIP_RETURN_CODE 77
                         TIMEOUT 600)
    foreach(prog hello cpu timer_irq iwram_copy gfx pad sound)
        add_test(NAME gba_rc_hb_${prog}
            COMMAND sh ${GBA_RC_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba ${GBA_RC_WORK}/hb_${prog}
                    --result --frames 900)
        add_test(NAME gba_rc_hb_lle_${prog}
            COMMAND sh ${GBA_RC_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba
                    ${GBA_RC_WORK}/hb_lle_${prog} --result --frames 900 --bios ${GBA_BIOS})
        set_tests_properties(gba_rc_hb_${prog} gba_rc_hb_lle_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    endforeach()
    foreach(t arm thumb memory bios sram flash64 flash128 none)
        add_test(NAME gba_rc_jsmolka_${t}
            COMMAND sh ${GBA_RC_ROM} $<TARGET_FILE:gba_run> ${GBA_TESTS_DIR}/${t}.gba ${GBA_RC_WORK}/jsmolka_${t}
                    --frames 300 --reg-result 12)
        set_tests_properties(gba_rc_jsmolka_${t} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    endforeach()
    # Save states (XBA-182): the round trip on every homebrew (a state saved after frame 120, the next 180 frames
    # hashed, then the same from the state loaded in a new machine), interpreter alone (gba_state_hb_*) and with the
    # ROM's pack (gba_rc_state_hb_*: states cross between lifted and interpreted runs), and another game's state
    # refused (gba_state_wrong_game). state_rom.sh.
    set(GBA_STATE_ROM ${CMAKE_CURRENT_SOURCE_DIR}/state_rom.sh)
    foreach(prog hello cpu timer_irq iwram_copy gfx pad sound)
        add_test(NAME gba_state_hb_${prog}
            COMMAND sh ${GBA_STATE_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba
                    ${CMAKE_CURRENT_BINARY_DIR}/state/hb_${prog} interp)
        add_test(NAME gba_rc_state_hb_${prog}
            COMMAND sh ${GBA_STATE_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba
                    ${CMAKE_CURRENT_BINARY_DIR}/state/rc_hb_${prog} pack)
        set_tests_properties(gba_state_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
        set_tests_properties(gba_rc_state_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
    endforeach()
    add_test(NAME gba_state_hb_lle_sound
        COMMAND sh ${GBA_STATE_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/sound.gba
                ${CMAKE_CURRENT_BINARY_DIR}/state/hb_lle_sound interp --bios ${GBA_BIOS})
    add_test(NAME gba_state_wrong_game
        COMMAND sh ${GBA_STATE_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/gfx.gba
                ${CMAKE_CURRENT_BINARY_DIR}/state/wrong_game other:${GBA_HOMEBREW_DIR}/sound.gba)
    set_tests_properties(gba_state_hb_lle_sound gba_state_wrong_game PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    # Rewind (XBA-194, gba_run --rewind): 600 frames with pseudo-random keys, a capture every 2 frames as the runtime
    # takes them, 90 steps back: the picture there and the frames after it must be the first run's
    # (gba_rewind_hb_*); gba_rewind_cap: a 2 MB cap drops the oldest captures, the rest still come back exact.
    foreach(prog hello cpu timer_irq iwram_copy gfx pad sound)
        add_test(NAME gba_rewind_hb_${prog}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba --frames 600 --rewind 2:90)
        set_tests_properties(gba_rewind_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
    endforeach()
    add_test(NAME gba_rewind_cap
        COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/sound.gba --frames 2400 --rewind 1:2000
                --rewind-mb 2)
    set_tests_properties(gba_rewind_cap PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
    # Cheats (XBA-197, gba_run --cheats cheats/hello.cheats): a raw write, GameShark v1/v2 and Action Replay v3
    # encrypted codes (a conditional on the result block's status, a fill), CodeBreaker writes and an add, a cartridge
    # patch, and a cheat that is off; the values read back after the verdict (--peek). gba_rc_cheats_*: the same on
    # the recompiled pack in lockstep with the interpreter: RAM codes only (the pack stays), and with the cartridge
    # patch (the pack's lifted code is set aside while it is in place).
    set(GBA_CHEAT_PEEKS --peek 0x0203FFB0 --peek 0x0203FFB4 --peek 0x0203FFB8 --peek 0x0203FFBC --peek 0x0203FFC0
        --peek 0x08000004)
    string(CONCAT GBA_CHEAT_WANT "HELO PASS|peek 0x0203FFB0 = 0xC0DE0001|peek 0x0203FFB4 = 0x00000000"
           "|peek 0x0203FFB8 = 0x0000BEEF|peek 0x0203FFBC = 0x77777777|peek 0x0203FFC0 = 0x00111234")
    add_test(NAME gba_cheats_hb_hello
        COMMAND ${GBA_EXPECT} "${GBA_CHEAT_WANT}|peek 0x08000004 = 0x51AEABCD"
                sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/hello.gba --result --frames 900
                --cheats ${GBA_CHEATS}/hello.cheats ${GBA_CHEAT_PEEKS})
    set_tests_properties(gba_cheats_hb_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
    # The mGBA test suite (XBA-201): each group's pass count must stay at least what the core reached (raise these
    # when a fix passes more; consoles/gba/tests/mgba_suite.sh). The built-in BIOS (HLE) and Cult-of-GBA (LLE) differ
    # where a test times or checks the BIOS's own code; gba_rc_mgba: the suite recompiled, in lockstep, HLE counts.
    set(GBA_MGBA_HLE memory=1498 io_read=130 timing=2020 timers=936 timer_irq=90 shifter=140 carry=93
        multiply_long=72 bios_math=615 dma=1244 sio_read=90 sio_timing=4 misc=6 video=6)
    set(GBA_MGBA_LLE memory=1488 io_read=130 timing=1900 timers=847 timer_irq=90 shifter=140 carry=93
        multiply_long=72 bios_math=603 dma=1244 sio_read=90 sio_timing=4 misc=6 video=6)
    set(GBA_MGBA ${CMAKE_CURRENT_SOURCE_DIR}/mgba_suite.sh)
    foreach(gm ${GBA_MGBA_HLE})
        string(REGEX REPLACE "=.*" "" g ${gm})
        add_test(NAME gba_mgba_${g} COMMAND sh ${GBA_MGBA} $<TARGET_FILE:gba_run> ${GBA_SUITE} ${gm})
        set_tests_properties(gba_mgba_${g} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    endforeach()
    foreach(gm ${GBA_MGBA_LLE})
        string(REGEX REPLACE "=.*" "" g ${gm})
        add_test(NAME gba_mgba_lle_${g}
            COMMAND sh ${GBA_MGBA} $<TARGET_FILE:gba_run> ${GBA_SUITE} ${gm} -- --bios ${GBA_BIOS})
        set_tests_properties(gba_mgba_lle_${g} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    endforeach()
    add_test(NAME gba_rc_mgba
        COMMAND sh ${GBA_MGBA} $<TARGET_FILE:gba_run> ${GBA_SUITE} --recomp ${GBA_RC_WORK}/mgba ${GBA_MGBA_HLE})
    set_tests_properties(gba_rc_mgba PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gba_rc_fuzz
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recomp_fuzz.py --gba-run $<TARGET_FILE:gba_run>
                    --work ${GBA_RC_WORK}/fuzz --seeds 0-5 --frames 20)
        set_tests_properties(gba_rc_fuzz PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
    endif()

    # The interpreter's predecoded blocks (XBA-208) against its reference loop, one full step per instruction
    # (gba_run --lockstep-ref): the whole save state and the frame compared after every frame, and with
    # --lockstep-cycles the CPU every 61 cycles (the whole state every 1024 stops), which also cuts blocks short at
    # every point; the built-in and the Cult-of-GBA BIOS, jsmolka's ROMs, mGBA suite groups (their counts as above),
    # random ARM / Thumb programs under timer IRQs from the cartridge and copied to IWRAM each pass with an immediate
    # patched (recomp_fuzz.py --ref [--iwram]).
    set(lockref)
    foreach(prog hello cpu timer_irq iwram_copy gfx pad sound)
        add_test(NAME gba_lockref_hb_${prog}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba --frames 900
                    --lockstep-ref)
        add_test(NAME gba_lockref_lle_hb_${prog}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba --frames 900 --lockstep-ref
                    --bios ${GBA_BIOS})
        add_test(NAME gba_lockref_cyc_hb_${prog}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba --frames 60
                    --lockstep-cycles 61)
        list(APPEND lockref gba_lockref_hb_${prog} gba_lockref_lle_hb_${prog} gba_lockref_cyc_hb_${prog})
    endforeach()
    foreach(t arm thumb memory bios sram flash64 flash128 none)
        add_test(NAME gba_lockref_jsmolka_${t}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_TESTS_DIR}/${t}.gba --frames 300 --lockstep-ref)
        list(APPEND lockref gba_lockref_jsmolka_${t})
    endforeach()
    foreach(gm ${GBA_MGBA_HLE})
        string(REGEX REPLACE "=.*" "" g ${gm})
        add_test(NAME gba_lockref_mgba_${g} COMMAND sh ${GBA_MGBA} $<TARGET_FILE:gba_run> ${GBA_SUITE} ${gm} --
                 --lockstep-ref)
        list(APPEND lockref gba_lockref_mgba_${g})
    endforeach()
    if(Python3_Interpreter_FOUND)
        add_test(NAME gba_lockref_fuzz
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recomp_fuzz.py --gba-run $<TARGET_FILE:gba_run>
                    --work ${CMAKE_CURRENT_BINARY_DIR}/lockref_fuzz --seeds 0-7 --frames 30 --ref --lockstep-cycles 61)
        add_test(NAME gba_lockref_fuzz_iwram
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recomp_fuzz.py --gba-run $<TARGET_FILE:gba_run>
                    --work ${CMAKE_CURRENT_BINARY_DIR}/lockref_fuzz_iwram --seeds 0-7 --frames 30 --insns 30 --ref
                    --iwram --lockstep-cycles 61)
        list(APPEND lockref gba_lockref_fuzz gba_lockref_fuzz_iwram)
    endif()
    set_tests_properties(${lockref} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()
