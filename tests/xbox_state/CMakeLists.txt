cmake_minimum_required(VERSION 3.20)
project(xbox_state_test C)
# Xbox save states, part 1 (XBA-245): with game-shaped guest workers frozen at
# their kernel calls (the park detector, XBA-244) against a real guest window
# (xbox_MemoryLayoutInit on the synthetic XBE), capture guest RAM, the threads'
# registers and the memory manager into an XBXS blob, write it as a slot file
# and read it back section by section; corrupt, foreign and other-build blobs
# are refused. XBA-249: the same-run load (and its refusals), rewind's ring and
# the recomp_states front end. Prints the capture and load times and the blob size.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX host")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(xbox_state_test test_main.c)
target_include_directories(xbox_state_test PRIVATE ../../src/kernel)
target_link_libraries(xbox_state_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME xbox_state_test
         COMMAND xbox_state_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# The same with the guest CPU lock and the dispatch gate on: the capture takes
# the gate and must not deadlock against either.
add_test(NAME xbox_state_guest_lock
         COMMAND xbox_state_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
set_tests_properties(xbox_state_guest_lock PROPERTIES
                     ENVIRONMENT "RECOMP_GUEST_LOCK=1;RECOMP_DISPATCH_GATE=1")
set_tests_properties(xbox_state_test xbox_state_guest_lock PROPERTIES TIMEOUT 120)
