#!/bin/sh
# run_hello_arm64.sh WORKDIR -- the x360_hello test on arm64 under qemu (XBA-270), for a developer machine: run inside
# the xba-arm64-deps image (aarch64-linux-gnu-gcc + qemu-aarch64), from the repository root:
#
#   docker run --rm -v "$PWD:/src" -w /src xba-arm64-deps sh consoles/x360/tests/run_hello_arm64.sh /tmp/x360a64
#
# Builds x360_run and the title's pack for aarch64 (the cross toolchain's sysroot serves qemu), runs both
# under qemu-aarch64 and compares the DbgPrint text with hello.txt. (The CI's linux-arm64 job runs ctest x360_hello
# natively instead.)
set -e
work=$1
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
rt=$repo/consoles/x360/runtime
cc=${CROSS_CC:-aarch64-linux-gnu-gcc}
rm -rf "$work"
mkdir -p "$work"
export PYTHONPATH="$repo:$repo/consoles/x360/tools"
python3 "$here/make_hello.py" "$work/hello.xex" --compression basic
python3 -m x360_tools.recomp "$work/hello.xex" "$work/src" --no-comments
"$cc" -O2 -ffp-contract=off -DXE_RUNTIME=1 -D_GNU_SOURCE -I "$rt" -I "$repo/consoles/gc/runtime" -fPIC -shared \
    -o "$work/libtitle.so" "$work"/src/*.c -lm
"$cc" -O2 -ffp-contract=off -DXE_RUNTIME=1 -D_GNU_SOURCE -I "$rt" -I "$repo/consoles/gc/runtime" \
    -o "$work/x360_run" "$rt"/x360_mem.c "$rt"/x360_load.c "$rt"/x360_thread.c "$rt"/x360_kernel.c \
    "$rt"/x360_interp.c "$rt"/x360_file.c "$rt"/tools/x360_run.c -I "$repo/src/xiso" "$repo"/src/xiso/xiso.c \
    "$repo"/src/xiso/xiso_source.c -ldl -lm -lpthread
mkdir -p "$work/disc/data"
printf 'Hello from the disc!' >"$work/disc/data/hello.txt"
printf 'binary data' >"$work/disc/data/b.bin"
printf 'readme\n' >"$work/disc/readme.txt"
python3 "$repo/tools/xiso/mkxiso.py" "$work/disc" "$work/disc.iso" >/dev/null
sysroot=$(dirname "$(dirname "$("$cc" -print-file-name=libc.so.6)")")
qemu-aarch64 -L "$sysroot" "$work/x360_run" "$work/hello.xex" --pack "$work/libtitle.so" --disc "$work/disc.iso" --dbg "$work/dbg.txt" --stats
cmp "$work/dbg.txt" "$here/hello.txt"
echo "arm64 (qemu): DbgPrint output matches hello.txt"
