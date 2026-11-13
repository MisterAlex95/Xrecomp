cmake_minimum_required(VERSION 3.20)
project(nv2a_gl_backend_regression C)
enable_testing()
# The GL back end (src/gpu/nv2a_gl.c, included by the test source) against a
# fake guest memory: desktop GL 3.3 core and, on Linux, the GLES 3.0 path the
# Android build runs (NV2A_GL_FORCE_ES, through libepoxy). Exit 77 without a
# GL context; on Linux EGL_PLATFORM=surfaceless gives Mesa (llvmpipe) one in
# a container.
set(GPU_SOURCES
    ../../src/gpu/nv2a_gl_context.c
    ../../src/gpu/nv2a_gl_scale.c
    ../../src/gpu/nv2a_gl_shader.c
    ../../src/gpu/nv2a_gl_vsh.c
    ../../src/kernel/nv2a_combiner.c)
set(VARIANTS nv2a-gl-backend)
if(NOT APPLE AND NOT ANDROID)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    list(APPEND VARIANTS nv2a-gl-backend-es)
endif()
foreach(v ${VARIANTS})
    add_executable(${v}-test nv2a_gl_backend_test.c ${GPU_SOURCES})
    target_include_directories(${v}-test PRIVATE ../../src/gpu ../../src/kernel ../../src)
    if(APPLE)
        target_link_libraries(${v}-test PRIVATE "-framework OpenGL" m)
    elseif(ANDROID)
        target_link_libraries(${v}-test PRIVATE GLESv3 EGL m)
    else()
        target_include_directories(${v}-test PRIVATE ${EPOXY_INCLUDE_DIRS})
        target_link_libraries(${v}-test PRIVATE ${EPOXY_LINK_LIBRARIES} m)
    endif()
    set(env "EGL_PLATFORM=surfaceless")
    if(v STREQUAL "nv2a-gl-backend-es")
        target_compile_definitions(${v}-test PRIVATE NV2A_GL_FORCE_ES=1)
        # Mesa held to GLES 3.0, the version the Android build asks for.
        set(env "${env};MESA_GLES_VERSION_OVERRIDE=3.0")
    endif()
    add_test(NAME ${v} COMMAND ${v}-test)
    # Without the merge the same cases must come out wrong: the test tells
    # the two apart.
    add_test(NAME ${v}-cpu-writes-off COMMAND ${v}-test)
    set_tests_properties(${v} PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "${env}")
    set_tests_properties(${v}-cpu-writes-off PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "${env};RECOMP_GPU_CPU_WRITES=0")
endforeach()
