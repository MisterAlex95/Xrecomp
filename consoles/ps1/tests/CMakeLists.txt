cmake_minimum_required(VERSION 3.20)
project(console_ps1_tests C)
# The PlayStation runtime's C tests (XBA-146): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_ps1) and by the root build's ctest when XBR_CONSOLE_PS1 is ON. POSIX hosts.
#
#   ps1_runtime        the runtime links and answers (needs xbr_host / SDL2; not built with PS1_CORE_ONLY=ON)
#   ps1_gpu            GPU feature tests (test_gpu.c); ps1_gpu_bench runs by hand
#   ps1_cpu            core unit tests (test_cpu.c): delay slots, load delays, exceptions, COP0, IRQs, DMA, timers,
#                      pads / memory cards
#   ps1_gte            GTE vectors checked by hand (test_gte.c)
#   ps1_hb_<prog>      our homebrew EXEs under ps1_run --result through OpenBIOS (LLE + fast boot); gfx needs the real
#                      GPU (ps1_gpu.c), overlay the disc and CD-ROM modules (ps1_disc.c, ps1_cdrom.c)
#   ps1_hb_disc_<prog> the same booted from their discs by the BIOS (SYSTEM.CNF), with the disc and CD-ROM modules
#   ps1_jcz_<test>     JaCzekanski/ps1-tests (cpu, gte, dma, timers, pad; spu with ps1_spu.c), console text checked
# The EXE / BIOS tests skip (exit 77) when their inputs are absent, so a CI job without Docker or network stays green.
# They come from (repo root):
#   tools/consoles/build_homebrew.sh ps1 build/homebrew   -> PS1_HOMEBREW_DIR (default build/homebrew/ps1)
#   tools/consoles/build_ps1_bios.sh build/ps1-bios       -> PS1_BIOS (default build/ps1-bios/openbios.bin)
#   tools/consoles/fetch_ps1_tests.sh build/ps1-tests     -> PS1_TESTS_DIR (default build/ps1-tests)
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only (xbr_host)")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET ps1_gpu)
    add_subdirectory(../runtime ps1_runtime)
endif()

enable_testing()
if(TARGET ps1_runtime)
    add_executable(ps1_runtime_test test_main.c)
    target_link_libraries(ps1_runtime_test PRIVATE ps1_runtime)
    add_test(NAME ps1_runtime COMMAND ps1_runtime_test)
    set_tests_properties(ps1_runtime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

# The GPU (P-7, XBA-165): feature tests on synthetic GP0 / GP1 streams; ps1_gpu_bench, its micro benchmark, runs by
# hand (ps1_gpu_bench [seconds]).
add_executable(ps1_gpu_test test_gpu.c)
target_link_libraries(ps1_gpu_test PRIVATE ps1_gpu)
target_compile_options(ps1_gpu_test PRIVATE -Wall -Wextra)
add_test(NAME ps1_gpu COMMAND ps1_gpu_test)
set_tests_properties(ps1_gpu PROPERTIES TIMEOUT 60)
add_executable(ps1_gpu_bench bench_gpu.c)
target_link_libraries(ps1_gpu_bench PRIVATE ps1_gpu)
target_compile_options(ps1_gpu_bench PRIVATE -Wall -Wextra -O2)

# The disc images, CD-ROM controller, SPU and MDEC (P-1 XBA-159, P-8 XBA-166, P-9 XBA-167): unit tests on synthetic
# images and register sequences the tests make themselves (dev_test.h; no game data, no Docker). The disc test also
# compares with a compressed CHD when chdman is on the PATH.
foreach(t disc cdrom spu mdec)
    add_executable(ps1_${t}_test test_${t}.c)
    target_link_libraries(ps1_${t}_test PRIVATE ps1_cd ps1_spu ps1_mdec m)
    target_include_directories(ps1_${t}_test PRIVATE ../runtime) # ps1_spu_gauss.inc (the reference decoder)
    target_compile_options(ps1_${t}_test PRIVATE -Wall -Wextra)
    if(XBR_PS1_CHD)
        target_compile_definitions(ps1_${t}_test PRIVATE XBR_PS1_CHD=1)
    endif()
    add_test(NAME ps1_${t} COMMAND ps1_${t}_test)
    set_tests_properties(ps1_${t} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endforeach()
# The core (P-4 / P-6 / P-9: XBA-162, XBA-164, XBA-167).
foreach(t cpu gte)
    add_executable(ps1_${t}_test test_${t}.c)
    target_link_libraries(ps1_${t}_test PRIVATE ps1_core)
    target_compile_options(ps1_${t}_test PRIVATE -Wall -Wextra)
    add_test(NAME ps1_${t} COMMAND ps1_${t}_test)
    set_tests_properties(ps1_${t} PROPERTIES TIMEOUT 60)
endforeach()

get_filename_component(PS1_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(PS1_HOMEBREW_DIR ${PS1_REPO_ROOT}/build/homebrew/ps1 CACHE PATH "Built PS1 homebrew (build_homebrew.sh ps1)")
set(PS1_BIOS ${PS1_REPO_ROOT}/build/ps1-bios/openbios.bin CACHE FILEPATH "OpenBIOS (build_ps1_bios.sh)")
set(PS1_TESTS_DIR ${PS1_REPO_ROOT}/build/ps1-tests CACHE PATH "JaCzekanski/ps1-tests (fetch_ps1_tests.sh)")
set(PS1_RT ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)
set(PS1_HAVE_GPU OFF)
set(PS1_HAVE_CD OFF)
if(EXISTS ${PS1_RT}/ps1_gpu.c)
    set(PS1_HAVE_GPU ON)
endif()
if(EXISTS ${PS1_RT}/ps1_disc.c AND EXISTS ${PS1_RT}/ps1_cdrom.c)
    set(PS1_HAVE_CD ON)
endif()

if(TARGET ps1_run)
    set(PS1_RUN_EXE sh ${CMAKE_CURRENT_SOURCE_DIR}/run_exe.sh $<TARGET_FILE:ps1_run> ${PS1_BIOS})
    set(hb cpu hello timer_irq)
    if(PS1_HAVE_GPU)
        list(APPEND hb gfx)
    endif()
    foreach(prog ${hb})
        add_test(NAME ps1_hb_${prog} COMMAND ${PS1_RUN_EXE} ${PS1_HOMEBREW_DIR}/${prog}.exe --result --frames 900 --quiet)
        set_tests_properties(ps1_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endforeach()
    if(PS1_HAVE_CD)
        add_test(NAME ps1_hb_overlay COMMAND ${PS1_RUN_EXE} ${PS1_HOMEBREW_DIR}/overlay.exe
                 --disc ${PS1_HOMEBREW_DIR}/overlay.cue --result --frames 1800 --quiet)
        set_tests_properties(ps1_hb_overlay PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
        foreach(prog ${hb} overlay)
            add_test(NAME ps1_hb_disc_${prog} COMMAND ${PS1_RUN_EXE} ${PS1_HOMEBREW_DIR}/${prog}.cue --result --frames 3000
                     --quiet)
            # OpenBIOS boots the disc through SYSTEM.CNF (Setloc, SeekL, ReadN per file)
            set_tests_properties(ps1_hb_disc_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
        endforeach()
    endif()

    # JaCzekanski/ps1-tests: each runs to "Done." with no failing check; EXPECT strings pin the key verdicts.
    set(PS1_JCZ sh ${CMAKE_CURRENT_SOURCE_DIR}/run_jcz.sh $<TARGET_FILE:ps1_run> ${PS1_BIOS})
    add_test(NAME ps1_jcz_cpu_cop COMMAND ${PS1_JCZ} ${PS1_TESTS_DIR}/cpu/cop/cop.exe "testDisabledCoprocessorThrows")
    add_test(NAME ps1_jcz_cpu_code_in_io COMMAND ${PS1_JCZ} ${PS1_TESTS_DIR}/cpu/code-in-io/code-in-io.exe
             "pass - testCodeInDMAControl")
    add_test(NAME ps1_jcz_gte_test_all COMMAND ${PS1_JCZ} ${PS1_TESTS_DIR}/gte/test-all/test-all.exe
             "Passed tests: 1150" "Failed tests: 0")
    add_test(NAME ps1_jcz_dma_otc COMMAND ${PS1_JCZ} ${PS1_TESTS_DIR}/dma/otc-test/otc-test.exe
             "pass - testOtcStandardWithMasterDisabled")
    add_test(NAME ps1_jcz_dma_dpcr COMMAND ${PS1_JCZ} ${PS1_TESTS_DIR}/dma/dpcr/dpcr.exe "pass - testSPUDMARead")
    add_test(NAME ps1_jcz_timers COMMAND ${PS1_JCZ} ${PS1_TESTS_DIR}/timers/timers.exe
             "Reached target:                      1 (ok)" "Counter reset AFTER reaching target: 1 (ok)")
    set(jcz ps1_jcz_cpu_cop ps1_jcz_cpu_code_in_io ps1_jcz_gte_test_all ps1_jcz_dma_otc ps1_jcz_dma_dpcr ps1_jcz_timers)
    if(EXISTS ${PS1_RT}/ps1_spu.c)
        add_test(NAME ps1_jcz_spu_memory_transfer COMMAND ${PS1_JCZ}
                 ${PS1_TESTS_DIR}/spu/memory-transfer/memory-transfer.exe "pass - testDMAReadTiming"
                 "pass - testDMAReadToRamSyncMode0")
        list(APPEND jcz ps1_jcz_spu_memory_transfer)
    endif()
    set_tests_properties(${jcz} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
