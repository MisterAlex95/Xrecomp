cmake_minimum_required(VERSION 3.20)
project(n64_runtime C)

# n64_runtime: the Nintendo 64 runtime (MIPS VR4300, big-endian, 64-bit), XBA-146. The core (N-1 XBA-169 ROM / boot,
# N-3 XBA-171 interpreter, N-4 XBA-172 PI / SI / VI / AI / saves) is n64_core.h; the RSP and the RDP are reached
# through n64_devices.h. See consoles/README.md and the Plane spec « Spec · Other consoles ». Built when
# XBR_CONSOLE_N64 is ON (root CMakeLists.txt); a project may also add this directory alone (consoles/n64/tests does).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The RCP devices, each the real module (n64_rsp.c / n64_rdp.c) when present or its placeholder n64_<dev>_stub.c, in a
# target of its own so the modules merge without duplicate symbols.
# n64_rdp: the RDP (N-5, XBA-173, n64_devices.h + n64_rdp.h): plain C11 (+ pthreads for the optional scanline bands),
# no host dependency, so tests build anywhere. n64_rdp.c replaces the core's n64_rdp_stub.c when it is present.
if(NOT TARGET n64_rdp AND (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rdp.c OR EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rdp_stub.c))
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rdp.c)
        set(N64_RDP_SRC n64_rdp.c)
    else()
        set(N64_RDP_SRC n64_rdp_stub.c)
    endif()
    add_library(n64_rdp STATIC ${N64_RDP_SRC})
    target_include_directories(n64_rdp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(n64_rdp PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(n64_rdp PRIVATE -O2) # the software rasterizer is far too slow unoptimised
    endif()
    # Auto-vectorised 4-channel code reads back scalar stores as vectors (store-forwarding stalls): 1.6x slower.
    target_compile_options(n64_rdp PRIVATE $<$<C_COMPILER_ID:GNU>:-fno-tree-vectorize>
                           $<$<C_COMPILER_ID:Clang,AppleClang>:-fno-slp-vectorize>)
    set_target_properties(n64_rdp PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_package(Threads)
    if(Threads_FOUND)
        target_link_libraries(n64_rdp PUBLIC Threads::Threads)
    endif()
endif()

# n64_rsp: the RSP (N-5, XBA-173, n64_devices.h), LLE scalar + vector unit: plain C11, no host dependency, so tests
# build anywhere. n64_rsp.c replaces the core's n64_rsp_stub.c when it is present. It calls the n64_rdp_* functions
# (COP0 8..15) but does not link an RDP: the consumer links one (n64_rdp, or a test's fake). Vectorisation stays on:
# the 8-lane vector-unit loops are 1.7x faster with it.
if(NOT TARGET n64_rsp AND (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rsp.c OR EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rsp_stub.c))
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rsp.c)
        set(N64_RSP_SRC n64_rsp.c)
    else()
        set(N64_RSP_SRC n64_rsp_stub.c)
    endif()
    add_library(n64_rsp STATIC ${N64_RSP_SRC})
    target_include_directories(n64_rsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(n64_rsp PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(n64_rsp PRIVATE -O2) # microcode must run in real time even in debug builds
    endif()
    set_target_properties(n64_rsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# n64_core: the emulated machine (CPU, COP0 / TLB, COP1, bus, MI / PI / SI / PIF / VI / AI, saves) over n64_rsp and
# n64_rdp. Plain C11 + libm, no host dependency, so the tests and n64_run build anywhere (no SDL).
if(NOT TARGET n64_core)
    add_library(n64_core STATIC n64_core.c n64_cpu.c n64_fpu.c n64_bus.c n64_io.c n64_pif.c n64_rom.c)
    target_include_directories(n64_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(n64_core PUBLIC n64_rsp n64_rdp)
    find_library(N64_LIBM m)
    if(N64_LIBM)
        target_link_libraries(n64_core PUBLIC ${N64_LIBM})
    endif()
    target_compile_options(n64_core PRIVATE -Wall -Wextra)
    # COP1 runs on the host FPU between fesetround / fetestexcept: keep the compiler from moving or folding it.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(n64_fpu.c PROPERTIES COMPILE_OPTIONS "-frounding-math;-fno-fast-math")
    endif()
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(n64_core PRIVATE -O2) # the interpreter is far too slow unoptimised
    endif()
    set_target_properties(n64_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# N64_CORE_ONLY=ON skips n64_runtime (no xbr_host / SDL2), so a host without SDL2 can still build and test the core
# and the devices (like PS1_CORE_ONLY).
option(N64_CORE_ONLY "Build only n64_core, the RCP devices and n64_run, not n64_runtime (no xbr_host / SDL2)" OFF)
if(NOT N64_CORE_ONLY AND NOT TARGET xbr_host)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
endif()
# n64_runtime: the core + the host layer (n64_runtime_start: still the pack stub until the Android hookup).
if(NOT N64_CORE_ONLY AND NOT TARGET n64_runtime)
    add_library(n64_runtime STATIC n64_runtime.c)
    target_include_directories(n64_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(n64_runtime PUBLIC n64_core xbr_host)
    target_compile_options(n64_runtime PRIVATE -Wall -Wextra)
    set_target_properties(n64_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# n64_run: the headless runner (desktop hosts only).
if(NOT ANDROID AND NOT TARGET n64_run)
    add_executable(n64_run tools/n64_run.c)
    target_link_libraries(n64_run PRIVATE n64_core)
    target_compile_options(n64_run PRIVATE -Wall -Wextra)
endif()
