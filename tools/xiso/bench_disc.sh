#!/usr/bin/env bash
# Read throughput of D: through the runtime's file layer (XBA-126): the same synthetic 64 MB tree as a folder, a
# plain image, a split image, a CSO v1 (deflate) and a CSO v2 (LZ4, as the Xbox tools write it).
#
#   tools/xiso/bench_disc.sh BUILD_DIR WORK_DIR
#
# BUILD_DIR holds kernel_disc_bench (cmake -S tests/kernel_disc -B BUILD_DIR && cmake --build BUILD_DIR). WORK_DIR
# gets the tree and the images (about 400 MB). Half the bytes are random (stored blocks in a CSO), half compressible
# text. Prints a Markdown table.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$1
WORK=$2
mkdir -p "$WORK/game/bench" "$WORK/save"
python3 - "$WORK" <<'EOF'
import os, random, sys
w = sys.argv[1]
rnd = random.Random(126)
words = [b"the", b"quick", b"brown", b"fox", b"jumps", b"over", b"lazy", b"dog", b"texture", b"level", b"\x00\x00\x00"]
for i in (1, 2):
    with open(os.path.join(w, "game", "bench", "random%d.bin" % i), "wb") as f:
        f.write(os.urandom(16 << 20))
    out = bytearray()
    while len(out) < 16 << 20:
        out += rnd.choice(words) + b" " + bytes([rnd.getrandbits(8)])
    with open(os.path.join(w, "game", "bench", "text%d.bin" % i), "wb") as f:
        f.write(out[:16 << 20])
EOF
cp "$ROOT/tools/conformance/test.xbe" "$WORK/game/default.xbe"
cd "$ROOT"
python3 tools/xiso/mkxiso.py "$WORK/game" "$WORK/bench.iso" >/dev/null
python3 tools/xiso/mkcso.py "$WORK/bench.iso" "$WORK/bench_v1.cso" --format cso1 >/dev/null
python3 tools/xiso/mkcso.py "$WORK/bench.iso" "$WORK/bench_v2.cso" --format cso2 >/dev/null
python3 tools/xiso/mkcso.py "$WORK/bench.iso" "$WORK/split.iso" --split $((24 << 20)) >/dev/null
ls -l "$WORK"/*.iso "$WORK"/*.cso | awk '{print "  " $5 "  " $9}' >&2
"$BUILD/kernel_disc_bench" "$WORK/save" "$WORK/game" "$WORK/bench.iso" "$WORK/split.1.iso" "$WORK/bench_v1.cso" \
    "$WORK/bench_v2.cso" 2>/dev/null
