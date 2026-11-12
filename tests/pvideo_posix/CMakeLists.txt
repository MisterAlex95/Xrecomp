cmake_minimum_required(VERSION 3.20)
project(pvideo_posix_test C)
# The PVIDEO overlay on POSIX hosts: the register block is plain memory, the
# NV2A ack pass answers STOP and the overlay scan-out (src/video/pvideo_posix.c)
# consumes banks at vblank. Windows traps the block instead (tests/nv2a_pvideo).
if(WIN32)
    message(FATAL_ERROR "The POSIX overlay scan-out is not built on Windows")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(pvideo_posix_test test_main.c)
target_include_directories(pvideo_posix_test PRIVATE ../../src ../../src/kernel ../../src/video)
target_link_libraries(pvideo_posix_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME pvideo_posix COMMAND pvideo_posix_test)
set_tests_properties(pvideo_posix PROPERTIES TIMEOUT 60
                     ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
