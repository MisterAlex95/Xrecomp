cmake_minimum_required(VERSION 3.20)
project(host_cpus_test C)
set(CMAKE_C_STANDARD 11)
if(WIN32)
    message(FATAL_ERROR "The fake sysfs tree is POSIX-only")
endif()
# host_cpus.c only: the fast-core mask from a fake sysfs tree, a tracked thread put back after a resume.
add_executable(host_cpus_test test_main.c ../../src/platform/host_cpus.c)
target_include_directories(host_cpus_test PRIVATE ../../src/platform)
enable_testing()
add_test(NAME host_cpus COMMAND host_cpus_test)

# host_perf_hint.c over fake ADPF entry points (a no-op without them), and a worker thread through
# recomp_host_worker_hook put back on its cores after a resume (pthread_attr_setaffinity_np: Linux).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(host_perf_hint_test test_perf_hint.c ../../src/platform/host_perf_hint.c
                   ../../src/platform/host_cpus.c)
    target_include_directories(host_perf_hint_test PRIVATE ../../src/platform)
    target_compile_options(host_perf_hint_test PRIVATE -Wall -Wextra)
    target_link_libraries(host_perf_hint_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME host_perf_hint COMMAND host_perf_hint_test)
endif()
