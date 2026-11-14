cmake_minimum_required(VERSION 3.20)
project(recomp_states_test C)
# Save states and fast-forward of the console runtimes, the shared part (XBA-182, src/platform/recomp_states.h): slot
# files (write, read, refusals), thumbnails, the attach / save / load path through a runtime's callbacks, the
# fast-forward switches, speed and audio. libc + pthreads only (xbr_host_core), no SDL.
if(WIN32)
    message(FATAL_ERROR "recomp_states is POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform/host_core.cmake)

add_executable(recomp_states_test test_main.c)
target_link_libraries(recomp_states_test PRIVATE xbr_host_core)
target_compile_options(recomp_states_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME recomp_states COMMAND recomp_states_test)
set_tests_properties(recomp_states PROPERTIES TIMEOUT 60)
