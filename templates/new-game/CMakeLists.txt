cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
#
# Android: this project is not built on its own but through the app shell,
# which sets XBOXRECOMP_SHELL_TITLE and links the title into its libmain.so:
#   cd xboxrecomp/android && gradle assembleDebug -PxbaTitleDir=/path/to/this/project
# (see "Android" below and docs/android/app-shell.md).
# ============================================================================

project(your_game_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
# (The Android app shell has added it already, and set XBOXRECOMP_DIR.)
if(NOT XBOXRECOMP_SHELL_TITLE)
    # Frame generation (-DXBOXRECOMP_LSFG=ON) links a C++20 library: CXX must be enabled at this level.
    if(XBOXRECOMP_LSFG)
        enable_language(CXX)
    endif()
    add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)
endif()

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The profile layers (profiles/ in the toolkit: XDK libraries, engine, title;
# copied here by tools.prepare, docs/technical/profiles.md): switches and
# fixes. main.c calls their hooks through the generated xba_layers.c.
file(GLOB TITLE_PROFILE_SOURCES CONFIGURE_DEPENDS "src/title/*.c")
if(TITLE_PROFILE_SOURCES)
    list(APPEND GAME_SOURCES ${TITLE_PROFILE_SOURCES})
    add_compile_definitions(XBA_TITLE_PROFILE=1)
endif()
# Where a run records what the static analysis missed (src/kernel/
# recomp_discovery.h): tools.prepare passes <work>/discovered.jsonl.
set(XBA_DISCOVERY_FILE "" CACHE STRING "default discovery file (RECOMP_DISCOVERY)")
if(XBA_DISCOVERY_FILE)
    add_compile_definitions(XBA_DISCOVERY_FILE="${XBA_DISCOVERY_FILE}")
endif()
# The options the title forces (title.json "env_force"), set by main() before
# anything reads the environment.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/title/env_force.h")
    add_compile_definitions(XBA_TITLE_ENV_FORCE=1)
endif()

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

if(XBOXRECOMP_SHELL_TITLE)
    # ── Android: the title inside the app shell's libmain.so ─────
    # The shell (android/app/src/main/cpp) owns SDL_main and the start-up
    # (logcat, recomp.cfg, storage, fast cores, fault handler, window,
    # lifecycle); main.c then defines recomp_title_main() instead of main(),
    # and takes its paths from recomp_storage. The target name is fixed: the
    # shell links xba_title. Everything after this block is for executables.
    add_library(xba_title OBJECT ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
    target_compile_definitions(xba_title PRIVATE XBOXRECOMP_SHELL_TITLE=1)
    target_link_libraries(xba_title PRIVATE xboxrecomp)
    target_include_directories(xba_title PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
        ${XBOXRECOMP_DIR}/include
        ${XBOXRECOMP_DIR}/src
    )
    return()
endif()

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time (Linux/macOS get SDL2 and the GL
# backend through the xboxrecomp target)
if(WIN32)
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
