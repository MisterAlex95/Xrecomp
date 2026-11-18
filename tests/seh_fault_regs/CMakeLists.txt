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
# The same cases with the access macros the generated code uses (XBA-38:
# guest memory types that do not alias g_cpu, RECOMP_GUEST_MEM_NOALIAS).
add_executable(seh_fault_regs_noalias_test test_main.c)
target_include_directories(seh_fault_regs_noalias_test PRIVATE ../../templates/runtime ../../src)
target_compile_definitions(seh_fault_regs_noalias_test PRIVATE RECOMP_GUEST_MEM_NOALIAS=1)
if(NOT MSVC)
    target_compile_options(seh_fault_regs_noalias_test PRIVATE -O2)
endif()
if(NOT WIN32)
    target_link_libraries(seh_fault_regs_noalias_test PRIVATE m)
endif()
add_test(NAME seh_fault_regs_noalias COMMAND seh_fault_regs_noalias_test)
set_tests_properties(seh_fault_regs_noalias PROPERTIES SKIP_RETURN_CODE 77)
# The same cases with g_cpu reached through bionic's TLS_SLOT_APP, as Android's generated code does (XBA-273,
# RECOMP_CPU_SLOT; arm64 only, the slot emulated on glibc: see test_main.c), plus the slot's own cases.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(seh_fault_regs_slot_test test_main.c)
    target_include_directories(seh_fault_regs_slot_test PRIVATE ../../templates/runtime ../../src)
    target_compile_definitions(seh_fault_regs_slot_test PRIVATE RECOMP_GUEST_MEM_NOALIAS=1 RECOMP_CPU_SLOT=1
                               SEH_TEST_CPU_SLOT=1)
    target_compile_options(seh_fault_regs_slot_test PRIVATE -O2)
    find_package(Threads REQUIRED)
    target_link_libraries(seh_fault_regs_slot_test PRIVATE m Threads::Threads)
    add_test(NAME seh_fault_regs_slot COMMAND seh_fault_regs_slot_test)
    set_tests_properties(seh_fault_regs_slot PROPERTIES SKIP_RETURN_CODE 77)
endif()
