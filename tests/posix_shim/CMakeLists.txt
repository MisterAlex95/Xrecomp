cmake_minimum_required(VERSION 3.20)
project(posix_shim_test C)
# The POSIX calls of the console runtimes on Windows (src/platform/win32_posix/xbr_posix_win32.h, force-included there
# as in src/platform/host_core.cmake). Self-contained, libc only. On POSIX hosts the same checks run against libc and
# pass; the build that matters is MinGW-w64, run under wine or on a Windows PC:
#   cmake -S tests/posix_shim -B build-shim -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#   cmake --build build-shim && wine build-shim/posix_shim_rename.exe
set(CMAKE_C_STANDARD 11)

function(posix_shim_test name)
    add_executable(${name} ${name}.c)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    if(WIN32)
        target_compile_options(${name} PRIVATE
            "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform/win32_posix/xbr_posix_win32.h")
        target_link_options(${name} PRIVATE -static)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

enable_testing()
posix_shim_test(posix_shim_rename)
