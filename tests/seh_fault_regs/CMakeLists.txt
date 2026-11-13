cmake_minimum_required(VERSION 3.20)
project(seh_fault_regs_test C)
# Guest registers in g_cpu at a host fault on a 16-bit, 64-bit or float
# guest access (XBA-132, XBA-138): the real access macros at -O2, whatever the
# build type, since the bug is an optimisation.
set(CMAKE_C_STANDARD 11)
enable_testing()
add_executable(seh_fault_regs_test test_main.c)
target_include_directories(seh_fault_regs_test PRIVATE ../../templates/runtime ../../src)
if(NOT MSVC)
    target_compile_options(seh_fault_regs_test PRIVATE -O2)
endif()
if(NOT WIN32)
    target_link_libraries(seh_fault_regs_test PRIVATE m)
endif()
add_test(NAME seh_fault_regs COMMAND seh_fault_regs_test)
set_tests_properties(seh_fault_regs PROPERTIES SKIP_RETURN_CODE 77)
