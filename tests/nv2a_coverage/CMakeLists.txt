cmake_minimum_required(VERSION 3.20)
project(nv2a_coverage_regression C)
enable_testing()
# NV2A coverage (XBA-120): the real executor over a fake guest memory, on the
# CPU rasteriser and with fake back ends. No GL here; tests/nv2a_gl_coverage
# runs the same batches through the GL back end.
set(EXEC_SOURCES
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/guest_clock.c
    ../../src/kernel/nv2a_coverage.c
    ../../src/kernel/recomp_status.c
    ../../src/kernel/recomp_discovery.c
    ../../src/platform/host_cpus.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c)
add_executable(nv2a-coverage-test nv2a_coverage_test.c ${EXEC_SOURCES})
target_include_directories(nv2a-coverage-test PRIVATE ../../src/kernel ../../src)
if(MSVC)
    target_compile_definitions(nv2a-coverage-test PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(nv2a-coverage-test PRIVATE m Threads::Threads)
endif()
add_test(NAME nv2a-coverage COMMAND nv2a-coverage-test)
