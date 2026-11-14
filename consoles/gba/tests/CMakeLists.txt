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
#   gba_rc_*           the same ROMs recompiled (gba_tools.recomp + cc, discovery loop), in lockstep with the
#                      interpreter; gba_rc_fuzz: random ARM / Thumb programs, same comparison (needs python3, cc)
# The ROM and BIOS tests skip (exit 77) when their inputs are absent, so a CI job without Docker or network stays
# green. They come from (repo root):
#   tools/consoles/build_homebrew.sh gba build/homebrew     -> GBA_HOMEBREW_DIR (default build/homebrew/gba)
#   tools/consoles/fetch_gba_tests.sh build/gba-tests       -> GBA_TESTS_DIR (default build/gba-tests)
#   tools/consoles/build_gba_bios.sh build/gba-bios         -> GBA_BIOS (default build/gba-bios/cult-of-gba-bios.bin)
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
set(GBA_RUN_ROM ${CMAKE_CURRENT_SOURCE_DIR}/run_rom.sh)

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
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gba_rc_fuzz
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recomp_fuzz.py --gba-run $<TARGET_FILE:gba_run>
                    --work ${GBA_RC_WORK}/fuzz --seeds 0-5 --frames 20)
        set_tests_properties(gba_rc_fuzz PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
    endif()
endif()
