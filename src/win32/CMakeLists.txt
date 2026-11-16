# The Win32 runtime of a recompiled Windows PE (XBA-233, epic XBA-231: Windows PC games). A spike: console programs
# on POSIX hosts, separate from the Xbox kernel (it shares only the lifter's ABI, templates/runtime/recomp_types.h).
# tools.win32 builds a program against these sources directly; this target keeps them compiling with the toolkit.
add_library(xbr_win32 STATIC win32_runtime.c win32_hle.c)
target_include_directories(xbr_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                     PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../templates/runtime)
target_compile_definitions(xbr_win32 PRIVATE _DEFAULT_SOURCE)
if(NOT MSVC)
    target_compile_options(xbr_win32 PRIVATE -Wall -Wextra)
endif()
