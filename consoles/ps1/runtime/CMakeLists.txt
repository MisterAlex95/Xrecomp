cmake_minimum_required(VERSION 3.20)
project(ps1_runtime C)

# ps1_runtime: the PlayStation runtime (MIPS R3000A, little-endian), XBA-146. The core (P-4 XBA-162 interpreter, P-5
# XBA-163 BIOS boot, P-6 XBA-164 GTE, P-9 XBA-167 pads / memory cards) is ps1_core.h; the other chips are reached
# through ps1_devices.h. See consoles/README.md and the Plane spec « Spec · Other consoles ». Built when XBR_CONSOLE_PS1 is ON (root
# CMakeLists.txt); a project may also add this directory alone (consoles/ps1/tests does).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ps1_gpu: the GPU (P-7, XBA-165, ps1_devices.h): plain C11, no host dependency, so tests build anywhere. ps1_gpu.c
# replaces the core's ps1_gpu_stub.c when it is present.
if(NOT TARGET ps1_gpu)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ps1_gpu.c)
        set(PS1_GPU_SRC ps1_gpu.c)
    else()
        set(PS1_GPU_SRC ps1_gpu_stub.c)
    endif()
    add_library(ps1_gpu STATIC ${PS1_GPU_SRC})
    target_include_directories(ps1_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(ps1_gpu PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(ps1_gpu PRIVATE -O2) # the software renderer is far too slow unoptimised
    endif()
    set_target_properties(ps1_gpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ps1_cd (disc images ps1_disc.c, P-1 XBA-159, and the CD-ROM controller ps1_cdrom.c, P-9 XBA-167), ps1_spu (P-8,
# XBA-166; pulls the CD audio, so it links ps1_cd) and ps1_mdec (P-9, XBA-167): plain C11, no host dependency, each
# source replacing the core's ps1_<dev>_stub.c when present. XBR_PS1_CHD: .chd images through the vendored libchdr
# (third_party/libchdr: BSD-3-Clause, its deps public domain / MIT / BSD-3; see NOTICE).
option(XBR_PS1_CHD "Read .chd PlayStation disc images (vendored libchdr)" ON)
foreach(dev disc cdrom spu mdec)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/ps1_${dev}.c)
        set(PS1_${dev}_SRC ps1_${dev}.c)
    else()
        set(PS1_${dev}_SRC ps1_${dev}_stub.c)
    endif()
endforeach()
foreach(lib cd spu mdec)
    if(NOT TARGET ps1_${lib})
        if(lib STREQUAL "cd")
            add_library(ps1_cd STATIC ${PS1_disc_SRC} ${PS1_cdrom_SRC})
        else()
            add_library(ps1_${lib} STATIC ${PS1_${lib}_SRC})
        endif()
        target_include_directories(ps1_${lib} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(ps1_${lib} PRIVATE -Wall -Wextra)
        if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
            target_compile_options(ps1_${lib} PRIVATE -O2) # the SPU mixes 24 voices at 44.1 kHz
        endif()
        set_target_properties(ps1_${lib} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endforeach()
target_link_libraries(ps1_spu PUBLIC ps1_cd)
if(XBR_PS1_CHD AND PS1_disc_SRC STREQUAL "ps1_disc.c")
    if(NOT TARGET ps1_chdr)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/libchdr ${CMAKE_CURRENT_BINARY_DIR}/ps1_chdr)
    endif()
    target_link_libraries(ps1_cd PRIVATE ps1_chdr)
    target_compile_definitions(ps1_cd PRIVATE XBR_PS1_CHD=1)
endif()

# ps1_core: the emulated machine (CPU, GTE, bus, DMA, timers, SIO0, scheduler) over the device libraries above. Plain
# C11 + libm, no host dependency, so the tests and ps1_run build anywhere (no SDL).
if(NOT TARGET ps1_core)
    add_library(ps1_core STATIC ps1_core.c ps1_cpu.c ps1_gte.c ps1_bus.c ps1_io.c ps1_sio.c ps1_recomp.c)
    target_include_directories(ps1_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ps1_core PUBLIC ps1_gpu ps1_cd ps1_spu ps1_mdec)
    find_library(PS1_LIBM m)
    if(PS1_LIBM)
        target_link_libraries(ps1_core PUBLIC ${PS1_LIBM})
    endif()
    target_compile_options(ps1_core PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(ps1_core PRIVATE -O2) # the interpreter is far too slow unoptimised
    endif()
    set_target_properties(ps1_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ps1_pack: a PS1 title pack, the runtime's side (pack.json checks, the boot EXE's identity, dlopen of the table,
# the discovered code written at exit; ps1_pack.h, XBA-168). No SDL: the tests link it alone.
if(NOT TARGET ps1_pack)
    add_library(ps1_pack STATIC ps1_pack.c)
    target_include_directories(ps1_pack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ps1_pack PUBLIC ps1_core ${CMAKE_DL_LIBS})
    target_compile_options(ps1_pack PRIVATE -Wall -Wextra)
    set_target_properties(ps1_pack PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ps1_core_exported(target): the executable exports the core's symbols, which a recompiled title pack (ps1_recomp.h,
# ps1_recomp_rt.h: ps1_rc_*) binds to when it is dlopen'ed. The whole archive is linked so every symbol is there.
function(ps1_core_exported tgt)
    set_target_properties(${tgt} PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        target_link_options(${tgt} PRIVATE "LINKER:-force_load,$<TARGET_FILE:ps1_core>")
    else()
        target_link_libraries(${tgt} PRIVATE -Wl,--whole-archive ps1_core -Wl,--no-whole-archive)
    endif()
    target_link_libraries(${tgt} PRIVATE ${CMAKE_DL_LIBS})
endfunction()

# PS1_CORE_ONLY=ON skips ps1_runtime (no xbr_host / SDL2), so a host without SDL2 can still build and test the
# devices (like GBA_CORE_ONLY).
option(PS1_CORE_ONLY "Build only ps1_core, the PS1 devices and ps1_run, not ps1_runtime (no xbr_host / SDL2)" OFF)
if(NOT PS1_CORE_ONLY AND NOT TARGET xbr_host)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
endif()
# ps1_runtime: the core + the host layer (ps1_runtime_run plays a disc or an EXE: window, pads, audio, memory cards,
# pacing; XBA-168).
if(NOT PS1_CORE_ONLY AND NOT TARGET ps1_runtime)
    add_library(ps1_runtime STATIC ps1_runtime.c)
    target_include_directories(ps1_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ps1_runtime PUBLIC ps1_core ps1_pack xbr_host)
    target_compile_options(ps1_runtime PRIVATE -Wall -Wextra)
    set_target_properties(ps1_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ps1_play: plays a disc or an EXE on a desktop through ps1_runtime (XBA-168). The title key comes from the disc as
# the app reads it (recomp_pack_rom_identify): src/platform/recomp_pack.c is compiled in (libc + recomp_cfg, which
# xbr_host brings), so a project that adds this directory alone needs nothing more. It loads title packs (--pack,
# tools/prepare/ps1.py): the whole core is exported.
if(NOT ANDROID AND TARGET ps1_runtime AND NOT TARGET ps1_play)
    get_filename_component(PS1_PLATFORM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/platform ABSOLUTE)
    add_executable(ps1_play tools/ps1_play.c ${PS1_PLATFORM_DIR}/recomp_pack.c)
    ps1_core_exported(ps1_play) # first, so the whole archive comes before the plain one
    target_include_directories(ps1_play PRIVATE ${PS1_PLATFORM_DIR})
    target_link_libraries(ps1_play PRIVATE ps1_runtime ${CMAKE_DL_LIBS})
    target_compile_options(ps1_play PRIVATE -Wall -Wextra)
endif()

# ps1_run: the headless runner (desktop hosts only); --pack loads a recompiled title (static recompilation, XBA-160..162).
if(NOT ANDROID AND NOT TARGET ps1_run)
    add_executable(ps1_run tools/ps1_run.c)
    ps1_core_exported(ps1_run) # first, so the whole archive comes before the plain one
    target_link_libraries(ps1_run PRIVATE ps1_core)
    target_compile_options(ps1_run PRIVATE -Wall -Wextra)
endif()
