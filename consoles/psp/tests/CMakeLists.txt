cmake_minimum_required(VERSION 3.20)
project(console_psp_tests C)

if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()
# The PSP spike's tests (XBA-224): a self-contained project like those under tests/, run by tools/run_c_tests.sh (as
# console_psp) and by the root build's ctest when XBR_CONSOLE_PSP is ON. POSIX hosts.
#
#   psp_hello      our PSPSDK hello world (consoles/psp/tests/homebrew/hello, its PRX) under psp_run, interpreter
#                  only: the framebuffer when it exits must equal hello.png
#   psp_rc_hello   the same module recompiled (psp_tools.recomp + cc, discovery loop), mostly native: same picture
#   psp_pbp_hello  the EBOOT.PBP the PRX ships in, interpreter only: same picture
# All skip (exit 77) when the homebrew is not built (the linux CI job has no corpus; the homebrew job builds it):
#   tools/consoles/build_homebrew.sh psp build/homebrew    -> PSP_HOMEBREW_DIR (default build/homebrew/psp)
# The recompiler's own tests (synthetic code, no toolchain) run with the Python suite: python3 -m pytest consoles/.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET psp_core)
    add_subdirectory(../runtime psp_runtime)
endif()
enable_testing()

get_filename_component(PSP_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(PSP_HOMEBREW_DIR ${PSP_REPO_ROOT}/build/homebrew/psp CACHE PATH "Built PSP homebrew (build_homebrew.sh psp)")
set(PSP_RUN_HELLO ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh)

if(TARGET psp_run)
    add_test(NAME psp_hello
        COMMAND sh ${PSP_RUN_HELLO} $<TARGET_FILE:psp_run> ${PSP_HOMEBREW_DIR}/hello.prx
                ${CMAKE_CURRENT_BINARY_DIR}/hello_interp interp ${CMAKE_CURRENT_SOURCE_DIR}/hello.png)
    add_test(NAME psp_pbp_hello
        COMMAND sh ${PSP_RUN_HELLO} $<TARGET_FILE:psp_run> ${PSP_HOMEBREW_DIR}/hello.pbp
                ${CMAKE_CURRENT_BINARY_DIR}/hello_pbp interp ${CMAKE_CURRENT_SOURCE_DIR}/hello.png)
    add_test(NAME psp_rc_hello
        COMMAND sh ${PSP_RUN_HELLO} $<TARGET_FILE:psp_run> ${PSP_HOMEBREW_DIR}/hello.prx
                ${CMAKE_CURRENT_BINARY_DIR}/hello_rc rc ${CMAKE_CURRENT_SOURCE_DIR}/hello.png)
    set_tests_properties(psp_hello psp_pbp_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    set_tests_properties(psp_rc_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
