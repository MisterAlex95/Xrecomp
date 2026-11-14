cmake_minimum_required(VERSION 3.20)
project(console_n64_tests C)
# The Nintendo 64 runtime's C tests (XBA-146): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_n64) and by the root build's ctest when XBR_CONSOLE_N64 is ON. POSIX hosts.
#
#   n64_runtime       the runtime: helpers, headless runs of gfx.z64 (saves, fd:N, .v64, the app shell's entry);
#                     needs xbr_host / SDL2, not built with N64_CORE_ONLY=ON
#   n64_rdp           RDP feature tests on synthetic command lists (test_rdp.c, with n64_rdp.c); n64_rdp_bench, its
#                     benchmark, runs by hand (n64_rdp_bench [seconds per case])
#   n64_rsp           RSP vectors and differential groups vs ares (test_rsp.c); n64_rsp_rdp: RSP microcode driving
#                     the real RDP over XBUS (test_rsp_rdp.c); n64_rsp_bench runs by hand
#   n64_cpu           core unit tests (test_cpu.c): integer unit, delay slots, exceptions, interrupts, TLB, COP1
#   n64_io            core unit tests (test_io.c): ROM byte orders / header, PIF boot state, joybus (paks, EEPROM
#                     type, RTC), PI, SRAM, FlashRAM, the AI and its resampler (tones, rate change), the VI picture
#                     path (canvas, scaling, filters, gamma, interlace)
#   n64_hb_<prog>     our homebrew ROMs under n64_run --result (HLE PIF, libdragon IPL3 run as on the hardware);
#                     rsp needs the real RSP (n64_rsp.c), gfx the real RSP and RDP (n64_rsp.c, n64_rdp.c)
#   n64_rc_*          the same ROMs recompiled (n64_tools.recomp + cc, discovery loop) in lockstep with the
#                     interpreter; n64_rc_systemtest the suite under --pack; n64_rc_fuzz random MIPS III code
#   n64_systemtest    lemmy-64/n64-systemtest (MIT) built from source: every CPU / COP0 / TLB / COP1 / memory test
#                     must pass but the known gaps listed in systemtest_known.txt (RSP / RDP tests only count with
#                     the real modules)
# The ROM tests skip (exit 77) when their inputs are absent, so a CI job without Docker or network stays green.
# They come from (repo root):
#   tools/consoles/build_homebrew.sh n64 build/homebrew     -> N64_HOMEBREW_DIR (default build/homebrew/n64)
#   tools/consoles/build_n64_systemtest.sh build/n64-systemtest -> N64_SYSTEMTEST (default
#                                                              build/n64-systemtest/n64-systemtest.z64)
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only (xbr_host)")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET n64_core)
    add_subdirectory(../runtime n64_runtime)
endif()

enable_testing()

get_filename_component(N64_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(N64_HOMEBREW_DIR ${N64_REPO_ROOT}/build/homebrew/n64 CACHE PATH "Built N64 homebrew (build_homebrew.sh n64)")

# The runtime (XBA-174): helpers always, headless runs of gfx.z64 when the homebrew is built.
if(TARGET n64_runtime)
    add_executable(n64_runtime_test test_main.c)
    target_link_libraries(n64_runtime_test PRIVATE n64_runtime)
    target_compile_options(n64_runtime_test PRIVATE -Wall -Wextra)
    add_test(NAME n64_runtime COMMAND n64_runtime_test ${N64_HOMEBREW_DIR})
    set_tests_properties(n64_runtime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()

if(TARGET n64_rdp AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/n64_rdp.c)
    add_executable(n64_rdp_test test_rdp.c)
    target_link_libraries(n64_rdp_test PRIVATE n64_rdp m)
    target_compile_options(n64_rdp_test PRIVATE -Wall -Wextra)
    add_test(NAME n64_rdp COMMAND n64_rdp_test)
    set_tests_properties(n64_rdp PROPERTIES TIMEOUT 120)
    add_executable(n64_rdp_bench bench_rdp.c)
    target_link_libraries(n64_rdp_bench PRIVATE n64_rdp m)
    target_compile_options(n64_rdp_bench PRIVATE -Wall -Wextra -O2)
endif()

# The RSP (N-5, XBA-173): hand-checked vectors and the differential groups of homebrew/common/rsp_oracle.h against
# the checksums recorded from ares (rsp_oracle_expected.h), with a fake RDP (test_rsp.c); test_rsp_rdp.c drives the
# real RDP through the DPC registers (an XBUS command list built by RSP microcode). n64_rsp_bench, RSP instructions/s
# on a vector-heavy microcode loop, runs by hand (n64_rsp_bench [seconds]).
if(TARGET n64_rsp AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/n64_rsp.c)
    add_executable(n64_rsp_test test_rsp.c)
    target_link_libraries(n64_rsp_test PRIVATE n64_rsp)
    target_compile_options(n64_rsp_test PRIVATE -Wall -Wextra)
    add_test(NAME n64_rsp COMMAND n64_rsp_test)
    set_tests_properties(n64_rsp PROPERTIES TIMEOUT 120)
    add_executable(n64_rsp_bench bench_rsp.c)
    target_link_libraries(n64_rsp_bench PRIVATE n64_rsp)
    target_compile_options(n64_rsp_bench PRIVATE -Wall -Wextra -O2)
    if(TARGET n64_rdp AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/n64_rdp.c)
        add_executable(n64_rsp_rdp_test test_rsp_rdp.c)
        target_link_libraries(n64_rsp_rdp_test PRIVATE n64_rsp n64_rdp m)
        target_compile_options(n64_rsp_rdp_test PRIVATE -Wall -Wextra -Wno-unused-function) # rdp_cmd.h scenes
        add_test(NAME n64_rsp_rdp COMMAND n64_rsp_rdp_test)
        set_tests_properties(n64_rsp_rdp PROPERTIES TIMEOUT 60)
    endif()
endif()

foreach(t cpu io)
    add_executable(n64_${t}_test test_${t}.c)
    target_link_libraries(n64_${t}_test PRIVATE n64_core)
    target_compile_options(n64_${t}_test PRIVATE -Wall -Wextra)
    add_test(NAME n64_${t} COMMAND n64_${t}_test)
    set_tests_properties(n64_${t} PROPERTIES TIMEOUT 60)
endforeach()

set(N64_SYSTEMTEST ${N64_REPO_ROOT}/build/n64-systemtest/n64-systemtest.z64 CACHE FILEPATH
    "n64-systemtest ROM (build_n64_systemtest.sh)")
set(N64_RT ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)
set(N64_HAVE_RCP OFF)
if(EXISTS ${N64_RT}/n64_rsp.c AND EXISTS ${N64_RT}/n64_rdp.c)
    set(N64_HAVE_RCP ON)
endif()

if(TARGET n64_run)
    set(hb hello cpu timer_irq)
    if(EXISTS ${N64_RT}/n64_rsp.c)
        list(APPEND hb rsp)
    endif()
    if(N64_HAVE_RCP)
        list(APPEND hb gfx)
    endif()
    foreach(prog ${hb})
        add_test(NAME n64_hb_${prog} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_rom.sh $<TARGET_FILE:n64_run>
                 ${N64_HOMEBREW_DIR}/${prog}.z64 --result --frames 900 --quiet)
        set_tests_properties(n64_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endforeach()
    add_test(NAME n64_systemtest COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_systemtest.sh $<TARGET_FILE:n64_run>
             ${N64_SYSTEMTEST} ${CMAKE_CURRENT_SOURCE_DIR}/systemtest_known.txt ${N64_HAVE_RCP})
    set_tests_properties(n64_systemtest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

    # Static recompilation (N-1 XBA-169, N-2 XBA-170, N-3 XBA-171): each homebrew ROM lifted to C from the ROM alone
    # (n64_tools.recomp; its ELF only measures the discovery: >= 99 % of the function symbols found), built with the
    # discovery loop and run in lockstep with the interpreter (recomp_rom.sh: the whole state compared after every
    # frame); n64-systemtest under --pack (the interpreter's pass / known-gap set) and in lockstep
    # (recomp_systemtest.sh); n64_rc_fuzz: random MIPS III code under COUNT = COMPARE interrupts (recomp_fuzz.py).
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        set(N64_RC_WORK ${CMAKE_CURRENT_BINARY_DIR}/recomp)
        foreach(prog ${hb})
            add_test(NAME n64_rc_hb_${prog} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/recomp_rom.sh $<TARGET_FILE:n64_run>
                     ${N64_HOMEBREW_DIR}/${prog}.z64 ${N64_RC_WORK}/hb_${prog} 900 --result)
            set_tests_properties(n64_rc_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800
                                 ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")
        endforeach()
        add_test(NAME n64_rc_systemtest COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/recomp_systemtest.sh
                 $<TARGET_FILE:n64_run> ${N64_SYSTEMTEST} ${CMAKE_CURRENT_SOURCE_DIR}/systemtest_known.txt
                 ${N64_HAVE_RCP} ${N64_RC_WORK}/systemtest)
        set_tests_properties(n64_rc_systemtest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600
                             ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")
        add_test(NAME n64_rc_fuzz COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recomp_fuzz.py
                 --n64-run $<TARGET_FILE:n64_run> --work ${N64_RC_WORK}/fuzz --seeds 0-7 --frames 15 --insns 1500)
        set_tests_properties(n64_rc_fuzz PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
    endif()
endif()

# An N64 title pack (XBA-174): fake_n64_pack is built like tools/prepare's libtitle.so (its core calls resolved against
# the program that loads it), n64_pack_test exports the core and checks n64_pack.h (the homebrew gfx.z64's identity
# too when it is built).
add_library(fake_n64_pack SHARED fake_pack.c)
target_include_directories(fake_n64_pack PRIVATE ../runtime)
if(APPLE)
    target_link_options(fake_n64_pack PRIVATE -undefined dynamic_lookup)
endif()
add_executable(n64_pack_test test_pack.c)
n64_core_exported(n64_pack_test)
target_link_libraries(n64_pack_test PRIVATE n64_pack)
target_compile_options(n64_pack_test PRIVATE -Wall -Wextra)
add_test(NAME n64_pack COMMAND n64_pack_test $<TARGET_FILE:fake_n64_pack> ${N64_HOMEBREW_DIR})
set_tests_properties(n64_pack PROPERTIES TIMEOUT 60)
