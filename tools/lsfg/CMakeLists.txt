cmake_minimum_required(VERSION 3.20)
project(lsfg_tools C CXX)
# tools/lsfg/lsfg_translate (docs/technical/framegen.md, "DXBC translation"): a host tool that fills the LSFG shader
# cache from the user's own Lossless.dll with DXVK's DXBC compiler: DXVK v2.6.2, the pinned tarball fetched
# (src/framegen/lsfg_sources.cmake) unless XBOXRECOMP_LSFG_DXVK_SRC (or $DXVK_SRC) names a checkout:
#   cmake -S tools/lsfg -B build/lsfg [-DXBOXRECOMP_LSFG_DXVK_SRC=$DXVK_SRC] [-DXBOXRECOMP_VULKAN_HEADERS=<dir>]
# Not a test project: tools/run_c_tests.sh only walks tests/.
set(CMAKE_C_STANDARD 11)
set(XBOXRECOMP_LSFG_DXVK_SRC "$ENV{DXVK_SRC}" CACHE PATH "DXVK checkout (v2.6.2, include/spirv populated)")
if(NOT XBOXRECOMP_LSFG_DXVK_SRC)
    include(../../src/framegen/lsfg_sources.cmake)
    xbr_lsfg_fetch_dxvk(XBOXRECOMP_LSFG_DXVK_SRC)
endif()
include(../../src/framegen/lsfg_shaders.cmake)
include(../../src/framegen/lsfg_dxbc.cmake)
add_executable(lsfg_translate lsfg_translate.c)
target_link_libraries(lsfg_translate PRIVATE lsfg_shaders lsfg_dxbc)
set_target_properties(lsfg_translate PROPERTIES LINKER_LANGUAGE CXX)
