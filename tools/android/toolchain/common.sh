# Shared by the on-device toolchain build scripts (sourced). See docs/android/toolchain.md.
#   XBA_TC_OUT    the toolchain directory the app build picks up (-PxbaToolchainDir): jniLibs/arm64-v8a/, assets/
#   XBA_TC_WORK   build trees (removed by the caller when done)
#   XBA_TC_CACHE  downloaded sources, checked against versions.env
#   JOBS          parallel compile jobs (default 4, as in the other images)
# The host is the Docker image's Linux, or macOS (the NDK's darwin-x86_64 prebuilts, universal binaries; Homebrew
# cmake, ninja and a python3 >= 3.12 first in PATH): the host-specific bits are the NDK host tag, sha256 and sizes.
set -euo pipefail
ulimit -c 0

TC_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=versions.env
. "$TC_DIR/versions.env"

: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME (the xba-android image does)}"
XBA_TC_OUT=${XBA_TC_OUT:-/opt/xba-toolchain}
XBA_TC_WORK=${XBA_TC_WORK:-/work/toolchain}
XBA_TC_CACHE=${XBA_TC_CACHE:-/work/cache}
JOBS=${JOBS:-4}
case $(uname -s) in
    Darwin) NDK_HOST=darwin-x86_64 ;;
    *)      NDK_HOST=linux-x86_64 ;;
esac
NDK_PREBUILT=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$NDK_HOST
NDK_BIN=$NDK_PREBUILT/bin
NDK_SYSROOT=$NDK_PREBUILT/sysroot
JNI_DIR=$XBA_TC_OUT/jniLibs/arm64-v8a
# The app looks for its zips in assets/prepare/ (docs/android/prepare.md, Toolchain.java).
ASSET_DIR=$XBA_TC_OUT/assets/prepare
mkdir -p "$JNI_DIR" "$ASSET_DIR" "$XBA_TC_WORK" "$XBA_TC_CACHE"

log() { printf '\n=== %s\n' "$*" >&2; }

# sha256sum where there is one (Linux, Homebrew coreutils), else macOS's shasum: same input and output format.
if command -v sha256sum >/dev/null; then sha256() { sha256sum "$@"; }; else sha256() { shasum -a 256 "$@"; }; fi
# filesize FILE: its size in bytes (GNU and BSD stat differ).
filesize() { wc -c < "$1" | tr -d ' '; }

# fetch URL SHA256: the file in $XBA_TC_CACHE, downloaded once and always checked. Prints its path.
fetch() {
    local url=$1 sum=$2 f
    f=$XBA_TC_CACHE/$(basename "$url")
    if [ ! -f "$f" ]; then
        curl -fL --retry 5 --retry-all-errors -sS -o "$f.part" "$url"
        mv "$f.part" "$f"
    fi
    if ! echo "$sum  $f" | sha256 -c --quiet - >&2; then
        rm -f "$f"
        echo "checksum mismatch: $url" >&2
        return 1
    fi
    echo "$f"
}

# zip_dir DIR ZIP: DIR's files into ZIP (sorted, no extra attributes), and ZIP.sha256 ("<hex>  <name>", which the
# app checks before extracting).
zip_dir() {
    rm -f "$2" "$2.sha256"
    (cd "$1" && find . -type f -o -type l | sed 's#^\./##' | LC_ALL=C sort | zip -q -X -9 -@ "$2")
    (cd "$(dirname "$2")" && sha256 "$(basename "$2")" > "$(basename "$2").sha256")
    ls -l "$2"
}

# unpack ARCHIVE DIR: a fresh copy of the sources in DIR.
unpack() {
    rm -rf "$2" && mkdir -p "$2"
    tar -xf "$1" -C "$2" --strip-components=1
}

# check_jnilib FILE: an aarch64 ELF that needs only the libraries an app process has, or ones we ship.
check_jnilib() {
    local f=$1 needed bad
    # (Output captured first: grep -q stops reading early, and pipefail would count readelf's SIGPIPE.)
    [[ $("$NDK_BIN/llvm-readelf" -h "$f") == *AArch64* ]] || { echo "$f: not an AArch64 ELF" >&2; return 1; }
    needed=$("$NDK_BIN/llvm-readelf" -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
    # ("(pattern)": macOS's bash 3.2 cannot parse a case without it inside $(...).)
    bad=$(for n in $needed; do case $n in
            (libc.so|libm.so|libdl.so|liblog.so|libz.so|libpython3.*.so|libcapstone.so) ;;
            (*) echo "$n" ;; esac; done)
    [ -z "$bad" ] || { echo "$f: needs $bad" >&2; return 1; }
    printf '%-28s %10s  NEEDED: %s\n' "$(basename "$f")" "$(filesize "$f")" "${needed:-(none: static)}"
}
