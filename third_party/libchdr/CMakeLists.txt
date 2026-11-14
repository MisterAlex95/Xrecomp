# ps1_chdr: libchdr, the CHD (MAME compressed hunks of data) reader, as a static library for the PS1 disc reader
# (consoles/ps1/runtime/ps1_disc.c, XBA-159). Not our code: vendored unmodified from https://github.com/rtissera/libchdr
# at tag v0.3.0 (commit 93d8c239ff0d4e8d7722985992649fce12d2463b, 2026-04-24) with the dependencies it ships in
# deps/. Only the reader sources and licences were copied (no tests, no fuzzer corpus, no LZMA assembly); this file
# replaces libchdr's own CMakeLists.txt (one static library, no install, no shared build). Licences (see NOTICE):
#   libchdr            BSD-3-Clause (LICENSE.txt), dr_flac (include/dr_libs) public domain / MIT-0
#   deps/lzma-25.01    LZMA SDK decoder, public domain (deps/lzma-25.01/LICENSE)
#   deps/miniz-3.1.1   zlib inflate, MIT (deps/miniz-3.1.1/LICENSE)
#   deps/zstd-1.5.7    zstd single-file decoder, BSD-3-Clause (deps/zstd-1.5.7/LICENSE; dual BSD / GPLv2, BSD chosen)
cmake_minimum_required(VERSION 3.20)

if(NOT TARGET ps1_chdr)
    add_library(ps1_chdr STATIC
        src/libchdr_bitstream.c
        src/libchdr_cdrom.c
        src/libchdr_chd.c
        src/libchdr_codec_cdfl.c
        src/libchdr_codec_cdlz.c
        src/libchdr_codec_cdzl.c
        src/libchdr_codec_cdzs.c
        src/libchdr_codec_flac.c
        src/libchdr_codec_huff.c
        src/libchdr_codec_lzma.c
        src/libchdr_codec_zlib.c
        src/libchdr_codec_zstd.c
        src/libchdr_flac.c
        src/libchdr_huffman.c
        deps/lzma-25.01/src/LzmaDec.c
        deps/miniz-3.1.1/miniz.c
        deps/zstd-1.5.7/zstddeclib.c)
    target_include_directories(ps1_chdr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(ps1_chdr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/deps/miniz-3.1.1
                                                ${CMAKE_CURRENT_SOURCE_DIR}/deps/zstd-1.5.7
                                                ${CMAKE_CURRENT_SOURCE_DIR}/deps/lzma-25.01/include)
    target_compile_definitions(ps1_chdr PRIVATE WANT_RAW_DATA_SECTOR=1 WANT_SUBCODE=1 VERIFY_BLOCK_CRC=1
                               MINIZ_NO_ARCHIVE_APIS MINIZ_NO_DEFLATE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)
    set_target_properties(ps1_chdr PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    # Third-party code: its warnings are not ours to fix.
    target_compile_options(ps1_chdr PRIVATE -w)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(ps1_chdr PRIVATE -O2)
    endif()
endif()
