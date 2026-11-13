cmake_minimum_required(VERSION 3.20)
project(kernel_objects_test C)
# The object namespace and objects with a guest body (XBA-114), through the
# thunk dispatcher against a real guest window (xbox_MemoryLayoutInit on the
# synthetic XBE), since ObCreateObject allocates from the guest pool.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_objects_test test_main.c)
target_include_directories(kernel_objects_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_objects_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_objects_test
         COMMAND kernel_objects_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
