cmake_minimum_required(VERSION 3.20)
project(host_layer_test C)
# The host layer alone (XBA-145, src/platform/HOST_LAYER.md): a program that links only xbr_host (src/host) opens a
# window, presents a GL texture and reads the pads. The project adds src/host and nothing else, so its build
# compiles no Xbox source; the configure step also walks xbr_host's link closure and fails on an Xbox target or
# source (below).
#   host_layer_core    no display: time, lifecycle, config, storage, pads and audio out on SDL's dummy drivers
#   host_layer_window  the window, GL present and pads; skipped (77) headless or under XBR_BUILD_ONLY
#   host_layer_shaders the picture shaders (XBA-195) drawn and read back, their cost; skipped the same way
if(WIN32)
    message(FATAL_ERROR "The host layer is POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)

add_subdirectory(../../src/host host)

# Every target xbr_host reaches, and its sources: none may be Xbox-specific.
function(_xbr_host_closure target)
    if(NOT TARGET ${target} OR target IN_LIST _xbr_seen)
        return()
    endif()
    list(APPEND _xbr_seen ${target})
    set(_xbr_seen ${_xbr_seen} PARENT_SCOPE)
    get_target_property(_aliased ${target} ALIASED_TARGET)
    get_target_property(_imported ${target} IMPORTED)
    if(_imported OR _aliased)
        return()   # SDL2::SDL2, Threads::Threads
    endif()
    if(target MATCHES "^(platform|xbox_|recomp_app$|recomp_pack$)")
        message(FATAL_ERROR "xbr_host links the Xbox target ${target}")
    endif()
    get_target_property(_srcs ${target} SOURCES)
    foreach(_s IN LISTS _srcs)
        if(_s MATCHES "(win32_compat|xbox_|kernel|nv2a|mmio_decode|xbe|apu|dsound|xinput_device|fb_present)")
            message(FATAL_ERROR "xbr_host builds the Xbox source ${_s} (target ${target})")
        endif()
    endforeach()
    get_target_property(_libs ${target} LINK_LIBRARIES)
    get_target_property(_ilibs ${target} INTERFACE_LINK_LIBRARIES)
    foreach(_l IN LISTS _libs _ilibs)
        if(_l AND TARGET ${_l})
            _xbr_host_closure(${_l})
            set(_xbr_seen ${_xbr_seen} PARENT_SCOPE)
        endif()
    endforeach()
endfunction()
set(_xbr_seen "")
_xbr_host_closure(xbr_host)
message(STATUS "xbr_host closure: ${_xbr_seen}")

add_executable(host_layer_test test_main.c)
target_link_libraries(host_layer_test PRIVATE xbr_host)

enable_testing()
add_test(NAME host_layer_core COMMAND host_layer_test core)
set_tests_properties(host_layer_core PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy" TIMEOUT 60)
add_test(NAME host_layer_window COMMAND host_layer_test window)
set_tests_properties(host_layer_window PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
# XBA-195: the picture shaders, each drawn and read back (host_window_capture); skipped (77) like the window test
add_test(NAME host_layer_shaders COMMAND host_layer_test shaders)
set_tests_properties(host_layer_shaders PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
# XBA-203: the shader module shared with the Xbox presenter: keys and sources everywhere, each preset built in every
# GL context the display offers (none: those builds are skipped, the rest still runs)
add_test(NAME host_layer_present_shaders COMMAND host_layer_test present_shaders)
set_tests_properties(host_layer_present_shaders PROPERTIES TIMEOUT 120)
# GC-003: frame generation's read-back of a GPU picture through pack buffers. host_window.c is compiled into the test
# with frame generation on (xbr_host's own copy, without it, is not linked); skipped (77) like the window test
add_executable(host_layer_fg_readback test_fg_readback.c)
target_link_libraries(host_layer_fg_readback PRIVATE xbr_host)
add_test(NAME host_layer_fg_readback COMMAND host_layer_fg_readback)
set_tests_properties(host_layer_fg_readback PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 ENVIRONMENT "SDL_AUDIODRIVER=dummy")
# XBA-243: the performance overlay's text fed by each console's performance line, no display
add_test(NAME host_layer_overlay COMMAND host_layer_test overlay)
set_tests_properties(host_layer_overlay PROPERTIES TIMEOUT 60)
# host_window_screenshot_bmp of a texture present (the X360 runtime, the GameCube's GX and GPU-YUV paths): grabbed by
# the presenting thread at its next present or pump, sampled as the window samples it; skipped (77) like the window test
add_executable(host_layer_grab test_grab.c)
target_link_libraries(host_layer_grab PRIVATE xbr_host)
add_test(NAME host_layer_grab COMMAND host_layer_grab)
set_tests_properties(host_layer_grab PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
