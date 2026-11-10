cmake_minimum_required(VERSION 3.20)
project(xiso C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Xbox disc image (XISO / XDVDFS) extraction (XBA-40, xiso.h): the Android app extracts a picked disc image into the
# game folder with it, tools/xiso/xiso_extract does the same on a desktop. libc only, POSIX hosts.
add_library(xiso STATIC xiso.c)
target_include_directories(xiso PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(xiso PROPERTIES POSITION_INDEPENDENT_CODE ON)   # linked into the app's libmain.so
if(NOT MSVC)
    target_compile_options(xiso PRIVATE -Wall -Wextra)
endif()
