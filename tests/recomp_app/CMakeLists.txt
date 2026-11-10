cmake_minimum_required(VERSION 3.20)
project(recomp_app_test C)
set(CMAKE_C_STANDARD 11)
# recomp_app.c (XBA-52) and the platform pieces it calls; its two kernel calls and the logcat redirect are stubs in
# the test (on a device the redirect would send the test's own output to logcat), so no runtime to build.
if(WIN32)
    message(FATAL_ERROR "recomp_app is for POSIX hosts (the Android app shell)")
endif()
find_package(Threads REQUIRED)
add_executable(recomp_app_test test_main.c
    ../../src/platform/recomp_app.c ../../src/platform/recomp_cfg.c ../../src/platform/recomp_storage.c
    ../../src/platform/host_cpus.c)
target_include_directories(recomp_app_test PRIVATE ../../src/platform)
target_link_libraries(recomp_app_test PRIVATE Threads::Threads)
if(ANDROID)
    target_link_libraries(recomp_app_test PRIVATE log)   # recomp_app.c reports a failed redirect to logcat
endif()
enable_testing()
add_test(NAME recomp_app COMMAND recomp_app_test)
