cmake_minimum_required(VERSION 3.20)
project(console_gc_tests C)

if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()
# The GameCube spike's tests (XBA-225): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_gc) and by the root build's ctest when XBR_CONSOLE_GC is ON. POSIX hosts.
#
#   gc_hello      our libogc hello world (consoles/gc/tests/homebrew/hello) under gc_run, interpreter only: the XFB
#                 after 120 fields must equal hello_xfb.png
#   gc_rc_hello   the same DOL recompiled (gc_tools.recomp + cc, discovery loop), mostly native: same picture
#   gc_fnma       fnmadd / fnmsub and their s / ps_ forms keep an exact zero's -0 and a NaN's sign (no homebrew)
# Both skip (exit 77) when the homebrew is not built (the linux CI job has no corpus; the homebrew job builds it):
#   tools/consoles/build_homebrew.sh gc build/homebrew    -> GC_HOMEBREW_DIR (default build/homebrew/gc)
# The frontend's own tests (synthetic code, no toolchain; one runs a program both interpreted and recompiled) run
# with the Python suite: python3 -m pytest tools/ppc consoles/gc.
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET gc_core)
    add_subdirectory(../runtime gc_runtime)
endif()
enable_testing()

get_filename_component(GC_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(GC_HOMEBREW_DIR ${GC_REPO_ROOT}/build/homebrew/gc CACHE PATH "Built GameCube homebrew (build_homebrew.sh gc)")
set(GC_RUN_HELLO ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh)

add_executable(gc_fnma gc_fnma.c)
target_include_directories(gc_fnma PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)
target_compile_options(gc_fnma PRIVATE -Wall -Wextra)
if(NOT APPLE)
    target_link_libraries(gc_fnma PRIVATE m)
endif()
add_test(NAME gc_fnma COMMAND gc_fnma)

if(TARGET gc_run)
    add_test(NAME gc_hello
        COMMAND sh ${GC_RUN_HELLO} $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/hello.dol
                ${CMAKE_CURRENT_BINARY_DIR}/hello_interp interp ${CMAKE_CURRENT_SOURCE_DIR}/hello_xfb.png)
    add_test(NAME gc_rc_hello
        COMMAND sh ${GC_RUN_HELLO} $<TARGET_FILE:gc_run> ${GC_HOMEBREW_DIR}/hello.dol
                ${CMAKE_CURRENT_BINARY_DIR}/hello_rc rc ${CMAKE_CURRENT_SOURCE_DIR}/hello_xfb.png)
    set_tests_properties(gc_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    set_tests_properties(gc_rc_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
