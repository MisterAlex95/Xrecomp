# The console pad mapping (console_pad.h, XBA-158 / XBA-168 / XBA-174): each console's defaults equal the fixed
# mappings the GBA, PS1 and N64 runtimes had, the text form (parse, format, round trip, rejects), sources from a host
# pad state (thresholds), sticks, and the touch overlay merged through the defaults. Pure C, no SDL.
cmake_minimum_required(VERSION 3.20)
project(console_pad_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

include(../../src/input/mapping/console_pad.cmake)
add_executable(console_pad_test test_main.c)
target_link_libraries(console_pad_test PRIVATE console_pad)

add_test(NAME console_pad COMMAND console_pad_test)
set_tests_properties(console_pad PROPERTIES TIMEOUT 60)
