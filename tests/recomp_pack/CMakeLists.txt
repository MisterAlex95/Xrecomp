cmake_minimum_required(VERSION 3.20)
project(recomp_pack_test C)
set(CMAKE_C_STANDARD 11)
# A title pack, the loader's side (src/platform/recomp_pack.h, android/app/src/main/cpp/pack_loader.h):
# 1. recomp_pack.c alone (+ recomp_cfg.c, which sets env_force): the XBE's title ID, pack.json parsing and checks;
# 2. the routing, with three small shared libraries built the way the app's are:
#    - fake_shell: pack_loader.c (the weak forwarders, xba_shell_abi, xba_pack_load) + recomp_pack.c + a stand-in
#      for the toolkit that calls recomp_lookup & co. from inside the shell (as kernel_bridge.c does in libmain.so);
#    - fake_title: a pack, linked like tools/prepare's libtitle.so (-Bsymbolic --no-undefined against fake_shell),
#      exporting recomp_title_main, recomp_lookup, recomp_lookup_manual, g_fe_cursor, fe_menu_get_pb_state;
#      fake_title_min exports recomp_title_main only;
#    - fake_shell_static: fake_shell with a title linked in (strong symbols), as the stub APK is.
#    The test dlopens them by path (argv), as SDLActivity loads libmain.so, then xba_pack_load does the rest.
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform)
set(APP ${CMAKE_CURRENT_SOURCE_DIR}/../../android/app/src/main/cpp)

add_executable(recomp_pack_test test_main.c ${SRC}/recomp_pack.c ${SRC}/recomp_cfg.c)
target_include_directories(recomp_pack_test PRIVATE ${SRC})
target_link_libraries(recomp_pack_test PRIVATE ${CMAKE_DL_LIBS})

# The other consoles' runtimes pack_loader.c routes to (XBA-147). They all play games now (core + SDL host layer, GBA
# XBA-157, PS1 XBA-168, N64 XBA-174, GameCube XBA-259), which this test does not link: stand-ins that log.
set(CONSOLES ${CMAKE_CURRENT_SOURCE_DIR}/../../consoles)
set(CONSOLE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/fake_gba_runtime.c ${CMAKE_CURRENT_SOURCE_DIR}/fake_ps1_runtime.c
                ${CMAKE_CURRENT_SOURCE_DIR}/fake_n64_runtime.c ${CMAKE_CURRENT_SOURCE_DIR}/fake_gc_runtime.c)
set(CONSOLE_INC ${CONSOLES}/gba/runtime ${CONSOLES}/ps1/runtime ${CONSOLES}/n64/runtime ${CONSOLES}/gc/runtime
                ${CMAKE_CURRENT_SOURCE_DIR}/../../src/input)

foreach(t fake_shell fake_shell_static)
    add_library(${t} SHARED ${APP}/pack_loader.c ${SRC}/recomp_pack.c ${SRC}/recomp_cfg.c fake_toolkit.c ${CONSOLE_SRC})
    target_include_directories(${t} PRIVATE ${SRC} ${APP} ${CONSOLE_INC})
    target_link_libraries(${t} PRIVATE ${CMAKE_DL_LIBS})
    if(NOT APPLE)   # GNU ld's spelling; Apple's ld refuses undefined symbols by default
        target_link_options(${t} PRIVATE -Wl,--no-undefined)
    else()          # ...the weak reference pack_loader.c tests for NULL included: allow that one by name
        target_link_options(${t} PRIVATE -Wl,-U,_xbox_disc_slurp)
    endif()
endforeach()
target_sources(fake_shell_static PRIVATE title_static.c)

foreach(t fake_title fake_title_min)
    add_library(${t} SHARED ${t}.c)
    target_link_libraries(${t} PRIVATE fake_shell)
    if(NOT APPLE)   # as above; Mach-O's two-level namespace already binds a library's own symbols first
        target_link_options(${t} PRIVATE -Wl,-Bsymbolic -Wl,--no-undefined)
    endif()
endforeach()

enable_testing()
add_test(NAME recomp_pack COMMAND recomp_pack_test
         $<TARGET_FILE:fake_shell> $<TARGET_FILE:fake_title> $<TARGET_FILE:fake_title_min>
         $<TARGET_FILE:fake_shell_static>)
set_tests_properties(recomp_pack PROPERTIES SKIP_RETURN_CODE 77)
