# The LSFG shader-cache helpers (lsfg_dll.c, lsfg_spv.c, lsfg_cache.c): C, libc only. Included by
# src/framegen/CMakeLists.txt, and on its own by the Android app's libxba_import.so (DLL import from the picker)
# and the tests, none of which need the pacer or Vulkan.
if(NOT TARGET lsfg_shaders)
    add_library(lsfg_shaders STATIC
        ${CMAKE_CURRENT_LIST_DIR}/lsfg_dll.c
        ${CMAKE_CURRENT_LIST_DIR}/lsfg_spv.c
        ${CMAKE_CURRENT_LIST_DIR}/lsfg_cache.c)
    target_include_directories(lsfg_shaders PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    set_target_properties(lsfg_shaders PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
                                                  POSITION_INDEPENDENT_CODE ON)
    if(NOT MSVC)
        target_compile_options(lsfg_shaders PRIVATE -Wall -Wextra)
    endif()
endif()
