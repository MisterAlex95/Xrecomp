cmake_minimum_required(VERSION 3.20)
project(aspect_policy_test C)
# The presenter's aspect policy (src/video/aspect_policy.c): pure C, every
# host, no SDL.
set(CMAKE_C_STANDARD 11)
add_executable(aspect_policy_test test_main.c ../../src/video/aspect_policy.c)
target_include_directories(aspect_policy_test PRIVATE ../../src/video)
enable_testing()
add_test(NAME aspect_policy COMMAND aspect_policy_test)
