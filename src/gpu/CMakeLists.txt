# GPU render back end for the pushbuffer executor (XBA-27): OpenGL 3.3/4.1
# core on desktop POSIX, OpenGL ES 3.0 on Android. Selected at run time by
# RECOMP_GPU (nv2a_gl_install_from_env): on by default on Android, where
# RECOMP_GPU=0 picks the CPU rasteriser; elsewhere RECOMP_GPU=1 turns it on.
#
#   macOS    system OpenGL framework, headless CGL context
#   Android  GLESv3 + EGL from the NDK
#   Linux    libepoxy (GL and EGL)
# Linux needs libepoxy through pkg-config. Without it (a consumer that turned
# pkg-config off, as tests/cmake_d3d8_consumer does, or no libepoxy) the back
# end is left out: the CPU rasteriser stays, RECOMP_GPU=1 has no effect.
if(NOT APPLE AND NOT ANDROID)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(XBOX_GPU_EPOXY QUIET epoxy)
    endif()
    if(NOT XBOX_GPU_EPOXY_FOUND)
        message(STATUS "xbox_gpu: libepoxy not found through pkg-config, GL render back end left out")
        return()
    endif()
endif()

add_library(xbox_gpu STATIC
    nv2a_gl.c
    nv2a_gl_context.c
    nv2a_gl_scale.c
    nv2a_gl_shader.c
    nv2a_gl_vsh.c
)
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
target_include_directories(xbox_gpu PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
    ${XBOXRECOMP_SRC_DIR}/kernel
)
target_link_libraries(xbox_gpu PUBLIC xbox_kernel)
# The shader badge's counter (XBA-314): the warm-up and the programs compiled report into it.
include(${XBOXRECOMP_SRC_DIR}/platform/host_shader_progress.cmake)
# Threads: the shader worker (XBA-310), which compiles programs off the draw path in a shared context.
find_package(Threads REQUIRED)
target_link_libraries(xbox_gpu PUBLIC host_shader_progress Threads::Threads)
if(APPLE)
    target_link_libraries(xbox_gpu PUBLIC "-framework OpenGL")
elseif(ANDROID)
    target_link_libraries(xbox_gpu PUBLIC GLESv3 EGL)
else()
    target_include_directories(xbox_gpu PRIVATE ${XBOX_GPU_EPOXY_INCLUDE_DIRS})
    target_link_libraries(xbox_gpu PUBLIC ${XBOX_GPU_EPOXY_LINK_LIBRARIES})
endif()
if(NOT MSVC)
    target_compile_options(xbox_gpu PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
