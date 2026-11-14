# host_framegen: the frame generation pacer and its fake generator (framegen_pacer.h, framegen_policy.h), the console
# window's distinct frames and timed FIFO (framegen_console.h), C and
# pthreads only, no Vulkan and no Xbox source: part of the host layer (src/platform/HOST_LAYER.md, XBA-145).
# Included by src/framegen/CMakeLists.txt (xbox_framegen links it) and by src/host/CMakeLists.txt (xbr_host); the
# guard keeps one definition. POSIX hosts.
if(NOT TARGET host_framegen)
    get_filename_component(_xbr_fg_src "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    add_library(host_framegen STATIC
        ${CMAKE_CURRENT_LIST_DIR}/framegen_pacer.c
        ${CMAKE_CURRENT_LIST_DIR}/framegen_fake.c
        ${CMAKE_CURRENT_LIST_DIR}/framegen_console.c)   # the console window's front end (XBA-186)
    target_include_directories(host_framegen PUBLIC ${CMAKE_CURRENT_LIST_DIR} ${_xbr_fg_src})
    set_target_properties(host_framegen PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
                                                   POSITION_INDEPENDENT_CODE ON)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(host_framegen PUBLIC Threads::Threads)
endif()
