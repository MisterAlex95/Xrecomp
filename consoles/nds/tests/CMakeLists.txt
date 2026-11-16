cmake_minimum_required(VERSION 3.20)
project(console_nds_tests C)

if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()
# The Nintendo DS spike's tests (XBA-223): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_nds) and by the root build's ctest when XBR_CONSOLE_NDS is ON. POSIX hosts.
#
#   nds_hello      our libnds hello world (consoles/nds/tests/homebrew/hello) under nds_run, interpreter only: the
#                  top screen after 120 frames must equal hello_top.png
#   nds_rc_hello   the same ARM9 code recompiled (nds_tools.recomp + cc, discovery loop), mostly native: same picture
# Both skip (exit 77) when the homebrew is not built (the linux CI job has no corpus; the homebrew job builds it):
#   tools/consoles/build_homebrew.sh nds build/homebrew    -> NDS_HOMEBREW_DIR (default build/homebrew/nds)
# The recompiler's own tests (synthetic code, no toolchain) run with the Python suite: python3 -m pytest consoles/.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET nds_core)
    add_subdirectory(../runtime nds_runtime)
endif()
enable_testing()

get_filename_component(NDS_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(NDS_HOMEBREW_DIR ${NDS_REPO_ROOT}/build/homebrew/nds CACHE PATH "Built NDS homebrew (build_homebrew.sh nds)")
set(NDS_RUN_HELLO ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh)

if(TARGET nds_run)
    add_test(NAME nds_hello
        COMMAND sh ${NDS_RUN_HELLO} $<TARGET_FILE:nds_run> ${NDS_HOMEBREW_DIR}/hello.nds
                ${CMAKE_CURRENT_BINARY_DIR}/hello_interp interp ${CMAKE_CURRENT_SOURCE_DIR}/hello_top.png)
    add_test(NAME nds_rc_hello
        COMMAND sh ${NDS_RUN_HELLO} $<TARGET_FILE:nds_run> ${NDS_HOMEBREW_DIR}/hello.nds
                ${CMAKE_CURRENT_BINARY_DIR}/hello_rc rc ${CMAKE_CURRENT_SOURCE_DIR}/hello_top.png)
    set_tests_properties(nds_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    set_tests_properties(nds_rc_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
