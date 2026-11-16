cmake_minimum_required(VERSION 3.20)
project(console_ps2_tests C)

if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()
# The PS2 spike's tests (XBA-226): a self-contained project like those under tests/, run by tools/run_c_tests.sh (as
# console_ps2) and by the root build's ctest when XBR_CONSOLE_PS2 is ON. POSIX hosts.
#
#   ps2_hello      our PS2SDK hello world (consoles/ps2/tests/homebrew/hello, its ELF) under ps2_run, interpreter
#                  only: what it writes to the EE's SIO until it exits must equal hello.txt
#   ps2_rc_hello   the same executable recompiled (ps2_tools.recomp + cc, discovery loop), mostly native: same text
# Both skip (exit 77) when the homebrew is not built (the linux CI job has no corpus; the homebrew job builds it):
#   tools/consoles/build_homebrew.sh ps2 build/homebrew    -> PS2_HOMEBREW_DIR (default build/homebrew/ps2)
# The recompiler's own tests (synthetic code, no toolchain) run with the Python suite: python3 -m pytest consoles/.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET ps2_core)
    add_subdirectory(../runtime ps2_runtime)
endif()
enable_testing()

get_filename_component(PS2_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(PS2_HOMEBREW_DIR ${PS2_REPO_ROOT}/build/homebrew/ps2 CACHE PATH "Built PS2 homebrew (build_homebrew.sh ps2)")
set(PS2_RUN_HELLO ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh)

if(TARGET ps2_run)
    add_test(NAME ps2_hello
        COMMAND sh ${PS2_RUN_HELLO} $<TARGET_FILE:ps2_run> ${PS2_HOMEBREW_DIR}/hello.elf
                ${CMAKE_CURRENT_BINARY_DIR}/hello_interp interp ${CMAKE_CURRENT_SOURCE_DIR}/hello.txt)
    add_test(NAME ps2_rc_hello
        COMMAND sh ${PS2_RUN_HELLO} $<TARGET_FILE:ps2_run> ${PS2_HOMEBREW_DIR}/hello.elf
                ${CMAKE_CURRENT_BINARY_DIR}/hello_rc rc ${CMAKE_CURRENT_SOURCE_DIR}/hello.txt)
    set_tests_properties(ps2_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    set_tests_properties(ps2_rc_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
