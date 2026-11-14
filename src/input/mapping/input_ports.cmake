# input_ports: which host controller is which pad port (input_ports.h, XBA-123). Pure logic, libc only, no Xbox type:
# part of the host layer (src/platform/HOST_LAYER.md, XBA-145). Included by mapping/CMakeLists.txt (xbox_input_map
# links it) and by src/host/CMakeLists.txt (xbr_host); the guard keeps one definition.
if(NOT TARGET input_ports)
    add_library(input_ports STATIC ${CMAKE_CURRENT_LIST_DIR}/../input_ports.c)
    target_include_directories(input_ports PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)
    target_compile_features(input_ports PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(input_ports PRIVATE /W4)
    else()
        target_compile_options(input_ports PRIVATE -Wall -Wextra)
    endif()
endif()
