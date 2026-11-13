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

foreach(t fake_shell fake_shell_static)
    add_library(${t} SHARED ${APP}/pack_loader.c ${SRC}/recomp_pack.c ${SRC}/recomp_cfg.c fake_toolkit.c)
    target_include_directories(${t} PRIVATE ${SRC} ${APP})
    target_link_libraries(${t} PRIVATE ${CMAKE_DL_LIBS})
    if(NOT APPLE)   # GNU ld's spelling; Apple's ld refuses undefined symbols by default
        target_link_options(${t} PRIVATE -Wl,--no-undefined)
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
