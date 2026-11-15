# The DXBC -> SPIR-V translator (lsfg_dxbc.cpp) over DXVK's DXBC compiler. In: XBOXRECOMP_LSFG_DXVK_SRC, a DXVK
# checkout at v2.6.2 with its include/spirv submodule (SPIRV-Headers) populated; DXVK is zlib-licensed, used from
# that path, never vendored. Out: the target lsfg_dxbc (static, PIC).
#
# DXVK's src/dxbc includes "../dxvk/dxvk_shader.h" (the whole Vulkan back end), so src/dxbc, src/spirv and src/util
# are staged into the build tree next to our reduced stand-ins (dxvk_shim/dxvk/), and only the files the compiler
# needs are built. 2.6.2 is the pin: 2.7 moved samplers to a global heap (descriptor indexing), which framegen's
# fixed per-pass layouts cannot express.
if(TARGET lsfg_dxbc)
    return()
endif()
enable_language(CXX)
set(_dxvk "${XBOXRECOMP_LSFG_DXVK_SRC}")
if(NOT EXISTS "${_dxvk}/src/dxbc/dxbc_module.h")
    message(FATAL_ERROR "XBOXRECOMP_LSFG_DXVK_SRC=${_dxvk}: no src/dxbc/dxbc_module.h (clone DXVK at v2.6.2)")
endif()
if(NOT EXISTS "${_dxvk}/include/spirv/include/spirv/unified1/spirv.hpp")
    message(FATAL_ERROR "${_dxvk}/include/spirv is empty: run `git -C ${_dxvk} submodule update --init include/spirv`")
endif()
set(_dxvk_version "unknown")
if(EXISTS "${_dxvk}/meson.build")
    file(STRINGS "${_dxvk}/meson.build" _l REGEX "version *: *'v?[0-9.]+'" LIMIT_COUNT 1)
    if(_l MATCHES "'v?([0-9.]+)'")
        set(_dxvk_version "${CMAKE_MATCH_1}")
    endif()
endif()

set(_stage "${CMAKE_BINARY_DIR}/lsfg-dxvk-stage/src")
file(REMOVE_RECURSE "${_stage}")
file(COPY "${_dxvk}/src/dxbc" "${_dxvk}/src/spirv" "${_dxvk}/src/util" DESTINATION "${_stage}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/dxvk_shim/dxvk" DESTINATION "${_stage}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/dxvk_shim/dxvk/dxvk_shader.h"
    "${CMAKE_CURRENT_LIST_DIR}/dxvk_shim/dxvk/dxvk_device.h"
    "${CMAKE_CURRENT_LIST_DIR}/dxvk_shim/dxvk/dxbc_options_default.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/dxvk_shim/dxvk/util_env_min.cpp")

file(GLOB _dxbc_src "${_stage}/dxbc/*.cpp")
list(REMOVE_ITEM _dxbc_src "${_stage}/dxbc/dxbc_options.cpp")
add_library(lsfg_dxbc STATIC
    ${CMAKE_CURRENT_LIST_DIR}/lsfg_dxbc.cpp
    ${_dxbc_src}
    ${_stage}/dxvk/dxbc_options_default.cpp
    ${_stage}/spirv/spirv_code_buffer.cpp
    ${_stage}/spirv/spirv_compression.cpp
    ${_stage}/spirv/spirv_module.cpp
    ${_stage}/util/log/log.cpp
    ${_stage}/util/log/log_debug.cpp
    ${_stage}/dxvk/util_env_min.cpp
    ${_stage}/util/util_string.cpp)
target_include_directories(lsfg_dxbc
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}
    PRIVATE ${_stage} ${_dxvk}/include/spirv/include ${_dxvk}/include/native/windows)
target_compile_definitions(lsfg_dxbc PRIVATE NOMINMAX LSFG_DXVK_VERSION="${_dxvk_version}")
set_target_properties(lsfg_dxbc PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
# DXVK's code, built as is. Its dxbc_module.h uses std::optional without including <optional> (libc++ brings it in
# through other headers, libstdc++ does not): forced in for GCC hosts.
target_compile_options(lsfg_dxbc PRIVATE -w -include optional)
if(NOT ANDROID)   # the NDK sysroot has the Vulkan headers
    include(${CMAKE_CURRENT_LIST_DIR}/lsfg_vulkan_headers.cmake)
    target_include_directories(lsfg_dxbc PRIVATE "${XBR_LSFG_VULKAN_HEADERS_STAGE}/include")
endif()
find_package(Threads REQUIRED)
target_link_libraries(lsfg_dxbc PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
message(STATUS "LSFG: DXBC translator over DXVK ${_dxvk_version} (${_dxvk})")
