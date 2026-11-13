cmake_minimum_required(VERSION 3.20)
project(xiso C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Xbox disc image (XISO / XDVDFS) extraction (XBA-40, xiso.h): the Android app extracts a picked disc image into the
# game folder with it, tools/xiso/xiso_extract does the same on a desktop. libc (and pthread) only, POSIX hosts.
# xiso_source.c (XBA-126): split parts and compressed images (CSO / ZSO / CCI, its own inflate and LZ4), which the
# runtime's disc backing (src/kernel/kernel_disc.c) reads in place.
add_library(xiso STATIC xiso.c xiso_source.c)
target_include_directories(xiso PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(xiso PUBLIC Threads::Threads)
set_target_properties(xiso PROPERTIES POSITION_INDEPENDENT_CODE ON)   # linked into the app's libmain.so
if(NOT MSVC)
    target_compile_options(xiso PRIVATE -Wall -Wextra)
endif()
