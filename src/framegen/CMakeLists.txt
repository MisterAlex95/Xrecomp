# Frame generation (LSFG, docs/technical/framegen.md). Built only with
# -DXBOXRECOMP_LSFG=ON on POSIX hosts (root CMakeLists.txt).
#
# C part (this file, WP2): the pacer and the fake generator, libc + pthreads.
# The Vulkan bridge (lsfg_bridge.cpp, C++20) and the DLL/SPIR-V helpers are
# added to xbox_framegen by their own package; the pacer reaches the bridge
# only through the FramegenGenerator vtable in framegen_pacer.h.
add_library(xbox_framegen STATIC
    framegen_pacer.c
    framegen_fake.c
)
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
target_include_directories(xbox_framegen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(xbox_framegen PUBLIC Threads::Threads)
