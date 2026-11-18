cmake_minimum_required(VERSION 3.20)
project(console_ps1_tests C)

# macOS: _POSIX_C_SOURCE hides the BSD / C99 extras (snprintf, mkdtemp...) unless _DARWIN_C_SOURCE is set.
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()
# The PlayStation runtime's C tests (XBA-146): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_ps1) and by the root build's ctest when XBR_CONSOLE_PS1 is ON. POSIX hosts.
#
#   ps1_runtime        the runtime: helpers, headless runs of gfx.exe / gfx.cue (needs xbr_host / SDL2; not built
#                      with PS1_CORE_ONLY=ON)
#   ps1_gpu            GPU feature tests (test_gpu.c); ps1_gpu_bench runs by hand
#   ps1_cpu            core unit tests (test_cpu.c): delay slots, load delays, exceptions, COP0, IRQs, DMA, timers,
#                      pads / memory cards
#   ps1_gte            GTE vectors checked by hand (test_gte.c)
#   ps1_hb_<prog>      our homebrew EXEs under ps1_run --result through OpenBIOS (LLE + fast boot); gfx needs the real
#                      GPU (ps1_gpu.c), overlay the disc and CD-ROM modules (ps1_disc.c, ps1_cdrom.c)
#   ps1_hb_disc_<prog> the same booted from their discs by the BIOS (SYSTEM.CNF), with the disc and CD-ROM modules
#   ps1_mcard          the mcard homebrew's save through the BIOS memory card functions, kept in a card file by
#                      ps1_run --card: created on a blank card, found again, an unformatted card formatted; the card
#                      image checked (check_mcard.py: checksums, directory entry, data)
#   ps1_jcz_<test>     JaCzekanski/ps1-tests (cpu, gte, dma, timers, pad; spu with ps1_spu.c), console text checked
#   ps1_hle            the HLE BIOS (test_hle.c): a check for every implemented A / B / C call, the exception path,
#                      the kernel layout, a synthetic disc booted through SYSTEM.CNF, the memory card file system
#   ps1_hle_*          the programs above under the HLE BIOS (no image): PASS, the same result block as under
#                      OpenBIOS (ps1_hle_same_*), disc boots, mcard, ps1-tests, recompiled in lockstep (ps1_hle_rc_*)
#   ps1_state_*        save states: the round trip, interpreter and packs (ps1_rc_state_*), refusals (XBA-182)
#   ps1_rewind_*       rewind round trips (ps1_run --rewind, XBA-194): every homebrew, a disc read, the memory cap
#   ps1_cheats_*       GameShark codes on the homebrew (ps1_run --cheats, XBA-197), interpreted and recompiled
#   ps1_rc_*           the same programs recompiled (ps1_tools.recomp + cc, discovery loop), in lockstep with the
#                      interpreter; ps1_rc_fuzz: random MIPS programs under timer IRQs (needs python3, cc)
#   ps1_lockref_*      the interpreter's predecoded blocks against its reference loop (ps1_run --lockstep-ref, XBA-204)
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
# The title key of a .chd disc (XBA-159): recomp_pack_rom_identify (src/platform, compiled in) with the PS1 disc reader as
# its disc hook (ps1_identify), on CHDs the test writes (uncompressed, and cdzl with a huffman-coded map) and on real
# chdman output (chdman/: multi-track with CD-DA, pregaps, cdzs / cdfl; fresh ones too when chdman is installed).
get_filename_component(PS1_PLATFORM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/platform ABSOLUTE)
add_executable(ps1_identify_test test_identify.c ${PS1_PLATFORM_SRC}/recomp_pack.c ${PS1_PLATFORM_SRC}/recomp_cfg.c)
target_include_directories(ps1_identify_test PRIVATE ${PS1_PLATFORM_SRC})
target_link_libraries(ps1_identify_test PRIVATE ps1_identify ${CMAKE_DL_LIBS})
target_compile_options(ps1_identify_test PRIVATE -Wall -Wextra)
if(XBR_PS1_CHD)
    target_compile_definitions(ps1_identify_test PRIVATE XBR_PS1_CHD=1)
endif()
add_test(NAME ps1_identify COMMAND ps1_identify_test ${CMAKE_CURRENT_SOURCE_DIR}/chdman)
set_tests_properties(ps1_identify PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
# The core (P-4 / P-6 / P-9: XBA-162, XBA-164, XBA-167); test_pad.c: the DualShock config commands (XBA-167).
foreach(t cpu gte pad hle)
    add_executable(ps1_${t}_test test_${t}.c)
    target_link_libraries(ps1_${t}_test PRIVATE ps1_core)
    target_compile_options(ps1_${t}_test PRIVATE -Wall -Wextra)
    add_test(NAME ps1_${t} COMMAND ps1_${t}_test)
    set_tests_properties(ps1_${t} PROPERTIES TIMEOUT 60)
endforeach()
# Save state refusals (XBA-182): fields out of range in a state are refused before anything loads (test_state.c).
# test_state_fields.c gives the private fields' offsets, built once per device with its source included (that copy
# links in place of the library's).
add_executable(ps1_state_test test_state.c)
foreach(dev GPU CDROM SPU MDEC HLE)
    add_library(ps1_state_fields_${dev} OBJECT test_state_fields.c)
    target_compile_definitions(ps1_state_fields_${dev} PRIVATE STATE_${dev}=1)
    target_include_directories(ps1_state_fields_${dev} PRIVATE ../runtime)
    target_link_libraries(ps1_state_fields_${dev} PRIVATE ps1_core) # its include directories
    target_compile_options(ps1_state_fields_${dev} PRIVATE -Wall -Wextra)
    target_sources(ps1_state_test PRIVATE $<TARGET_OBJECTS:ps1_state_fields_${dev}>)
endforeach()
target_link_libraries(ps1_state_test PRIVATE ps1_core)
target_compile_options(ps1_state_test PRIVATE -Wall -Wextra)
add_test(NAME ps1_state COMMAND ps1_state_test)
set_tests_properties(ps1_state PROPERTIES TIMEOUT 60)

get_filename_component(PS1_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(PS1_HOMEBREW_DIR ${PS1_REPO_ROOT}/build/homebrew/ps1 CACHE PATH "Built PS1 homebrew (build_homebrew.sh ps1)")

# A PS1 title pack (XBA-168): fake_ps1_pack is built like tools/prepare's libtitle.so (its core calls resolved against
# the program that loads it), ps1_pack_test exports the core and checks ps1_pack.h (the homebrew disc's boot EXE
# too when it is built).
add_library(fake_ps1_pack SHARED fake_pack.c)
target_include_directories(fake_ps1_pack PRIVATE ../runtime)
if(APPLE)
    target_link_options(fake_ps1_pack PRIVATE -undefined dynamic_lookup)
endif()
add_executable(ps1_pack_test test_pack.c)
ps1_core_exported(ps1_pack_test)
target_link_libraries(ps1_pack_test PRIVATE ps1_pack)
target_compile_options(ps1_pack_test PRIVATE -Wall -Wextra)
add_test(NAME ps1_pack COMMAND ps1_pack_test $<TARGET_FILE:fake_ps1_pack> ${PS1_HOMEBREW_DIR})
set_tests_properties(ps1_pack PROPERTIES TIMEOUT 60)
set(PS1_BIOS ${PS1_REPO_ROOT}/build/ps1-bios/openbios.bin CACHE FILEPATH "OpenBIOS (build_ps1_bios.sh)")
set(PS1_TESTS_DIR ${PS1_REPO_ROOT}/build/ps1-tests CACHE PATH "JaCzekanski/ps1-tests (fetch_ps1_tests.sh)")
set(PS1_RT ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)
# The runtime (XBA-168): helpers always, headless runs of gfx.exe / gfx.cue when the BIOS and the homebrew are built.
if(TARGET ps1_runtime)
    add_executable(ps1_runtime_test test_main.c)
    target_link_libraries(ps1_runtime_test PRIVATE ps1_runtime)
    target_compile_options(ps1_runtime_test PRIVATE -Wall -Wextra)
    add_test(NAME ps1_runtime COMMAND ps1_runtime_test ${PS1_BIOS} ${PS1_HOMEBREW_DIR})
    set_tests_properties(ps1_runtime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # The screenshot of a run in a real window (HOST-001); skipped (77) without a display.
    add_executable(ps1_present_test test_present.c)
    target_link_libraries(ps1_present_test PRIVATE ps1_runtime)
    target_compile_options(ps1_present_test PRIVATE -Wall -Wextra)
    add_test(NAME ps1_present COMMAND ps1_present_test)
    set_tests_properties(ps1_present PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
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
    set(hb cpu hello timer_irq mcard)
    if(PS1_HAVE_GPU)
        list(APPEND hb gfx)
    endif()
    foreach(prog ${hb})
        add_test(NAME ps1_hb_${prog} COMMAND ${PS1_RUN_EXE} ${PS1_HOMEBREW_DIR}/${prog}.exe --result --frames 900 --quiet)
        set_tests_properties(ps1_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endforeach()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ps1_mcard COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/mcard_test.sh $<TARGET_FILE:ps1_run> ${PS1_BIOS}
                 ${PS1_HOMEBREW_DIR}/mcard.exe ${CMAKE_CURRENT_BINARY_DIR}/mcard)
        set_tests_properties(ps1_mcard PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    endif()
    if(PS1_HAVE_CD)
        add_test(NAME ps1_hb_overlay COMMAND ${PS1_RUN_EXE} ${PS1_HOMEBREW_DIR}/overlay.exe
                 --disc ${PS1_HOMEBREW_DIR}/overlay.cue --result --frames 1800 --quiet)
        set_tests_properties(ps1_hb_overlay PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
        foreach(prog ${hb} overlay)
            add_test(NAME ps1_hb_disc_${prog} COMMAND ${PS1_RUN_EXE} ${PS1_HOMEBREW_DIR}/${prog}.cue --result --frames 3000
                     --quiet)
            # OpenBIOS boots the disc through SYSTEM.CNF (Setloc, SeekL, ReadN per file)
            set_tests_properties(ps1_hb_disc_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
            # the fast boot of a disc (XBA-168): the BIOS's shell skipped, the BIOS boots the disc itself
            add_test(NAME ps1_hb_fastboot_${prog} COMMAND ${PS1_RUN_EXE} ${PS1_HOMEBREW_DIR}/${prog}.cue --fast-boot
                     --result --frames 900 --quiet)
            set_tests_properties(ps1_hb_fastboot_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
        endforeach()
        # The overlay disc as CHDs ps1_tools.chd writes (cdlz, cdzl; XBA-168): booted through libchdr by the BIOS
        if(XBR_PS1_CHD)
            foreach(codec cdlz cdzl)
                add_test(NAME ps1_hb_chd_overlay_${codec} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_chd.sh
                         $<TARGET_FILE:ps1_run> ${PS1_BIOS} ${PS1_HOMEBREW_DIR}/overlay.bin ${codec}
                         ${CMAKE_CURRENT_BINARY_DIR}/chd/overlay_${codec}.chd --result --frames 3000 --quiet)
                set_tests_properties(ps1_hb_chd_overlay_${codec} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
            endforeach()
        endif()
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
    if(PS1_HAVE_CD)
        # cdrom/getloc, timing and disc-swap with a 74:10 synthetic disc (a sparse image the script makes, XBA-166)
        foreach(t getloc timing disc-swap)
            string(REPLACE "-" "_" n ${t})
            add_test(NAME ps1_jcz_cdrom_${n} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_jcz_cd.sh
                     $<TARGET_FILE:ps1_run> ${PS1_BIOS} ${PS1_TESTS_DIR}/cdrom/${t}/${t}.exe
                     ${CMAKE_CURRENT_BINARY_DIR}/jcz_cdrom_${n} ${t})
            list(APPEND jcz ps1_jcz_cdrom_${n})
        endforeach()
    endif()
    set_tests_properties(${jcz} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

    # The whole ps1-tests release run (XBA-200): GPU programs checked against the VRAM dumps made on the hardware that
    # the release ships (run_jcz_vram.sh + compare_vram.py, 5 bits per channel), GPU / DMA programs by their console
    # text, under OpenBIOS (ps1_jcz_*) and the HLE BIOS (ps1_hle_jcz_*). lines: the last segment of the two gouraud
    # polylines ends on a colour the program never sets (stack garbage), left out; transparency: the dump's VRAM
    # outside the 320x240 the program draws is white (not cleared on that console), only the picture is compared.
    set(jcz_more)
    if(PS1_HAVE_GPU AND Python3_Interpreter_FOUND)
        set(PS1_JCZ_VRAM_lines --ignore 150,141,182,173 --ignore 150,140,151,141 --ignore 210,141,242,173
            --ignore 210,140,211,141)
        set(PS1_JCZ_VRAM_transparency --region 0,0,320,240)
        foreach(t clipping clut-cache lines quad rectangles texture-flip texture-overflow transparency triangle
                  uv-interpolation vram-to-vram-overlap)
            string(REPLACE "-" "_" n ${t})
            foreach(b lle hle)
                if(b STREQUAL lle)
                    set(bios ${PS1_BIOS})
                    set(name ps1_jcz_gpu_${n})
                else()
                    set(bios hle)
                    set(name ps1_hle_jcz_gpu_${n})
                endif()
                add_test(NAME ${name} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_jcz_vram.sh $<TARGET_FILE:ps1_run>
                         ${bios} ${PS1_TESTS_DIR}/gpu/${t}/${t}.exe ${PS1_TESTS_DIR}/gpu/${t}/vram.png
                         ${CMAKE_CURRENT_BINARY_DIR}/${name} ${PS1_JCZ_VRAM_${t}})
                list(APPEND jcz_more ${name})
            endforeach()
        endforeach()
        # mdec/frame (XBA-200): the 320x240 picture the MDEC decodes, read by DMA1 and by the CPU, within one 5-bit unit
        # of the hardware dump (the IDCT's last unit of rounding is not the hardware's yet). The CPU build waits for
        # the output FIFO to fill again after its very last word, which the hardware does not do either: its last
        # 16-pixel column is never uploaded (the release's dump is the DMA build's), so it is left out.
        # frame-24bit-dma: the 24-bit picture (480 halfwords wide) byte by byte, within 4 (compare_vram.py --bytes).
        # frame-24bit (CPU) copies the 24-bit output as it comes, unswizzled: left out (the hardware's 24-bit block
        # order on a PIO read is not known).
        foreach(t frame-15bit-dma frame-15bit frame-24bit-dma)
            string(REPLACE "-" "_" n ${t})
            set(region --region 0,0,320,240 --tolerance 1)
            set(ref vram-15bit.png)
            if(t STREQUAL frame-15bit)
                set(region --region 0,0,304,240 --tolerance 1)
            elseif(t STREQUAL frame-24bit-dma)
                set(region --region 0,0,480,240 --bytes 4)
                set(ref vram-24bit.png)
            endif()
            foreach(b lle hle)
                if(b STREQUAL lle)
                    set(bios ${PS1_BIOS})
                    set(name ps1_jcz_mdec_${n})
                else()
                    set(bios hle)
                    set(name ps1_hle_jcz_mdec_${n})
                endif()
                add_test(NAME ${name} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_jcz_vram.sh $<TARGET_FILE:ps1_run>
                         ${bios} ${PS1_TESTS_DIR}/mdec/frame/${t}.exe ${PS1_TESTS_DIR}/mdec/frame/${ref}
                         ${CMAKE_CURRENT_BINARY_DIR}/${name} ${region})
                list(APPEND jcz_more ${name})
            endforeach()
        endforeach()
    endif()
    if(PS1_HAVE_GPU)
        foreach(b lle hle)
            if(b STREQUAL lle)
                set(bios ${PS1_BIOS})
                set(p ps1_jcz)
            else()
                set(bios hle)
                set(p ps1_hle_jcz)
            endif()
            set(J sh ${CMAKE_CURRENT_SOURCE_DIR}/run_jcz.sh $<TARGET_FILE:ps1_run> ${bios})
            add_test(NAME ${p}_gpu_gp0_e1 COMMAND ${J} ${PS1_TESTS_DIR}/gpu/gp0-e1/gp0-e1.exe
                     "pass - testUnsetAllowTextureDisableClearsBitAfterWrite")
            add_test(NAME ${p}_gpu_mask_bit COMMAND ${J} ${PS1_TESTS_DIR}/gpu/mask-bit/mask-bit.exe
                     "pass - testCheckIsMaskBitStickySetBit")
            # no "Done." in these two: the text after a fixed number of frames
            set(R sh ${CMAKE_CURRENT_SOURCE_DIR}/run_exe.sh $<TARGET_FILE:ps1_run> ${bios})
            add_test(NAME ${p}_gpu_version_detect COMMAND sh ${PS1_REPO_ROOT}/tools/consoles/expect_output.sh
                     "GPUREAD = 0x00000002|GPU version 2" ${R} ${PS1_TESTS_DIR}/gpu/version-detect/version-detect.exe
                     --tty --frames 120 --quiet)
            add_test(NAME ${p}_dma_chain_looping COMMAND sh ${PS1_REPO_ROOT}/tools/consoles/expect_output.sh
                     "empty but complete chain...|finished = true, irq = true|single self-referencing chain...|double self-referencing chain...|finished = false, irq = false"
                     ${R} ${PS1_TESTS_DIR}/dma/chain-looping/chain-looping.exe --tty --frames 600 --quiet)
            # input/pad: Up and Cross held (ps1_run --pad), the program prints the buttons it sees
            add_test(NAME ${p}_input_pad COMMAND sh ${PS1_REPO_ROOT}/tools/consoles/expect_output.sh "PAD_UP|PAD_X"
                     ${R} ${PS1_TESTS_DIR}/input/pad/pad.exe --tty --frames 120 --pad 0x4010 --quiet)
            # mdec/4bit: the decoded 8x8 block exactly as the hardware's psx.log (read by DMA1 with BA = 0)
            # ("Done" without the full stop: the text after a fixed number of frames)
            add_test(NAME ${p}_mdec_4bit COMMAND sh ${PS1_REPO_ROOT}/tools/consoles/expect_output.sh
                     "0: f0 0f ff 00|4: fd ff ff 0f|8: ed ff ff 0f|c: fb ff ff 0e|10: f0 ff ff 00|14: 00 ff 1f 02|18: 31 f2 30 02|1c: 41 34 33 11"
                     ${R} ${PS1_TESTS_DIR}/mdec/4bit/4bit.exe --tty --frames 120 --quiet)
            list(APPEND jcz_more ${p}_gpu_gp0_e1 ${p}_gpu_mask_bit ${p}_gpu_version_detect ${p}_dma_chain_looping
                 ${p}_input_pad ${p}_mdec_4bit)
        endforeach()
    endif()
    if(jcz_more)
        set_tests_properties(${jcz_more} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    endif()

    # The HLE BIOS (P-5 HLE, XBA-163): the same programs with no BIOS image (ps1_run --bios hle). ps1_hle_hb_<prog>:
    # PASS; ps1_hle_same_<prog>: the same result block as under OpenBIOS (checksums, every table entry);
    # ps1_hle_hb_disc_<prog>: booted from the disc by the HLE BIOS (SYSTEM.CNF read from ISO 9660); ps1_hle_mcard: the
    # memory card save through the HLE card file system; ps1_hle_jcz_<test>: the ps1-tests above.
    set(PS1_RUN_HLE sh ${CMAKE_CURRENT_SOURCE_DIR}/run_exe.sh $<TARGET_FILE:ps1_run> hle)
    set(PS1_SAME sh ${CMAKE_CURRENT_SOURCE_DIR}/compare_bios.sh $<TARGET_FILE:ps1_run> ${PS1_BIOS})
    set(hle)
    foreach(prog ${hb})
        add_test(NAME ps1_hle_hb_${prog} COMMAND ${PS1_RUN_HLE} ${PS1_HOMEBREW_DIR}/${prog}.exe --result --frames 900
                 --quiet)
        add_test(NAME ps1_hle_same_${prog} COMMAND ${PS1_SAME} ${PS1_HOMEBREW_DIR}/${prog}.exe --frames 900)
        list(APPEND hle ps1_hle_hb_${prog} ps1_hle_same_${prog})
    endforeach()
    if(Python3_Interpreter_FOUND)
        add_test(NAME ps1_hle_mcard COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/mcard_test.sh $<TARGET_FILE:ps1_run> hle
                 ${PS1_HOMEBREW_DIR}/mcard.exe ${CMAKE_CURRENT_BINARY_DIR}/hle_mcard)
        list(APPEND hle ps1_hle_mcard)
    endif()
    if(PS1_HAVE_CD)
        add_test(NAME ps1_hle_hb_overlay COMMAND ${PS1_RUN_HLE} ${PS1_HOMEBREW_DIR}/overlay.exe
                 --disc ${PS1_HOMEBREW_DIR}/overlay.cue --result --frames 1800 --quiet)
        list(APPEND hle ps1_hle_hb_overlay)
        foreach(prog ${hb} overlay)
            add_test(NAME ps1_hle_hb_disc_${prog} COMMAND ${PS1_RUN_HLE} ${PS1_HOMEBREW_DIR}/${prog}.cue --result
                     --frames 1800 --quiet)
            list(APPEND hle ps1_hle_hb_disc_${prog})
        endforeach()
    endif()
    set(PS1_JCZ_HLE sh ${CMAKE_CURRENT_SOURCE_DIR}/run_jcz.sh $<TARGET_FILE:ps1_run> hle)
    add_test(NAME ps1_hle_jcz_cpu_cop COMMAND ${PS1_JCZ_HLE} ${PS1_TESTS_DIR}/cpu/cop/cop.exe
             "testDisabledCoprocessorThrows")
    add_test(NAME ps1_hle_jcz_cpu_code_in_io COMMAND ${PS1_JCZ_HLE} ${PS1_TESTS_DIR}/cpu/code-in-io/code-in-io.exe
             "pass - testCodeInDMAControl")
    add_test(NAME ps1_hle_jcz_gte_test_all COMMAND ${PS1_JCZ_HLE} ${PS1_TESTS_DIR}/gte/test-all/test-all.exe
             "Passed tests: 1150" "Failed tests: 0")
    add_test(NAME ps1_hle_jcz_dma_otc COMMAND ${PS1_JCZ_HLE} ${PS1_TESTS_DIR}/dma/otc-test/otc-test.exe
             "pass - testOtcStandardWithMasterDisabled")
    add_test(NAME ps1_hle_jcz_dma_dpcr COMMAND ${PS1_JCZ_HLE} ${PS1_TESTS_DIR}/dma/dpcr/dpcr.exe
             "pass - testSPUDMARead")
    add_test(NAME ps1_hle_jcz_timers COMMAND ${PS1_JCZ_HLE} ${PS1_TESTS_DIR}/timers/timers.exe
             "Reached target:                      1 (ok)" "Counter reset AFTER reaching target: 1 (ok)")
    list(APPEND hle ps1_hle_jcz_cpu_cop ps1_hle_jcz_cpu_code_in_io ps1_hle_jcz_gte_test_all ps1_hle_jcz_dma_otc
         ps1_hle_jcz_dma_dpcr ps1_hle_jcz_timers)
    if(EXISTS ${PS1_RT}/ps1_spu.c)
        add_test(NAME ps1_hle_jcz_spu_memory_transfer COMMAND ${PS1_JCZ_HLE}
                 ${PS1_TESTS_DIR}/spu/memory-transfer/memory-transfer.exe "pass - testDMAReadTiming"
                 "pass - testDMAReadToRamSyncMode0")
        list(APPEND hle ps1_hle_jcz_spu_memory_transfer)
    endif()
    if(PS1_HAVE_CD)
        foreach(t getloc timing disc-swap)
            string(REPLACE "-" "_" n ${t})
            add_test(NAME ps1_hle_jcz_cdrom_${n} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_jcz_cd.sh
                     $<TARGET_FILE:ps1_run> hle ${PS1_TESTS_DIR}/cdrom/${t}/${t}.exe
                     ${CMAKE_CURRENT_BINARY_DIR}/hle_jcz_cdrom_${n} ${t})
            list(APPEND hle ps1_hle_jcz_cdrom_${n})
        endforeach()
    endif()
    set_tests_properties(${hle} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

    # Static recompilation (P-2 XBA-160, P-3 XBA-161, P-4 XBA-162): each program above lifted to C (ps1_tools.recomp),
    # built with the host cc at -O1, the discovery loop run to convergence (overlay: the overlays read from the disc
    # are recorded, bytes included, and lifted on the next round; both share 0x80100000), then run in lockstep with
    # the interpreter (recomp_exe.sh: the whole state compared after every frame). ps1_rc_fuzz: random MIPS code under
    # a fast timer IRQ, lifted and compared the same way (recomp_fuzz.py).
    set(PS1_RC_EXE sh ${CMAKE_CURRENT_SOURCE_DIR}/recomp_exe.sh $<TARGET_FILE:ps1_run> ${PS1_BIOS})
    set(PS1_RC_WORK ${CMAKE_CURRENT_BINARY_DIR}/recomp)
    set(rc)
    foreach(prog ${hb})
        add_test(NAME ps1_rc_hb_${prog} COMMAND ${PS1_RC_EXE} ${PS1_HOMEBREW_DIR}/${prog}.exe ${PS1_RC_WORK}/hb_${prog}
                 900 --result --quiet)
        list(APPEND rc ps1_rc_hb_${prog})
    endforeach()
    if(PS1_HAVE_CD)
        add_test(NAME ps1_rc_hb_overlay COMMAND ${PS1_RC_EXE} ${PS1_HOMEBREW_DIR}/overlay.exe ${PS1_RC_WORK}/hb_overlay
                 1800 --disc ${PS1_HOMEBREW_DIR}/overlay.cue --result --quiet)
        add_test(NAME ps1_rc_hb_disc_overlay COMMAND ${PS1_RC_EXE} ${PS1_HOMEBREW_DIR}/overlay.cue
                 ${PS1_RC_WORK}/hb_disc_overlay 3000 --result --quiet)
        list(APPEND rc ps1_rc_hb_overlay ps1_rc_hb_disc_overlay)
    endif()
    foreach(t cpu/cop/cop cpu/code-in-io/code-in-io gte/test-all/test-all dma/otc-test/otc-test dma/dpcr/dpcr
              timers/timers)
        get_filename_component(n ${t} NAME)
        string(REPLACE "-" "_" n ${n})
        add_test(NAME ps1_rc_jcz_${n} COMMAND ${PS1_RC_EXE} ${PS1_TESTS_DIR}/${t}.exe ${PS1_RC_WORK}/jcz_${n} 12000
                 --tty --until-tty "Done." --quiet)
        list(APPEND rc ps1_rc_jcz_${n})
    endforeach()
    if(Python3_Interpreter_FOUND)
        add_test(NAME ps1_rc_fuzz
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recomp_fuzz.py --ps1-run $<TARGET_FILE:ps1_run>
                    --bios ${PS1_BIOS} --work ${PS1_RC_WORK}/fuzz --seeds 0-5 --frames 40)
        list(APPEND rc ps1_rc_fuzz)
    endif()
    # ... and under the HLE BIOS: lifted code reaches the BIOS through the same 0xA0 / 0xB0 / 0xC0 vectors (kernel RAM
    # stays interpreted), in lockstep with the interpreter.
    foreach(prog ${hb})
        add_test(NAME ps1_hle_rc_hb_${prog} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/recomp_exe.sh $<TARGET_FILE:ps1_run>
                 hle ${PS1_HOMEBREW_DIR}/${prog}.exe ${PS1_RC_WORK}/hle_hb_${prog} 900 --result --quiet)
        list(APPEND rc ps1_hle_rc_hb_${prog})
    endforeach()
    # Cheats (XBA-197) on the recompiled hello in lockstep with the interpreter, the codes running on both.
    add_test(NAME ps1_hle_rc_cheats_hb_hello COMMAND sh ${PS1_REPO_ROOT}/tools/consoles/expect_output.sh
             "LOCKSTEP OK|peek 0x801F01B0 = 0x00561234|peek 0x801F01C0 = 0x00001234"
             sh ${CMAKE_CURRENT_SOURCE_DIR}/recomp_exe.sh $<TARGET_FILE:ps1_run> hle ${PS1_HOMEBREW_DIR}/hello.exe
             ${PS1_RC_WORK}/hle_cheats_hello 900 --result --quiet
             --cheats ${CMAKE_CURRENT_SOURCE_DIR}/cheats/hello.cheats --peek 0x801F01B0 --peek 0x801F01C0)
    list(APPEND rc ps1_hle_rc_cheats_hb_hello)
    if(PS1_HAVE_CD)
        add_test(NAME ps1_hle_rc_hb_disc_overlay COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/recomp_exe.sh
                 $<TARGET_FILE:ps1_run> hle ${PS1_HOMEBREW_DIR}/overlay.cue ${PS1_RC_WORK}/hle_hb_disc_overlay 3000
                 --result --quiet)
        list(APPEND rc ps1_hle_rc_hb_disc_overlay)
    endif()
    add_test(NAME ps1_hle_rc_jcz_timers COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/recomp_exe.sh $<TARGET_FILE:ps1_run> hle
             ${PS1_TESTS_DIR}/timers/timers.exe ${PS1_RC_WORK}/hle_jcz_timers 12000 --tty --until-tty "Done." --quiet)
    list(APPEND rc ps1_hle_rc_jcz_timers)
    set_tests_properties(${rc} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)

    # The interpreter's register-cached loop and its predecoded blocks (XBA-204) against its reference loop, one full
    # step per instruction (ps1_run --lockstep-ref): the whole state compared after every frame, and with
    # --lockstep-cycles the CPU every 61 cycles (the whole machine every 1024 stops), which also cuts blocks short
    # at every point; OpenBIOS and the HLE BIOS; random MIPS code under fast timer IRQs (recomp_fuzz.py --ref).
    set(lockref)
    foreach(prog ${hb})
        add_test(NAME ps1_lockref_hb_${prog} COMMAND ${PS1_RUN_EXE} ${PS1_HOMEBREW_DIR}/${prog}.exe --frames 900
                 --lockstep-ref --quiet)
        add_test(NAME ps1_lockref_cyc_hb_${prog} COMMAND ${PS1_RUN_EXE} ${PS1_HOMEBREW_DIR}/${prog}.exe --frames 60
                 --lockstep-ref --lockstep-cycles 61 --quiet)
        add_test(NAME ps1_hle_lockref_hb_${prog} COMMAND ${PS1_RUN_HLE} ${PS1_HOMEBREW_DIR}/${prog}.exe --frames 900
                 --lockstep-ref --quiet)
        list(APPEND lockref ps1_lockref_hb_${prog} ps1_lockref_cyc_hb_${prog} ps1_hle_lockref_hb_${prog})
    endforeach()
    if(PS1_HAVE_CD)
        add_test(NAME ps1_lockref_hb_disc_overlay COMMAND ${PS1_RUN_EXE} ${PS1_HOMEBREW_DIR}/overlay.cue --frames 900
                 --lockstep-ref --quiet)
        list(APPEND lockref ps1_lockref_hb_disc_overlay)
    endif()
    foreach(t cpu/cop/cop cpu/code-in-io/code-in-io gte/test-all/test-all dma/otc-test/otc-test dma/dpcr/dpcr
              timers/timers gpu/triangle/triangle)
        get_filename_component(n ${t} NAME)
        string(REPLACE "-" "_" n ${n})
        add_test(NAME ps1_lockref_jcz_${n} COMMAND ${PS1_RUN_EXE} ${PS1_TESTS_DIR}/${t}.exe --frames 600
                 --lockstep-ref --quiet)
        list(APPEND lockref ps1_lockref_jcz_${n})
    endforeach()
    if(Python3_Interpreter_FOUND)
        add_test(NAME ps1_lockref_fuzz
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recomp_fuzz.py --ps1-run $<TARGET_FILE:ps1_run>
                    --bios ${PS1_BIOS} --work ${CMAKE_CURRENT_BINARY_DIR}/lockref_fuzz --seeds 0-7 --frames 50
                    --ref --lockstep-cycles 61)
        list(APPEND lockref ps1_lockref_fuzz)
    endif()
    set_tests_properties(${lockref} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

    # Rewind (XBA-194, ps1_run --rewind): 600 frames with pseudo-random pads, a capture every 3 frames as the runtime
    # takes them, 60 steps back: the picture there and the frames after it must be the first run's, on every homebrew
    # (HLE BIOS) and through a disc read (overlay); ps1_rewind_cap: a cap just over the newest capture and its coding
    # buffer (4.65 MB since XBA-278 made the coding buffer as large as the deltas; 8.45 before) drops the oldest
    # captures, the rest still come back exact.
    set(rw)
    foreach(prog ${hb})
        add_test(NAME ps1_rewind_hb_${prog} COMMAND ${PS1_RUN_HLE} ${PS1_HOMEBREW_DIR}/${prog}.exe --frames 600
                 --rewind 3:60 --quiet)
        list(APPEND rw ps1_rewind_hb_${prog})
    endforeach()
    if(PS1_HAVE_CD)
        add_test(NAME ps1_rewind_hb_overlay COMMAND ${PS1_RUN_HLE} ${PS1_HOMEBREW_DIR}/overlay.exe
                 --disc ${PS1_HOMEBREW_DIR}/overlay.cue --frames 900 --rewind 3:120 --quiet)
        list(APPEND rw ps1_rewind_hb_overlay)
    endif()
    add_test(NAME ps1_rewind_cap COMMAND ${PS1_RUN_HLE} ${PS1_HOMEBREW_DIR}/timer_irq.exe --frames 900
             --rewind 1:800 --rewind-mb 4.65 --quiet)
    list(APPEND rw ps1_rewind_cap)
    set_tests_properties(${rw} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

    # Cheats (XBA-197, ps1_run --cheats cheats/hello.cheats): GameShark 30 / 80 writes, a D0 conditional on the result
    # block's status, a 50 serial repeater, a C2 copy and a cheat that is off, read back after the verdict (--peek).
    string(CONCAT want "PASS|peek 0x801F01B0 = 0x00561234|peek 0x801F01B4 = 0x0000BEEF|peek 0x801F01B8 = 0x00110010"
           "|peek 0x801F01BC = 0x00000012|peek 0x801F01C0 = 0x00001234")
    add_test(NAME ps1_cheats_hb_hello COMMAND sh ${PS1_REPO_ROOT}/tools/consoles/expect_output.sh "${want}"
             ${PS1_RUN_HLE} ${PS1_HOMEBREW_DIR}/hello.exe --result --frames 900 --quiet
             --cheats ${CMAKE_CURRENT_SOURCE_DIR}/cheats/hello.cheats
             --peek 0x801F01B0 --peek 0x801F01B4 --peek 0x801F01B8 --peek 0x801F01BC --peek 0x801F01C0)
    set_tests_properties(ps1_cheats_hb_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

    # Save states (XBA-182, state_exe.sh): the round trip (a state saved after frame 150, the next 150 frames hashed,
    # then the same from the state loaded in a new machine) on every homebrew under OpenBIOS and the HLE BIOS, a disc
    # read in progress (overlay.cue fast-booted: the CD-ROM controller, its sector buffers and the drive's position
    # are in the state), with each program's pack (ps1_rc_state_*: states cross between lifted and interpreted
    # runs), and refusals: another game's state, a state taken under another BIOS.
    set(PS1_STATE sh ${CMAKE_CURRENT_SOURCE_DIR}/state_exe.sh $<TARGET_FILE:ps1_run>)
    set(PS1_STATE_WORK ${CMAKE_CURRENT_BINARY_DIR}/state)
    set(st)
    foreach(prog ${hb})
        add_test(NAME ps1_state_hb_${prog} COMMAND ${PS1_STATE} ${PS1_BIOS} ${PS1_HOMEBREW_DIR}/${prog}.exe
                 ${PS1_STATE_WORK}/hb_${prog} interp)
        add_test(NAME ps1_state_hle_hb_${prog} COMMAND ${PS1_STATE} hle ${PS1_HOMEBREW_DIR}/${prog}.exe
                 ${PS1_STATE_WORK}/hle_hb_${prog} interp)
        add_test(NAME ps1_rc_state_hb_${prog} COMMAND ${PS1_STATE} ${PS1_BIOS} ${PS1_HOMEBREW_DIR}/${prog}.exe
                 ${PS1_STATE_WORK}/rc_hb_${prog} pack)
        list(APPEND st ps1_state_hb_${prog} ps1_state_hle_hb_${prog} ps1_rc_state_hb_${prog})
    endforeach()
    if(PS1_HAVE_CD)
        add_test(NAME ps1_state_hb_disc_overlay COMMAND ${PS1_STATE} ${PS1_BIOS} ${PS1_HOMEBREW_DIR}/overlay.cue
                 ${PS1_STATE_WORK}/hb_disc_overlay interp --fast-boot)
        add_test(NAME ps1_rc_state_hb_disc_overlay COMMAND ${PS1_STATE} ${PS1_BIOS} ${PS1_HOMEBREW_DIR}/overlay.cue
                 ${PS1_STATE_WORK}/rc_hb_disc_overlay pack --fast-boot)
        add_test(NAME ps1_state_hle_hb_disc_overlay COMMAND ${PS1_STATE} hle ${PS1_HOMEBREW_DIR}/overlay.cue
                 ${PS1_STATE_WORK}/hle_hb_disc_overlay interp)
        set_tests_properties(ps1_state_hb_disc_overlay ps1_rc_state_hb_disc_overlay ps1_state_hle_hb_disc_overlay
                             PROPERTIES ENVIRONMENT "STATE_N=100;STATE_M=200")
        list(APPEND st ps1_state_hb_disc_overlay ps1_rc_state_hb_disc_overlay ps1_state_hle_hb_disc_overlay)
    endif()
    add_test(NAME ps1_state_wrong_game COMMAND ${PS1_STATE} ${PS1_BIOS} ${PS1_HOMEBREW_DIR}/timer_irq.exe
             ${PS1_STATE_WORK}/wrong_game other:${PS1_HOMEBREW_DIR}/cpu.exe)
    add_test(NAME ps1_state_wrong_bios COMMAND ${PS1_STATE} ${PS1_BIOS} ${PS1_HOMEBREW_DIR}/timer_irq.exe
             ${PS1_STATE_WORK}/wrong_bios bios:hle)
    list(APPEND st ps1_state_wrong_game ps1_state_wrong_bios)
    set_tests_properties(${st} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
