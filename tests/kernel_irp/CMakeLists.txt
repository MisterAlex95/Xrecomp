cmake_minimum_required(VERSION 3.20)
project(kernel_irp_test C)
# IRPs in guest memory (XBA-138): IoAllocateIrp / IoInitializeIrp /
# IoBuildAsynchronousFsdRequest / IofCallDriver / IofCompleteRequest against a
# title-made driver, and NtFsControlFile(FSCTL_DISMOUNT_VOLUME), through the
# thunk dispatcher on a real guest window (the IRPs come from the guest heap).
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_irp_test test_main.c)
target_include_directories(kernel_irp_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_irp_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_irp_test
         COMMAND kernel_irp_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
add_test(NAME kernel_irp_test_off
         COMMAND kernel_irp_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe off)
set_tests_properties(kernel_irp_test_off PROPERTIES ENVIRONMENT "RECOMP_IRP_DISPATCH=0")
