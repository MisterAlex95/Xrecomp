cmake_minimum_required(VERSION 3.20)
project(stack_switch_test C)
# Guest stack switching (XBA-101): recomp_stack_switch.c against guest code in
# the shape the lifter emits. Builds the switch and the discovery writer alone
# (they need no more of the runtime). Hosts without a stack switch (Windows,
# 32-bit) exit 77.
set(CMAKE_C_STANDARD 11)
find_package(Threads REQUIRED)
add_executable(stack_switch_test
    test_main.c
    ../../src/kernel/recomp_stack_switch.c
    ../../src/kernel/recomp_discovery.c)
target_include_directories(stack_switch_test PRIVATE ../../templates/runtime ../../src/kernel ../../src)
target_link_libraries(stack_switch_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME stack_switch COMMAND stack_switch_test)
set_tests_properties(stack_switch PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
add_test(NAME stack_switch_off COMMAND stack_switch_test off)
set_tests_properties(stack_switch_off PROPERTIES TIMEOUT 60 ENVIRONMENT "RECOMP_STACK_SWITCH=0")
add_test(NAME stack_switch_first64 COMMAND stack_switch_test)
set_tests_properties(stack_switch_first64 PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 ENVIRONMENT "RECOMP_STACK_SWITCH_WHOLE=0")
