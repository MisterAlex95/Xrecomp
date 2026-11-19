cmake_minimum_required(VERSION 3.20)
project(nv2a_gl_upscale_regression C)
enable_testing()
# Internal resolution on the GL back end (XBA-237): 1x / 2x / 4x of one scene
# compared, read-backs at 1x. The real executor (linked) and
# the GL back end (src/gpu/nv2a_gl.c, included by the test source) over a
# fake guest memory, for desktop GL 3.3 core and, on Linux, the GLES 3.0
# path Android runs (NV2A_GL_FORCE_ES, through libepoxy). Exit 77 without a
# GL context; EGL_PLATFORM=surfaceless gives Mesa (llvmpipe) one in a
# container.
set(SOURCES
    ../../src/gpu/nv2a_gl_context.c
    ../../src/gpu/nv2a_gl_scale.c
    ../../src/gpu/nv2a_gl_shader.c
    ../../src/gpu/nv2a_gl_vsh.c
    ../../src/platform/host_shader_progress.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_coverage.c
    ../../src/kernel/recomp_status.c
    ../../src/kernel/recomp_discovery.c
    ../../src/platform/host_cpus.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c
    ../../src/kernel/guest_clock.c)
set(VARIANTS nv2a-gl-upscale)
if(NOT APPLE AND NOT ANDROID)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    list(APPEND VARIANTS nv2a-gl-upscale-es)
endif()
find_package(Threads REQUIRED)
foreach(v ${VARIANTS})
    add_executable(${v}-test nv2a_gl_upscale_test.c ${SOURCES})
    target_include_directories(${v}-test PRIVATE ../../src/gpu ../../src/kernel ../../src ../../src/platform)
    target_link_libraries(${v}-test PRIVATE Threads::Threads)
    if(APPLE)
        target_link_libraries(${v}-test PRIVATE "-framework OpenGL" m)
    elseif(ANDROID)
        target_link_libraries(${v}-test PRIVATE GLESv3 EGL m)
    else()
        target_include_directories(${v}-test PRIVATE ${EPOXY_INCLUDE_DIRS})
        target_link_libraries(${v}-test PRIVATE ${EPOXY_LINK_LIBRARIES} m)
    endif()
    set(env "EGL_PLATFORM=surfaceless")
    if(v STREQUAL "nv2a-gl-upscale-es")
        target_compile_definitions(${v}-test PRIVATE NV2A_GL_FORCE_ES=1)
        # Mesa held to GLES 3.0, the version the Android build asks for.
        set(env "${env};MESA_GLES_VERSION_OVERRIDE=3.0")
    endif()
    add_test(NAME ${v} COMMAND ${v}-test)
    set_tests_properties(${v} PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "${env}")
endforeach()
