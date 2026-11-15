# The pinned sources of frame generation (XBA-187): framegen (GameNative/lsfg-vk-android d33caceb, MIT) with volk,
# and DXVK v2.6.2 (zlib) with SPIRV-Headers, when no checkout is given. Pins: lsfg_sources.txt (name, SHA-256, URL).
#
#   xbr_lsfg_source(<name> <out-var>)   the extracted source tree of <name>, in the build tree
#
# Tarballs: XBOXRECOMP_DEPS_DIR (default $XBOXRECOMP_DEPS_DIR, else third_party/lsfg-sources/ of this repository,
# git-ignored) holds <name>-<sha256:16>.tar.gz. A tarball already there (the desktop downloads carry them, the
# Android image has them, a previous build fetched them) is used as is once its SHA-256 matches; otherwise it is
# downloaded once and checked. Offline with nothing there: configure with -DXBOXRECOMP_LSFG=OFF, or point
# XBOXRECOMP_LSFG_VK_SRC / XBOXRECOMP_LSFG_DXVK_SRC at checkouts. Each build tree extracts its own copy
# (lsfg-src/<name>), so builds never share a writable source tree.
include_guard(GLOBAL)

get_filename_component(_xbr_lsfg_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(DEFINED ENV{XBOXRECOMP_DEPS_DIR} AND NOT "$ENV{XBOXRECOMP_DEPS_DIR}" STREQUAL "")
    set(_xbr_deps_default "$ENV{XBOXRECOMP_DEPS_DIR}")
else()
    set(_xbr_deps_default "${_xbr_lsfg_root}/third_party/lsfg-sources")
endif()
set(XBOXRECOMP_DEPS_DIR "${_xbr_deps_default}" CACHE PATH
    "Where the pinned frame generation tarballs are kept (src/framegen/lsfg_sources.txt)")
set(_xbr_lsfg_pins "${CMAKE_CURRENT_LIST_DIR}/lsfg_sources.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_xbr_lsfg_pins}")

# The tarball of <name>, checked: its path in <out-var>.
function(xbr_lsfg_tarball name out)
    file(STRINGS "${_xbr_lsfg_pins}" _lines REGEX "^${name} ")
    list(LENGTH _lines _n)
    if(NOT _n EQUAL 1)
        message(FATAL_ERROR "lsfg_sources.txt: no single line for ${name}")
    endif()
    string(REPLACE " " ";" _f "${_lines}")
    list(GET _f 1 _sha)
    list(GET _f 2 _url)
    string(SUBSTRING "${_sha}" 0 16 _sha16)
    set(_tgz "${XBOXRECOMP_DEPS_DIR}/${name}-${_sha16}.tar.gz")
    file(MAKE_DIRECTORY "${XBOXRECOMP_DEPS_DIR}")
    # One configure at a time downloads (Gradle configures its variants in parallel).
    file(LOCK "${XBOXRECOMP_DEPS_DIR}/.lock" GUARD FUNCTION TIMEOUT 600)
    if(EXISTS "${_tgz}")
        file(SHA256 "${_tgz}" _have)
        if(NOT _have STREQUAL _sha)
            message(WARNING "${_tgz}: SHA-256 ${_have}, expected ${_sha}: fetching it again")
            file(REMOVE "${_tgz}")
        endif()
    endif()
    if(NOT EXISTS "${_tgz}")
        message(STATUS "LSFG: fetching ${name} (${_url})")
        file(DOWNLOAD "${_url}" "${_tgz}.part" EXPECTED_HASH SHA256=${_sha} TLS_VERIFY ON STATUS _st)
        list(GET _st 0 _code)
        if(NOT _code EQUAL 0)
            file(REMOVE "${_tgz}.part")
            list(GET _st 1 _msg)
            message(FATAL_ERROR "LSFG: cannot fetch ${name} from ${_url}: ${_msg}\n"
                "Frame generation is built by default from pinned sources (docs/technical/framegen.md « Build »). "
                "Offline: put ${name}-${_sha16}.tar.gz into ${XBOXRECOMP_DEPS_DIR} (tools/lsfg/fetch_sources.sh), "
                "pass checkouts (-DXBOXRECOMP_LSFG_VK_SRC=..., -DXBOXRECOMP_LSFG_DXVK_SRC=...), "
                "or build without frame generation: -DXBOXRECOMP_LSFG=OFF (Android: -PxbaLsfg=0).")
        endif()
        file(RENAME "${_tgz}.part" "${_tgz}")
    endif()
    set(${out} "${_tgz}" PARENT_SCOPE)
    set(${out}_SHA "${_sha}" PARENT_SCOPE)
endfunction()

# <name> extracted into <build>/lsfg-src/<name> (the archive's top folder stripped); re-extracted when the pin moves.
function(xbr_lsfg_source name out)
    set(_dir "${CMAKE_BINARY_DIR}/lsfg-src/${name}")
    xbr_lsfg_tarball(${name} _tgz)
    if(NOT EXISTS "${_dir}/.xbr-sha256" OR NOT EXISTS "${_dir}/.xbr-complete")
        set(_stamp "")
    else()
        file(READ "${_dir}/.xbr-sha256" _stamp)
    endif()
    if(NOT _stamp STREQUAL _tgz_SHA)
        file(REMOVE_RECURSE "${_dir}" "${_dir}.x")
        file(ARCHIVE_EXTRACT INPUT "${_tgz}" DESTINATION "${_dir}.x")
        file(GLOB _top LIST_DIRECTORIES true "${_dir}.x/*")
        list(LENGTH _top _n)
        if(NOT _n EQUAL 1 OR NOT IS_DIRECTORY "${_top}")
            message(FATAL_ERROR "${_tgz}: expected one top folder")
        endif()
        file(RENAME "${_top}" "${_dir}")
        file(REMOVE_RECURSE "${_dir}.x")
        file(WRITE "${_dir}/.xbr-sha256" "${_tgz_SHA}")
        file(WRITE "${_dir}/.xbr-complete" "")
    endif()
    set(${out} "${_dir}" PARENT_SCOPE)
endfunction()

# A submodule's tree copied into its place in the parent tree, again whenever its pin moved.
function(_xbr_lsfg_nest from to)
    file(READ "${from}/.xbr-sha256" _want)
    set(_have "")
    if(EXISTS "${to}/.xbr-sha256")
        file(READ "${to}/.xbr-sha256" _have)
    endif()
    if(NOT _have STREQUAL _want)
        file(REMOVE_RECURSE "${to}")
        file(COPY "${from}/" DESTINATION "${to}")
    endif()
endfunction()

# The framegen tree with volk in its thirdparty/volk (the fork's submodule, at the fork's pin), as
# XBOXRECOMP_LSFG_VK_SRC expects it.
function(xbr_lsfg_fetch_framegen out)
    xbr_lsfg_source(lsfg-vk-android _fg)
    xbr_lsfg_source(volk _volk)
    _xbr_lsfg_nest("${_volk}" "${_fg}/thirdparty/volk")
    set(${out} "${_fg}" PARENT_SCOPE)
endfunction()

# The DXVK tree with SPIRV-Headers in its include/spirv (its submodule, at DXVK's pin), as
# XBOXRECOMP_LSFG_DXVK_SRC expects it.
function(xbr_lsfg_fetch_dxvk out)
    xbr_lsfg_source(dxvk _dxvk)
    xbr_lsfg_source(spirv-headers _spv)
    _xbr_lsfg_nest("${_spv}" "${_dxvk}/include/spirv")
    set(${out} "${_dxvk}" PARENT_SCOPE)
endfunction()
