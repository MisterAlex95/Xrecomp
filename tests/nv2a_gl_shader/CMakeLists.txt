cmake_minimum_required(VERSION 3.20)
project(nv2a_gl_shader_regression C)
enable_testing()
add_executable(nv2a-gl-shader-test nv2a_gl_shader_test.c
    ../../src/gpu/nv2a_gl_shader.c
    ../../src/gpu/nv2a_gl_context.c
    ../../src/kernel/nv2a_combiner.c)
target_include_directories(nv2a-gl-shader-test PRIVATE ../../src/gpu ../../src/kernel ../../src)
if(APPLE)
    target_link_libraries(nv2a-gl-shader-test PRIVATE "-framework OpenGL" m)
elseif(ANDROID)
    target_link_libraries(nv2a-gl-shader-test PRIVATE GLESv3 EGL m)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    target_include_directories(nv2a-gl-shader-test PRIVATE ${EPOXY_INCLUDE_DIRS})
    target_link_libraries(nv2a-gl-shader-test PRIVATE ${EPOXY_LINK_LIBRARIES} m)
endif()
add_test(NAME nv2a-gl-shader COMMAND nv2a-gl-shader-test)
# No GL context (a CI container): the text checks run, the GPU half is skipped.
set_tests_properties(nv2a-gl-shader PROPERTIES SKIP_RETURN_CODE 77)
