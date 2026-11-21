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
    "$rt"/x360_interp.c "$rt"/x360_file.c "$rt"/x360_xam.c "$rt"/x360_gpu.c "$rt"/xenos_cp.c "$rt"/x360_state.c "$rt"/x360_crypt.c "$rt"/x360_more.c "$rt"/x360_input.c "$rt"/x360_xma.c "$rt"/x360_seh.c "$rt"/x360_kdpc.c "$rt"/x360_rtl.c "$rt"/x360_crypt2.c "$rt"/x360_vd.c "$rt"/x360_xam2.c "$rt"/x360_ob.c \
    "$rt"/xenos_gl.c "$rt"/tools/x360_run.c -I "$repo/src/xiso" "$repo"/src/xiso/xiso.c \
    "$repo"/src/xiso/xiso_source.c "$rt"/x360_pad_map.c -I "$repo/src/input" "$repo"/src/input/console_pad.c \
    -ldl -lm -lpthread
mkdir -p "$work/disc/data"
printf 'Hello from the disc!' >"$work/disc/data/hello.txt"
printf 'binary data' >"$work/disc/data/b.bin"
printf 'readme\n' >"$work/disc/readme.txt"
python3 "$repo/tools/xiso/mkxiso.py" "$work/disc" "$work/disc.iso" >/dev/null
sysroot=$(dirname "$(dirname "$("$cc" -print-file-name=libc.so.6)")")
qemu-aarch64 -L "$sysroot" "$work/x360_run" "$work/hello.xex" --pack "$work/libtitle.so" --disc "$work/disc.iso" --dbg "$work/dbg.txt" --stats
cmp "$work/dbg.txt" "$here/hello.txt"
echo "arm64 (qemu): DbgPrint output matches hello.txt"
# the HLE and command processor tests too
core="$rt/x360_mem.c $rt/x360_load.c $rt/x360_thread.c $rt/x360_kernel.c $rt/x360_interp.c $rt/x360_file.c
      $rt/x360_xam.c $rt/x360_gpu.c $rt/xenos_cp.c $rt/x360_state.c $rt/x360_crypt.c $rt/x360_more.c $rt/x360_input.c $rt/x360_xma.c $rt/x360_seh.c
      $rt/x360_kdpc.c $rt/x360_rtl.c $rt/x360_crypt2.c $rt/x360_vd.c $rt/x360_xam2.c $rt/x360_ob.c
      $rt/xenos_gl.c
      $repo/src/xiso/xiso.c $repo/src/xiso/xiso_source.c"
for t in x360_hle_test x360_kdpc_test x360_rtl_test x360_crypt2_test x360_vd_test x360_xam2_test x360_ob_test xenos_cp_test xenos_draw_test; do
    # shellcheck disable=SC2086
    "$cc" -O2 -ffp-contract=off -DXE_RUNTIME=1 -D_GNU_SOURCE -I "$rt" -I "$repo/consoles/gc/runtime" \
        -I "$repo/src/xiso" -o "$work/$t" "$here/$t.c" $core -ldl -lm -lpthread
    if [ "$t" = xenos_draw_test ]; then
        # the renderer: needs make_draw.py's shaders and an arm64 libEGL / libGLESv2 in the sysroot (77 without)
        PYTHONPATH="$repo/consoles/x360/tools" python3 "$here/make_draw.py" "$work/draw" || exit 1
        rc=0
        EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 qemu-aarch64 -L "$sysroot" "$work/$t" "$work/draw" || rc=$?
        [ "$rc" = 0 ] || [ "$rc" = 77 ] || exit 1
        [ "$rc" = 0 ] || echo "arm64 (qemu): xenos_draw_test skipped (no arm64 EGL / GLES 3.0 here)"
    else
        qemu-aarch64 -L "$sysroot" "$work/$t"
    fi
done
# and as on a host of 16 KB pages (x360_mem.c: qemu's user mode keeps the host's page size, X360_HOST_PAGE does not)
X360_HOST_PAGE=16384 qemu-aarch64 -L "$sysroot" "$work/x360_run" "$work/hello.xex" --pack "$work/libtitle.so" \
    --disc "$work/disc.iso" --dbg "$work/dbg16k.txt"
cmp "$work/dbg16k.txt" "$here/hello.txt"
X360_HOST_PAGE=16384 qemu-aarch64 -L "$sysroot" "$work/x360_hle_test"
echo "arm64 (qemu): the same with 16 KB host pages"
