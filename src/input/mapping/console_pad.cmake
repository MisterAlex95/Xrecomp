# console_pad: a host pad to a console's own pad, the player's mapping (console_pad.h, XBA-158 / XBA-168 / XBA-174).
# Pure logic, libc only: part of the host layer (src/platform/HOST_LAYER.md), linked by xbr_host (src/host), which
# adds console_pad_read over host_pad. Included by src/host/CMakeLists.txt and tests/console_pad; the guard keeps one
# definition.
if(NOT TARGET console_pad)
    add_library(console_pad STATIC ${CMAKE_CURRENT_LIST_DIR}/../console_pad.c)
    target_include_directories(console_pad PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)
    target_compile_features(console_pad PRIVATE c_std_11)
    set_target_properties(console_pad PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(console_pad PRIVATE /W4)
    else()
        target_compile_options(console_pad PRIVATE -Wall -Wextra)
    endif()
endif()
