cmake_minimum_required(VERSION 3.20)
project(park_reenter_test C)
# Xbox save states in a run (XBA-294): a load whose state has guest threads at other call chains than now makes
# them leave their host frames and re-enter the state's chains through the resume entries (XBA-250), on the same
# host threads: game-shaped workers and the INLINE main thread written as the lifter emits code, a phase word in
# guest memory sending them down other chains between the capture and the load; a chain that cannot be re-entered
# (through __try) refuses with nothing changed.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX host")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
find_package(Threads REQUIRED)
add_executable(park_reenter_test test_main.c)
target_include_directories(park_reenter_test PRIVATE ../../src/kernel)
target_link_libraries(park_reenter_test PRIVATE xboxrecomp Threads::Threads)
enable_testing()
add_test(NAME park_reenter_test
         COMMAND park_reenter_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
add_test(NAME park_reenter_guest_lock
         COMMAND park_reenter_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
set_tests_properties(park_reenter_guest_lock PROPERTIES ENVIRONMENT "RECOMP_GUEST_LOCK=1;RECOMP_DISPATCH_GATE=1")
set_tests_properties(park_reenter_test park_reenter_guest_lock PROPERTIES TIMEOUT 120)
