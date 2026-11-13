cmake_minimum_required(VERSION 3.20)
project(apu_downmix_test C)
# XBA-122: the mixbins folded to stereo by role (RECOMP_APU_MIXDOWN), the
# native I3DL2 reverb (RECOMP_APU_REVERB): coefficients, WAV dumps read back,
# decay times, and the host cost of one GP frame. Synthetic signals only.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_downmix_test test_main.c)
target_include_directories(apu_downmix_test PRIVATE ../../src ../../src/apu)
target_link_libraries(apu_downmix_test PRIVATE xboxrecomp)
if(NOT WIN32)
    target_link_libraries(apu_downmix_test PRIVATE m)
endif()
enable_testing()
add_test(NAME apu_downmix COMMAND apu_downmix_test)
