# host_shader_progress: the shader compile counter the window's badge shows (host_shader_progress.h, XBA-314). Plain
# C11 + atomics, no SDL and no console source, so a renderer anywhere links it: included by host_core.cmake
# (xbr_host_core links it, so every runtime and window has it) and by src/gpu (the Xbox GL back end); the guard keeps
# one definition.
if(NOT TARGET host_shader_progress)
    add_library(host_shader_progress STATIC ${CMAKE_CURRENT_LIST_DIR}/host_shader_progress.c)
    target_include_directories(host_shader_progress PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        message(FATAL_ERROR "host_shader_progress needs GCC / Clang atomics (__atomic_*)")
    endif()
    target_compile_options(host_shader_progress PRIVATE -Wall -Wextra)
    set_target_properties(host_shader_progress PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
