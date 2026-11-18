# console_pack: what the consoles' title packs share (console_pack.h: pack.json's checks, the library's loading;
# recomp_sha256.h: the hash). libc + dlopen only, no SDL and no console source: the consoles' cores and pack libraries
# link it, including the ones built without the host layer (GBA_CORE_ONLY..., gc_core), so it stays out of
# host_core.cmake, whose other targets those builds do not need. Included by each console's runtime/CMakeLists.txt.
if(NOT TARGET console_pack)
    add_library(console_pack STATIC ${CMAKE_CURRENT_LIST_DIR}/console_pack.c ${CMAKE_CURRENT_LIST_DIR}/recomp_sha256.c)
    target_include_directories(console_pack PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(console_pack PUBLIC ${CMAKE_DL_LIBS})
    target_compile_options(console_pack PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(console_pack PRIVATE -O2) # the hash of a pack's library runs at each start
    endif()
    if(APPLE)
        target_compile_definitions(console_pack PRIVATE _DARWIN_C_SOURCE)
    endif()
    if(WIN32 AND TARGET xbr_posix_win32)
        target_link_libraries(console_pack PUBLIC xbr_posix_win32) # dlopen, access... (src/platform/win32_posix)
    endif()
    set_target_properties(console_pack PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
endif()
