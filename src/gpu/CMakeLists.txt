# GPU render back end for the pushbuffer executor (XBA-27): OpenGL 3.3/4.1
# core on desktop POSIX, OpenGL ES 3.0 on Android. Selected at run time with
# RECOMP_GPU=1; the CPU rasteriser stays the default.
#
#   macOS    system OpenGL framework, headless CGL context
#   Android  GLESv3 + EGL from the NDK
#   Linux    libepoxy (GL and EGL)
add_library(xbox_gpu STATIC
    nv2a_gl.c
    nv2a_gl_context.c
    nv2a_gl_shader.c
)
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
target_include_directories(xbox_gpu PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
    ${XBOXRECOMP_SRC_DIR}/kernel
)
target_link_libraries(xbox_gpu PUBLIC xbox_kernel)
if(APPLE)
    target_link_libraries(xbox_gpu PUBLIC "-framework OpenGL")
elseif(ANDROID)
    target_link_libraries(xbox_gpu PUBLIC GLESv3 EGL)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XBOX_GPU_EPOXY REQUIRED epoxy)
    target_include_directories(xbox_gpu PRIVATE ${XBOX_GPU_EPOXY_INCLUDE_DIRS})
    target_link_libraries(xbox_gpu PUBLIC ${XBOX_GPU_EPOXY_LINK_LIBRARIES})
endif()
if(NOT MSVC)
    target_compile_options(xbox_gpu PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
