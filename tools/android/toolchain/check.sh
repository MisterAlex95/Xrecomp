#!/usr/bin/env bash
# Checks the on-device toolchain without a device (docs/android/toolchain.md):
#   1. every jniLib is an aarch64 ELF that needs only system libraries (or ones we ship); libxbacc.so is static
#   2. libxbacc.so runs under qemu-aarch64 as is: it compiles a C file against the shipped sysroot (assets/prepare/
#      sysroot.zip, extracted as the app does) and ld.lld links it, as a shared object and as an executable
#   3. tools.prepare --android builds the stub title's pack (tools/conformance/test.xbe: our synthetic XBE, no game
#      content) with the shipped compiler, as the app runs it (--cc libxbacc.so --cc-prefix-arg clang --sysroot
#      <sysroot.zip> --shell-lib libmain.so), against the shell's real libmain.so, built here with the NDK
#   4. with XBA_TC_ANDROID_ROOT (Android's /system/bin/linker64 and bionic, made by android_root.sh): the dynamic
#      parts too. libxbapython.so runs (capstone, tools.prepare --help) and runs step 3 itself with the shipped
#      standard library; the executable clang linked runs.
#   tools/android/toolchain/check.sh TOOLCHAIN_DIR        (toolchain image: qemu-user, python3-capstone, SDL2_SRC)
# qemu-user cannot exec another aarch64 binary without binfmt: the compiler and the Python of the child steps are
# reached through small host scripts that start qemu (--cc and XBA_PYTHON point at them).
set -euo pipefail
ulimit -c 0
TC=$(cd "${1:?usage: check.sh TOOLCHAIN_DIR}" && pwd)
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
W=${XBA_TC_CHECK_WORK:-/work/tc-check}
rm -rf "$W" && mkdir -p "$W"
XBA_TC_OUT=$TC XBA_TC_WORK=$W/work XBA_TC_CACHE=$W/cache
. "$(dirname "$0")/common.sh"            # versions, check_jnilib, JOBS
N=$JNI_DIR
MAJOR=${LLVM_VERSION%%.*}
API=$XBA_TC_API
QEMU=qemu-aarch64
AROOT=${XBA_TC_ANDROID_ROOT:-}
fail() { echo "FAIL: $*" >&2; exit 1; }
has() { grep -E "$1" >/dev/null; }      # reads all its input (grep -q + pipefail would see a SIGPIPE)
unzip_to() { python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$1" "$2"; }

echo "== jniLibs"
for so in "$N"/*.so; do check_jnilib "$so"; done
if "$NDK_BIN/llvm-readelf" -d "$N/libxbacc.so" | has NEEDED; then fail "libxbacc.so is not static"; fi
echo "== assets"
for z in "$ASSET_DIR"/*.zip; do
    printf '%-24s %10s\n' "$(basename "$z")" "$(stat -c%s "$z")"
    (cd "$ASSET_DIR" && sha256sum -c --quiet "$(basename "$z").sha256")
done

# What the app installs (Toolchain.java): files/sysroot-<v>, files/python-<v>.
F=$W/files
SYS=$F/sysroot
unzip_to "$ASSET_DIR/sysroot.zip" "$SYS"
RES=$SYS/lib/clang/$MAJOR
LIBS=$SYS/usr/lib/$XBA_TC_TRIPLE
cat > "$W/xbacc" <<EOF
#!/bin/sh
exec $QEMU $N/libxbacc.so "\$@"
EOF
chmod +x "$W/xbacc"

echo "== clang under qemu-aarch64"
"$W/xbacc" clang --version | sed -n 1,2p
"$W/xbacc" ld.lld --version
cat > "$W/hello.c" <<'EOF'
#include <stdio.h>
#include <stddef.h>
#include <math.h>
#include <android/log.h>
int xba_hello(double x) { return (int)sqrt(x); }
int main(void) { printf("hello from on-device clang: %d\n", xba_hello(1764.0)); return 0; }
EOF
t0=$(date +%s)
"$W/xbacc" clang --sysroot="$SYS" -resource-dir="$RES" -O2 -fPIC -c "$W/hello.c" -o "$W/hello.o"
END=("$RES/lib/linux/libclang_rt.builtins-aarch64-android.a" "$RES/lib/linux/aarch64/libunwind.a" -ldl -lc
     "$RES/lib/linux/libclang_rt.builtins-aarch64-android.a" "$RES/lib/linux/aarch64/libunwind.a" -ldl)
COMMON=(-EL --fix-cortex-a53-843419 -z now -z relro -z max-page-size=16384 --hash-style=gnu --eh-frame-hdr
        -m aarch64linux -L"$LIBS/$API" -L"$LIBS")
"$W/xbacc" ld.lld "${COMMON[@]}" -shared -o "$W/libhello.so" "$LIBS/$API/crtbegin_so.o" "$W/hello.o" -lm \
    "${END[@]}" "$LIBS/$API/crtend_so.o"
"$W/xbacc" ld.lld "${COMMON[@]}" -pie -dynamic-linker /system/bin/linker64 -o "$W/hello" \
    "$LIBS/$API/crtbegin_dynamic.o" "$W/hello.o" -lm "${END[@]}" "$LIBS/$API/crtend_android.o"
echo "hello: compiled and linked twice in $(($(date +%s) - t0)) s"
"$NDK_BIN/llvm-readelf" -h "$W/libhello.so" | has 'Type:.*DYN' || fail "libhello.so is not a shared object"
"$NDK_BIN/llvm-readelf" -h "$W/hello" | has 'AArch64' || fail "hello is not aarch64"
"$NDK_BIN/llvm-nm" -D --defined-only "$W/libhello.so" | has ' T xba_hello$' || fail "xba_hello not exported"

PY=(python3)
if [ -n "$AROOT" ]; then
    echo "== libxbapython.so under qemu-aarch64 with Android's bionic ($AROOT)"
    unzip_to "$ASSET_DIR/python-stdlib.zip" "$F/python"
    mkdir -p "$F/pycache" "$F/tmp"
    # The environment of PrepareService (docs/android/prepare.md « What the app provides »).
    cat > "$W/python" <<EOF
#!/bin/bash
export PYTHONHOME=$F/python PYTHONPYCACHEPREFIX=$F/pycache TMPDIR=$F/tmp HOME=$F LIBCAPSTONE_PATH=$N \\
       LD_LIBRARY_PATH=$N PYTHONUNBUFFERED=1 XBA_PYTHON=$W/python
exec $QEMU -L $AROOT $N/libxbapython.so "\$@" 2> >(grep -v -e linkerconfig -e tzdata >&2)
EOF
    chmod +x "$W/python"
    "$W/python" -c 'import sys, platform; print(sys.version.split()[0], sys.platform, platform.machine(), sys.executable)'
    "$W/python" -c 'import capstone
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
print("capstone", capstone.cs_version(), [i.mnemonic for i in md.disasm(bytes.fromhex("8b44240403442408c3"), 0)])'
    (cd "$ROOT" && PYTHONPATH=$ROOT "$W/python" -m tools.prepare --help | sed -n 1p)
    "$QEMU" -L "$AROOT" "$W/hello" 2> >(grep -v -e linkerconfig -e tzdata >&2) \
        | has '^hello from on-device clang: 42$' || fail "hello did not run"
    echo "hello ran under bionic: OK"
    PY=("$W/python")
fi

echo "== the shell's libmain.so (NDK), for --shell-lib"
cmake -S "$ROOT/android/app/src/main/cpp" -B "$W/shell" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="$API" -DXBOXRECOMP_SDL2_SOURCE_DIR="${SDL2_SRC:?}" -DXBOXRECOMP_BUILD_TESTS=OFF >/dev/null
cmake --build "$W/shell" --target main -j"$JOBS" >/dev/null
SHELL_LIB=$(find "$W/shell" -name libmain.so | sort | sed -n 1p)
[ -n "$SHELL_LIB" ] || fail "no libmain.so"

echo "== tools.prepare --android with the shipped compiler (Python: ${PY[0]})"
# The stub's entry (xbe_entry.c, which calls the five functions) out of stub_title.sh, with the host Python.
cp -r "$ROOT" "$W/repo" && rm -rf "$W/repo/.git" "$W/repo/android"
(cd "$W/repo" && PYTHON=python3 tools/android/stub_title.sh "$W/stub" >/dev/null)
t0=$(date +%s)
(cd "$W/repo" && PYTHONPATH=$W/repo "${PY[@]}" -m tools.prepare tools/conformance/test.xbe --android \
    --work "$W/aw" --pack-dir "$W/packs" --cc "$W/xbacc" --cc-prefix-arg clang --sysroot "$SYS" \
    --shell-lib "$SHELL_LIB" --jobs 2 --extra-src "$W/stub/src/recomp/gen/xbe_entry.c") | tee "$W/prepare.out" || true
echo "prepare: $(($(date +%s) - t0)) s"
has '^XBA_RESULT ok pack=' < "$W/prepare.out" || { tail -40 "$W/aw/prepare.log"; fail "prepare"; }
LIB=$(sed -n 's/^XBA_RESULT ok pack=//p' "$W/prepare.out")/libtitle.so
"$NDK_BIN/llvm-readelf" -h "$LIB" | grep -E 'Machine|Type'
"$NDK_BIN/llvm-readelf" -d "$LIB" | grep -E 'NEEDED|SONAME'
want=' T (recomp_title_main|xbe_entry_point|sub_00011000)$'
[ "$("$NDK_BIN/llvm-nm" -D --defined-only "$LIB" | grep -cE "$want")" = 3 ] || fail "libtitle.so symbols"
"$NDK_BIN/llvm-readelf" -d "$LIB" | has 'NEEDED.*\[libmain\.so\]' || fail "libtitle.so does not need libmain.so"
cat "$(dirname "$LIB")/pack.json"; echo
echo "OK: the on-device toolchain works ($( [ -n "$AROOT" ] && echo "Python and clang" || echo "clang; the shipped Python not run: no XBA_TC_ANDROID_ROOT"))"
rm -rf "$W"
