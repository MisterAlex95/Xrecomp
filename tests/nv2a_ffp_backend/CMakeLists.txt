cmake_minimum_required(VERSION 3.20)
project(nv2a_ffp_backend_regression C)
enable_testing()
set(NV2A_BACKEND_TEST_SOURCES
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_coverage.c
    ../../src/kernel/recomp_status.c
    ../../src/kernel/recomp_discovery.c
    ../../src/platform/host_cpus.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c)
add_executable(nv2a-ffp-backend-test nv2a_ffp_backend_test.c ${NV2A_BACKEND_TEST_SOURCES})
add_executable(nv2a-present-sync-test nv2a_present_sync_test.c ${NV2A_BACKEND_TEST_SOURCES})
add_executable(nv2a-zpass-test nv2a_zpass_test.c ${NV2A_BACKEND_TEST_SOURCES})
add_executable(nv2a-flip-cap-test nv2a_flip_cap_test.c ${NV2A_BACKEND_TEST_SOURCES})
add_executable(nv2a-pacing-test nv2a_pacing_test.c ${NV2A_BACKEND_TEST_SOURCES})
add_executable(nv2a-ffp-inline-test nv2a_ffp_inline_test.c ${NV2A_BACKEND_TEST_SOURCES})
add_executable(nv2a-ffp-batch-test nv2a_ffp_batch_test.c ${NV2A_BACKEND_TEST_SOURCES})
add_executable(nv2a-clear-rect-test nv2a_clear_rect_test.c ${NV2A_BACKEND_TEST_SOURCES})
foreach(target nv2a-ffp-backend-test nv2a-present-sync-test nv2a-zpass-test nv2a-flip-cap-test nv2a-pacing-test
               nv2a-ffp-inline-test nv2a-ffp-batch-test nv2a-clear-rect-test)
    target_include_directories(${target} PRIVATE ../../src/kernel ../../src)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    elseif(NOT WIN32)
        find_package(Threads REQUIRED)   # bionic has no libpthread: Threads::Threads, not -lpthread
        target_link_libraries(${target} PRIVATE m Threads::Threads)
    endif()
endforeach()
add_test(NAME nv2a-ffp-backend COMMAND nv2a-ffp-backend-test)
add_test(NAME nv2a-present-sync COMMAND nv2a-present-sync-test
         "${CMAKE_CURRENT_BINARY_DIR}/nv2a-present-sync-")
set_tests_properties(nv2a-present-sync PROPERTIES ENVIRONMENT
    "RECOMP_FB_DUMP_FLIPS=${CMAKE_CURRENT_BINARY_DIR}/nv2a-present-sync-dumping;RECOMP_FB_DUMP=${CMAKE_CURRENT_BINARY_DIR}/nv2a-present-sync-")
# Triangle strips alternate their winding only with RECOMP_STRIP_ALT_WINDING; the same test
# checks the unchanged order without it (the test above) and the swapped one with it.
add_test(NAME nv2a-ffp-backend-strip-winding COMMAND nv2a-ffp-backend-test)
set_tests_properties(nv2a-ffp-backend-strip-winding PROPERTIES ENVIRONMENT "RECOMP_STRIP_ALT_WINDING=1")
# Deferred visibility reports with a fake back end; RECOMP_GPU_ZPASS=lazy and =always.
add_test(NAME nv2a-zpass COMMAND nv2a-zpass-test)
add_test(NAME nv2a-zpass-lazy COMMAND nv2a-zpass-test)
set_tests_properties(nv2a-zpass-lazy PROPERTIES ENVIRONMENT "RECOMP_GPU_ZPASS=lazy")
add_test(NAME nv2a-zpass-always COMMAND nv2a-zpass-test)
set_tests_properties(nv2a-zpass-always PROPERTIES ENVIRONMENT "RECOMP_GPU_ZPASS=always")
# RECOMP_GPU_FPS_CAP on a fake clock: fast frames held, slow frames not.
add_test(NAME nv2a-flip-cap COMMAND nv2a-flip-cap-test)
# RECOMP_PACING=auto (XBA-107, XBA-135): synthetic titles of 10, 20, 40 ms, a flip-locked one,
# a fixed cap that wins, PAL 50 Hz vblanks, the CPU rasteriser; one case per process.
foreach(case fast slow slower locked capwins pal cpu)
    add_test(NAME nv2a-pacing-${case} COMMAND nv2a-pacing-test ${case})
endforeach()
# Inline (Begin/End) fixed-function batches: transformed (default), the old screen-space
# guess (RECOMP_FFP_INLINE=0), and only what the guess would skip (=2).
add_test(NAME nv2a-ffp-inline COMMAND nv2a-ffp-inline-test)
add_test(NAME nv2a-ffp-inline-old COMMAND nv2a-ffp-inline-test)
set_tests_properties(nv2a-ffp-inline-old PROPERTIES ENVIRONMENT "RECOMP_FFP_INLINE=0")
add_test(NAME nv2a-ffp-inline-guess COMMAND nv2a-ffp-inline-test)
set_tests_properties(nv2a-ffp-inline-guess PROPERTIES ENVIRONMENT "RECOMP_FFP_INLINE=2")
# Fixed-function batches resolved once per batch (deduplicated, on the pool) against the
# per-vertex path: the same vertices, byte for byte.
add_test(NAME nv2a-ffp-batch COMMAND nv2a-ffp-batch-test)
# CLEAR_SURFACE hands a back end SET_CLEAR_RECT on a swizzled surface, the clip on a linear one.
add_test(NAME nv2a-clear-rect COMMAND nv2a-clear-rect-test)
