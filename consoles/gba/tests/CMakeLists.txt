cmake_minimum_required(VERSION 3.20)
project(console_gba_tests C)
# The Game Boy Advance runtime's C tests (XBA-146, XBA-152, XBA-153): a self-contained project like those under tests/,
# run by tools/run_c_tests.sh (as console_gba) and by the root build's ctest when XBR_CONSOLE_GBA is ON. POSIX hosts.
#
#   gba_runtime        the runtime links and answers (needs xbr_host / SDL2; not built with GBA_CORE_ONLY=ON)
#   gba_cpu            unit tests of the core (test_cpu.c)
#   gba_ppu            PPU feature tests (test_ppu.c); gba_ppu_bench, its micro benchmark, runs by hand
#   gba_hb_<prog>      our homebrew under gba_run --result, built-in BIOS (HLE)
#   gba_hb_lle_<prog>  the same through the Cult-of-GBA BIOS (LLE)
#   gba_jsmolka_<t>    jsmolka/gba-tests arm / thumb / memory / bios
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
add_executable(gba_ppu_bench bench_ppu.c)
target_link_libraries(gba_ppu_bench PRIVATE gba_core)
target_compile_options(gba_ppu_bench PRIVATE -Wall -Wextra)

get_filename_component(GBA_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(GBA_HOMEBREW_DIR ${GBA_REPO_ROOT}/build/homebrew/gba CACHE PATH "Built GBA homebrew (build_homebrew.sh gba)")
set(GBA_TESTS_DIR ${GBA_REPO_ROOT}/build/gba-tests CACHE PATH "jsmolka/gba-tests ROMs (fetch_gba_tests.sh)")
set(GBA_BIOS ${GBA_REPO_ROOT}/build/gba-bios/cult-of-gba-bios.bin CACHE FILEPATH "Cult-of-GBA BIOS (build_gba_bios.sh)")
set(GBA_RUN_ROM ${CMAKE_CURRENT_SOURCE_DIR}/run_rom.sh)

if(TARGET gba_run)
    foreach(prog hello cpu timer_irq iwram_copy gfx pad)
        add_test(NAME gba_hb_${prog}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba --result --frames 900)
        add_test(NAME gba_hb_lle_${prog}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_HOMEBREW_DIR}/${prog}.gba --result --frames 900
                    --bios ${GBA_BIOS})
        set_tests_properties(gba_hb_${prog} gba_hb_lle_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
    endforeach()
    # bios.gba checks the opcodes the Nintendo BIOS leaves in its read latch: it passes with our built-in BIOS
    # (laid out to match) and not with Cult-of-GBA's, so it only runs HLE.
    foreach(t arm thumb memory bios)
        add_test(NAME gba_jsmolka_${t}
            COMMAND sh ${GBA_RUN_ROM} $<TARGET_FILE:gba_run> ${GBA_TESTS_DIR}/${t}.gba --frames 60 --reg-result 12)
        set_tests_properties(gba_jsmolka_${t} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
    endforeach()
endif()
