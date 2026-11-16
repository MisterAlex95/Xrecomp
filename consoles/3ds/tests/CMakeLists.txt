cmake_minimum_required(VERSION 3.20)
project(console_3ds_tests C)

if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()
# The Nintendo 3DS spike's tests (XBA-228): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_3ds) and by the root build's ctest when XBR_CONSOLE_3DS is ON. POSIX hosts.
#
#   ctr_hello      our libctru hello world (consoles/3ds/tests/homebrew/hello) under ctr_run, interpreter only: the
#                  top screen after 60 frames must equal hello_top.png
#   ctr_rc_hello   the same code recompiled (ctr_tools.recomp + cc, discovery loop), mostly native: same picture
# Both skip (exit 77) when the homebrew is not built (the linux CI job has no corpus; the homebrew job builds it):
#   tools/consoles/build_homebrew.sh 3ds build/homebrew    -> CTR_HOMEBREW_DIR (default build/homebrew/3ds)
# The recompiler's own tests (synthetic code, no toolchain) run with the Python suite: python3 -m pytest consoles/.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET ctr_core)
    add_subdirectory(../runtime ctr_runtime)
endif()
enable_testing()

get_filename_component(CTR_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(CTR_HOMEBREW_DIR ${CTR_REPO_ROOT}/build/homebrew/3ds CACHE PATH "Built 3DS homebrew (build_homebrew.sh 3ds)")
set(CTR_RUN_HELLO ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh)

if(TARGET ctr_run)
    add_test(NAME ctr_hello
        COMMAND sh ${CTR_RUN_HELLO} $<TARGET_FILE:ctr_run> ${CTR_HOMEBREW_DIR}/hello.3dsx
                ${CMAKE_CURRENT_BINARY_DIR}/hello_interp interp ${CMAKE_CURRENT_SOURCE_DIR}/hello_top.png)
    add_test(NAME ctr_rc_hello
        COMMAND sh ${CTR_RUN_HELLO} $<TARGET_FILE:ctr_run> ${CTR_HOMEBREW_DIR}/hello.3dsx
                ${CMAKE_CURRENT_BINARY_DIR}/hello_rc rc ${CMAKE_CURRENT_SOURCE_DIR}/hello_top.png)
    set_tests_properties(ctr_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    set_tests_properties(ctr_rc_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
