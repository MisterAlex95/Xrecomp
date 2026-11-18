cmake_minimum_required(VERSION 3.20)
project(console_x360_tests C)

# The Xbox 360 runtime's tests (XBA-270): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_x360) and by the root build's ctest when XBR_CONSOLE_X360 is ON. Linux only.
#
#   x360_hello       the synthetic test title (make_hello.py: our own PowerPC assembly in a synthetic XEX2, imports
#                    by their real ordinals) recompiled and run under x360_run: its DbgPrint text must equal
#                    hello.txt (memory, critical section, TLS, a second thread and an event, a delay, a semaphore,
#                    a jump table) and it must end with exit code 0
#   x360_hello_lzx   the same title encrypted (a made-up key) and LZX-compressed, rewritten plain by
#                    python3 -m tools.xex --plain first
#   x360_hello_c     a C test title (tests/homebrew/hello_c: devkitPPC, -mpowerpc64 -maltivec; recursion, a GCC jump
#                    table, function pointers, doubles, AltiVec, two threads with a critical section and events,
#                    virtual / physical memory) packed by homebrew/elf2xex.py: hello_c.txt. Skipped (77) when not
#                    built (the linux CI job has no devkitPPC): tools/consoles/build_homebrew.sh x360 build/homebrew
# The recompiler's own tests (synthetic code) run with the Python suite: python3 -m pytest consoles/.
if(WIN32 OR APPLE)
    message(FATAL_ERROR "The Xbox 360 runtime is Linux-only for now")
endif()
set(CMAKE_C_STANDARD 11)

if(NOT TARGET x360_core)
    add_subdirectory(../runtime x360_runtime)
endif()
enable_testing()

if(TARGET x360_run)
    foreach(_mode basic lzx)
        if(_mode STREQUAL basic)
            set(_name x360_hello)
        else()
            set(_name x360_hello_${_mode})
        endif()
        add_test(NAME ${_name}
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh $<TARGET_FILE:x360_run>
                    ${CMAKE_CURRENT_BINARY_DIR}/${_name} ${_mode} ${CMAKE_CURRENT_SOURCE_DIR}/hello.txt)
        set_tests_properties(${_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    endforeach()
    get_filename_component(X360_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    set(X360_HOMEBREW_DIR ${X360_REPO_ROOT}/build/homebrew/x360 CACHE PATH
        "Built Xbox 360 test titles (build_homebrew.sh x360)")
    add_test(NAME x360_hello_c
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_hello.sh $<TARGET_FILE:x360_run>
                ${CMAKE_CURRENT_BINARY_DIR}/x360_hello_c c ${CMAKE_CURRENT_SOURCE_DIR}/hello_c.txt
                ${X360_HOMEBREW_DIR}/hello_c.elf)
    set_tests_properties(x360_hello_c PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()
