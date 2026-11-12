cmake_minimum_required(VERSION 3.20)
project(nv2a_gl_vsh_regression C)
enable_testing()
# The interpreter is included by the test source (nv2a_vsh_interp.c), so its
# statics can be set directly.
add_executable(nv2a-gl-vsh-test nv2a_gl_vsh_test.c
    ../../src/gpu/nv2a_gl_vsh.c
    ../../src/gpu/nv2a_gl_shader.c
    ../../src/gpu/nv2a_gl_context.c
    ../../src/kernel/nv2a_combiner.c)
target_include_directories(nv2a-gl-vsh-test PRIVATE ../../src/gpu ../../src/kernel ../../src)
if(APPLE)
    target_link_libraries(nv2a-gl-vsh-test PRIVATE "-framework OpenGL" m)
elseif(ANDROID)
    target_link_libraries(nv2a-gl-vsh-test PRIVATE GLESv3 EGL m)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    target_include_directories(nv2a-gl-vsh-test PRIVATE ${EPOXY_INCLUDE_DIRS})
    target_link_libraries(nv2a-gl-vsh-test PRIVATE ${EPOXY_LINK_LIBRARIES} m)
endif()
add_test(NAME nv2a-gl-vsh COMMAND nv2a-gl-vsh-test)
# No GL context (a CI container): the text checks run, the GPU half is skipped.
set_tests_properties(nv2a-gl-vsh PROPERTIES SKIP_RETURN_CODE 77)

# Linux: Mesa's EGL finds no display in a container; EGL_PLATFORM=surfaceless
# gives it one (llvmpipe), so the GPU half runs in CI instead of skipping.
set_tests_properties(nv2a-gl-vsh PROPERTIES ENVIRONMENT "EGL_PLATFORM=surfaceless")
# The same checks through the GLES 3.0 path (the Android build's GLSL ES
# header, precision, GLES enums) on an EGL ES context, where the host has
# GLES through libepoxy (NV2A_GL_FORCE_ES, nv2a_gl_api.h); Mesa held to GLES
# 3.0, the version the Android build asks for (MESA_GLES_VERSION_OVERRIDE).
if(NOT APPLE AND NOT ANDROID)
    add_executable(nv2a-gl-vsh-es-test nv2a_gl_vsh_test.c
        ../../src/gpu/nv2a_gl_vsh.c
        ../../src/gpu/nv2a_gl_shader.c
        ../../src/gpu/nv2a_gl_context.c
        ../../src/kernel/nv2a_combiner.c)
    target_compile_definitions(nv2a-gl-vsh-es-test PRIVATE NV2A_GL_FORCE_ES=1)
    target_include_directories(nv2a-gl-vsh-es-test PRIVATE ../../src/gpu ../../src/kernel ../../src
                               ${EPOXY_INCLUDE_DIRS})
    target_link_libraries(nv2a-gl-vsh-es-test PRIVATE ${EPOXY_LINK_LIBRARIES} m)
    add_test(NAME nv2a-gl-vsh-es COMMAND nv2a-gl-vsh-es-test)
    set_tests_properties(nv2a-gl-vsh-es PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "EGL_PLATFORM=surfaceless;MESA_GLES_VERSION_OVERRIDE=3.0")
endif()
