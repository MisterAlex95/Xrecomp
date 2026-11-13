# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
    aspect_policy.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# xbox_input: the windows record keys in its key table (keyboard.c).
# xbox_nv2a: the window scans out the PVIDEO overlay (nv2a_mmio_hook.c).
target_link_libraries(xbox_video PUBLIC xbox_input xbox_nv2a)
if(TARGET xbox_d3d8)
    target_link_libraries(xbox_video PUBLIC xbox_d3d8)
endif()
if(WIN32)
    # gpu_present.c: the framebuffer window's D3D11 presenter.
    target_sources(xbox_video PRIVATE gpu_present.c)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32
                          d3d11 dxgi dxguid d3dcompiler)
else()
    # The framebuffer window on POSIX hosts: SDL2, on the process main thread
    # (fb_present_sdl.c), which then also pumps the events the game
    # controllers are read from.
    if(NOT TARGET SDL2::SDL2)
        find_package(SDL2 REQUIRED)
    endif()
    # pvideo_posix.c: the PVIDEO overlay's scan-out into this window.
    target_sources(xbox_video PRIVATE fb_present_sdl.c perf_overlay.c pvideo_posix.c)
    target_link_libraries(xbox_video PUBLIC SDL2::SDL2)
    if(ANDROID)
        # XBA-136: the display's refresh grid from AChoreographer (libandroid).
        target_sources(xbox_video PRIVATE display_vsync_android.c)
        target_link_libraries(xbox_video PUBLIC android ${CMAKE_DL_LIBS})
    endif()
    # RECOMP_GPU=1: the presenter registers the GL back end (src/gpu).
    if(TARGET xbox_gpu)
        target_link_libraries(xbox_video PUBLIC xbox_gpu)
        target_compile_definitions(xbox_video PRIVATE XBOXRECOMP_HAVE_GPU=1)
    endif()
endif()
