#!/usr/bin/env bash
# A minimal Android userland for qemu-aarch64 -L: Android 13's dynamic linker and bionic (linker64, libc, libm,
# libdl, ld-android, liblog, libz, libc++, libbase), taken from Google's emulator system image (API 33, the AYN
# Thor's Android) through sdkmanager, which checks the download. With it, check.sh runs the dynamically linked
# jniLibs (libxbapython.so) without a device. About 1.3 GB downloaded and 5 GB of scratch space, all removed at
# the end; the result is 3 MB.
#   tools/android/toolchain/android_root.sh OUT_DIR        (in the toolchain image: sdkmanager, debugfs)
set -euo pipefail
ulimit -c 0
OUT=${1:?usage: android_root.sh OUT_DIR}
PKG="system-images;android-33;default;arm64-v8a"
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/android-root.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

yes | sdkmanager --sdk_root="$tmp/sdk" --licenses >/dev/null
sdkmanager --sdk_root="$tmp/sdk" "$PKG" >/dev/null
img=$tmp/sdk/system-images/android-33/default/arm64-v8a/system.img
rm -rf "$tmp/sdk/emulator"
# system.img is a GPT disk: vbmeta, then "super" (dynamic partitions). Find super's first sector.
start=$(python3 -I - "$img" <<'EOF'
import struct, sys
with open(sys.argv[1], "rb") as f:
    f.seek(1024)
    for _ in range(128):
        e = f.read(128)
        if e[56:128].decode("utf-16le").rstrip("\0") == "super":
            print(struct.unpack("<Q", e[32:40])[0]); break
    else:
        sys.exit("no super partition")
EOF
)
python3 -I "$here/lpunpack.py" "$img" "$start" "$tmp/parts" system >/dev/null
rm -f "$img"
sysimg=$tmp/parts/system.img
mkdir -p "$OUT/system/bin" "$OUT/system/lib64" "$tmp/apex"
# The linker and libc/libm/libdl are in the runtime APEX (a zip holding an ext4 image); the rest in /system/lib64.
debugfs -R "dump /system/apex/com.android.runtime.apex $tmp/runtime.apex" "$sysimg" 2>/dev/null
python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extract("apex_payload.img", sys.argv[2])' \
    "$tmp/runtime.apex" "$tmp/apex"
pay=$tmp/apex/apex_payload.img
debugfs -R "dump /bin/linker64 $OUT/system/bin/linker64" "$pay" 2>/dev/null
for l in libc libm libdl libdl_android; do
    debugfs -R "dump /lib64/bionic/$l.so $OUT/system/lib64/$l.so" "$pay" 2>/dev/null
done
for l in ld-android liblog libz libc++ libbase; do
    debugfs -R "dump /system/lib64/$l.so $OUT/system/lib64/$l.so" "$sysimg" 2>/dev/null
done
chmod 755 "$OUT/system/bin/linker64"
for f in "$OUT"/system/bin/linker64 "$OUT"/system/lib64/*.so; do
    [ -s "$f" ] || { echo "missing $f" >&2; exit 1; }
done
grep -m1 -o 'ro.build.version.release=[0-9]*\|ro.system.build.version.release=[0-9]*' \
    "$tmp/sdk/system-images/android-33/default/arm64-v8a/build.prop" || true
du -sh "$OUT"
