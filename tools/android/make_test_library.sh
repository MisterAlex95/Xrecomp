#!/usr/bin/env bash
# A synthetic game library for the launcher's « Game library » (docs/android/app-shell.md): disc images built with
# tools/xiso/mkxiso.py and game folders, all around the synthetic XBE of tools/conformance (ours: five leaf
# functions, no game content) with patched title IDs, names, regions and versions. Nothing of a real game.
#
#   tools/android/make_test_library.sh OUT_DIR [COUNT]
#
# OUT_DIR gets (COUNT images, default 6):
#   Sample Game 01.iso ... Sample Game NN.iso   bare XDVDFS images (80 KB each), title IDs 58420001..
#   Sample Game 02 (v1.01).iso                  a second version of game 02 (same title ID): « 2 versions »
#   Sample Folder Game/default.xbe              a game folder (played in place with All files access)
#   nested/deeper/Sample Nested Game.iso        found up to four levels down
#   notes.iso                                   not an Xbox image: the « Other files » line
#   .hidden/Hidden Game.iso, Android/x.iso      skipped by the scan
# Then:
#   adb push OUT_DIR /sdcard/xbox-test   and choose it as the library folder (Source › Game library folder)
#
# The images hold a default.xbe and a small data file only: they import in a second; the games do not run (the
# synthetic XBE is not a game), which is enough for the library's screens, the import, the queue and Storage.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?usage: make_test_library.sh OUT_DIR [COUNT]}
COUNT=${2:-6}
PY=${PYTHON:-python3}
XBE="$ROOT/tools/conformance/test.xbe"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# patch_xbe OUT TID NAME REGION VERSION: the conformance XBE with its certificate's title ID (+0x08), name (+0x0C,
# 40 UTF-16LE units), game region (+0xA0) and version (+0xAC) set.
patch_xbe() {
    "$PY" - "$XBE" "$@" <<'PYEOF'
import struct, sys
src, out, tid, name, region, version = sys.argv[1:7]
b = bytearray(open(src, "rb").read())
base, = struct.unpack_from("<I", b, 0x104)
cert = struct.unpack_from("<I", b, 0x118)[0] - base
struct.pack_into("<I", b, cert + 0x08, int(tid, 16))
n = name.encode("utf-16-le")[:80]
b[cert + 0x0C:cert + 0x0C + 80] = n + b"\0" * (80 - len(n))
struct.pack_into("<I", b, cert + 0xA0, int(region, 0))
struct.pack_into("<I", b, cert + 0xAC, int(version, 0))
open(out, "wb").write(bytes(b))
PYEOF
}

# image OUT.iso TID NAME REGION VERSION [mkxiso options]
image() {
    local out=$1 tid=$2 name=$3 region=$4 version=$5
    shift 5
    local src="$WORK/src-$tid-$RANDOM"
    mkdir -p "$src/media"
    patch_xbe "$src/default.xbe" "$tid" "$name" "$region" "$version"
    printf 'synthetic data for %s\n' "$name" > "$src/media/data.bin"
    mkdir -p "$(dirname "$out")"
    "$PY" "$ROOT/tools/xiso/mkxiso.py" "$src" "$out" "$@" >/dev/null
}

for i in $(seq 1 "$COUNT"); do
    n=$(printf '%02d' "$i")
    tid=$(printf '584200%02X' "$i")
    region=$(( i % 3 == 0 ? 1 : i % 3 == 1 ? 4 : 7 ))
    image "$OUT/Sample Game $n.iso" "$tid" "Sample Game $n" "$region" 0x100
done
if [ "$COUNT" -ge 2 ]; then
    image "$OUT/Sample Game 02 (v1.01).iso" 58420002 "Sample Game 02" 4 0x101
fi
image "$OUT/nested/deeper/Sample Nested Game.iso" 584200A0 "Sample Nested Game" 4 0x100
mkdir -p "$OUT/Sample Folder Game/media"
patch_xbe "$OUT/Sample Folder Game/default.xbe" 584200F0 "Sample Folder Game" 4 0x100
printf 'synthetic data\n' > "$OUT/Sample Folder Game/media/data.bin"
printf 'not an Xbox image\n' > "$OUT/notes.iso"
image "$OUT/.hidden/Hidden Game.iso" 584200E0 "Hidden Game" 4 0x100
image "$OUT/Android/x.iso" 584200E1 "Skipped Game" 4 0x100

echo "$OUT: $(find "$OUT" -name '*.iso' | wc -l | tr -d ' ') images, 1 game folder"
