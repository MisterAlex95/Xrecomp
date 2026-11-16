cmake_minimum_required(VERSION 3.20)
project(console_dc_tests C)

if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()
# The Dreamcast spike's tests (XBA-227): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_dc) and by the root build's ctest when XBR_CONSOLE_DC is ON. POSIX hosts.
#
#   dc_sh4_ops      synthetic SH-4 programs (dc_sh4_test.c: divisions, carries, shifts, MAC, delay slots, TRAPA /
#                   RTE and the banks, FPU single / double, pairs, FIPR / FTRV / FSCA) in the interpreter
#   dc_rc_sh4_ops   the same programs lifted by dc_tools.recomp and compiled: same words (python3 + cc, else 77)
#   dc_naomi        the Naomi mode (XBA-235): a synthetic cartridge built in the test boots through the Naomi BIOS
#                   in C; its program checks the memory map (32 MB RAM, SRAM, 8 MB sound RAM) and the cartridge's PIO
#                   and DMA reads; the Dreamcast map unchanged; the JVS I/O stub's replies
#   dc_hello        our KallistiOS hello world (consoles/dc/tests/homebrew/hello) under dc_run, interpreter only:
#                   its serial lines must equal hello_serial.txt, its framebuffer hello_fb.png, its exit code 0
#   dc_rc_hello     the same program recompiled (dc_tools.recomp + cc, discovery loop), mostly native: same output
#   dc_naomi_hello  the same program (hello.bin) in a synthetic Naomi cartridge under dc_run --naomi: same output
# The hello tests skip (exit 77) when the homebrew is not built (the linux CI job has no corpus; the homebrew job
# builds it): tools/consoles/build_homebrew.sh dc build/homebrew -> DC_HOMEBREW_DIR (default build/homebrew/dc).
# The lifter's own tests (no toolchain) run with the Python suite: python3 -m pytest consoles/dc.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET dc_core)
    add_subdirectory(../runtime dc_runtime)
endif()
enable_testing()

get_filename_component(DC_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(DC_HOMEBREW_DIR ${DC_REPO_ROOT}/build/homebrew/dc CACHE PATH "Built Dreamcast homebrew (build_homebrew.sh dc)")
set(DC_RUN_HELLO ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh)

add_executable(dc_sh4_test dc_sh4_test.c)
target_link_libraries(dc_sh4_test PRIVATE dc_core)
target_compile_options(dc_sh4_test PRIVATE -Wall -Wextra)
add_test(NAME dc_sh4_ops COMMAND dc_sh4_test)

add_executable(dc_naomi_test dc_naomi_test.c)
target_link_libraries(dc_naomi_test PRIVATE dc_core)
target_compile_options(dc_naomi_test PRIVATE -Wall -Wextra)
add_test(NAME dc_naomi COMMAND dc_naomi_test)

if(TARGET dc_run)
    add_test(NAME dc_rc_sh4_ops
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_sh4_rc.sh $<TARGET_FILE:dc_sh4_test> $<TARGET_FILE:dc_run>
                ${CMAKE_CURRENT_BINARY_DIR}/sh4_rc)
    add_test(NAME dc_hello
        COMMAND sh ${DC_RUN_HELLO} $<TARGET_FILE:dc_run> ${DC_HOMEBREW_DIR}/hello.elf
                ${CMAKE_CURRENT_BINARY_DIR}/hello_interp interp)
    add_test(NAME dc_rc_hello
        COMMAND sh ${DC_RUN_HELLO} $<TARGET_FILE:dc_run> ${DC_HOMEBREW_DIR}/hello.elf
                ${CMAKE_CURRENT_BINARY_DIR}/hello_rc rc)
    add_test(NAME dc_naomi_hello
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_naomi_hello.sh $<TARGET_FILE:dc_run> ${DC_HOMEBREW_DIR}/hello.bin
                ${CMAKE_CURRENT_BINARY_DIR}/hello_naomi)
    set_tests_properties(dc_rc_sh4_ops dc_hello dc_rc_hello dc_naomi_hello PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(dc_rc_sh4_ops dc_hello dc_naomi_hello PROPERTIES TIMEOUT 300)
    set_tests_properties(dc_rc_hello PROPERTIES TIMEOUT 900)
endif()
