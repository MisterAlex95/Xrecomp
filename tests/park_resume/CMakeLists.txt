cmake_minimum_required(VERSION 3.20)
project(park_resume_test C)
# Xbox save states, part 6 (XBA-250): game-shaped guest workers written as the
# lifter emits code with resume entries (RECOMP_FRAME_ENTRY_R, a resume block,
# a label at every call that may park and after every host wait) are captured
# in one process, the state written as a slot file, and the process runs itself
# again (fork + exec): the second process resumes the state before any guest
# thread exists (xbox_StateResume) and every thread continues from the same
# point, with the same registers, ebp values and guest memory.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX host")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(park_resume_test test_main.c)
target_include_directories(park_resume_test PRIVATE ../../src/kernel)
target_link_libraries(park_resume_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME park_resume_test
         COMMAND park_resume_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
add_test(NAME park_resume_guest_lock
         COMMAND park_resume_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# The test runs itself again (fork + exec): a cross build's binary needs the emulator ctest runs it under
# (qemu-aarch64 for the arm64 CI), passed comma separated as XBR_TEST_EMULATOR.
set(_park_env "")
if(CMAKE_CROSSCOMPILING_EMULATOR)
    list(JOIN CMAKE_CROSSCOMPILING_EMULATOR "," _park_emu)
    set(_park_env "XBR_TEST_EMULATOR=${_park_emu}")
endif()
set_tests_properties(park_resume_test PROPERTIES ENVIRONMENT "${_park_env}")
set_tests_properties(park_resume_guest_lock PROPERTIES
                     ENVIRONMENT "RECOMP_GUEST_LOCK=1;RECOMP_DISPATCH_GATE=1;${_park_env}")
set_tests_properties(park_resume_test park_resume_guest_lock PROPERTIES TIMEOUT 120)
