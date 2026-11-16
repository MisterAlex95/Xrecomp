#!/bin/sh
# Builds our own Win32 hello worlds (tests/win32_hello, XBA-233) into 32-bit PEs with MinGW-w64 i686:
#   OUT/hello.exe      no CRT: our own entry point, imports GetStdHandle / WriteFile / ExitProcess
#   OUT/hello_crt.exe  mingw's CRT startup and msvcrt's printf
#
#   sh tools/win32/build_hello.sh OUT
#
# Uses i686-w64-mingw32-gcc from the PATH, else the xba-win32-mingw image (tools/docker/win32-mingw.Dockerfile,
# built on first use).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:?usage: build_hello.sh OUT}
mkdir -p "$out"
out=$(cd "$out" && pwd)

if ! command -v i686-w64-mingw32-gcc >/dev/null; then
    command -v docker >/dev/null || { echo "no i686-w64-mingw32-gcc and no docker" >&2; exit 2; }
    docker image inspect xba-win32-mingw >/dev/null 2>&1 \
        || docker build -q -f "$root/tools/docker/win32-mingw.Dockerfile" -t xba-win32-mingw "$root/tools/docker"
    exec docker run --rm -u "$(id -u):$(id -g)" -v "$root/tests/win32_hello:/in:ro" -v "$out:/out" \
        -v "$root/tools/win32/build_hello.sh:/build_hello.sh:ro" xba-win32-mingw sh /build_hello.sh /out /in
fi

src=${2:-$root/tests/win32_hello}
cc=i686-w64-mingw32-gcc
# -fno-asynchronous-unwind-tables: no .eh_frame to carry. --dynamicbase keeps the base relocations in the image.
$cc -O2 -fno-asynchronous-unwind-tables -nostdlib -nostartfiles -e _mainCRTStartup -Wl,--dynamicbase \
    "$src/hello.c" -lkernel32 -o "$out/hello.exe"
$cc -O2 -fno-asynchronous-unwind-tables -Wl,--dynamicbase "$src/hello_crt.c" -o "$out/hello_crt.exe"
ls -l "$out"/hello*.exe
