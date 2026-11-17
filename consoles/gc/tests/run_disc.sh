#!/bin/sh
# ctest helper (XBA-261): GameCube disc images generated here, no game data.
#
#   run_disc.sh formats GC_RUN GC_DISC_INFO WORKDIR
#     python3 -m gc_tools.disc testset: a GCM of our DVD-interface test program (tools.ppc.asm, no toolchain) with
#     our apploader and test files, converted to ISO, CISO, GCZ, RVZ (zstd, LZMA, LZMA2, bzip2, none) and WIA (purge,
#     bzip2, LZMA2). Every image must give gc_disc_info's expected listing (title key from gc_disc and from
#     recomp_pack_rom_identify, DOL bytes, FST files); then the GCM and the zstd RVZ boot under gc_run (HLE apploader,
#     no IPL) and the program's DVD results must be right.
#   run_disc.sh hello GC_RUN HELLO_DOL WORKDIR EXPECTED_PNG
#     our libogc hello world packed into a GCM boots to the same XFB as the DOL (skipped, 77, without the corpus:
#     tools/consoles/build_homebrew.sh gc).
# A cross build's binaries run under the emulator ctest uses (XBR_TEST_EMULATOR, comma separated: qemu-aarch64
# for the arm64 CI); empty on a native build.
emu=$(printf '%s' "${XBR_TEST_EMULATOR:-}" | tr ',' ' ')
mode=$1
run=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
export PYTHONPATH="$repo/consoles/gc/tools"

if [ "$mode" = play ]; then
    # run_disc.sh play GC_PLAY HELLO_DOL WORKDIR EXPECTED_PNG: the hello world as a zstd RVZ through gc_runtime (the
    # app's runtime), headless: same XFB (run_play.sh)
    dol=$3
    work=$4
    if [ ! -f "$dol" ]; then
        echo "skipped: $dol not built (tools/consoles/build_homebrew.sh gc)"
        exit 77
    fi
    rm -rf "$work"
    timeout 120 "$py" -m gc_tools.disc testset "$work" --dol "$dol" || exit 1
    exec sh "$here/run_play.sh" "$run" "$work/test-zstd.rvz" "$work/play" "$5" --frames 120
fi

if [ "$mode" = hello ]; then
    dol=$3
    work=$4
    expected=$5
    if [ ! -f "$dol" ]; then
        echo "skipped: $dol not built (tools/consoles/build_homebrew.sh gc)"
        exit 77
    fi
    rm -rf "$work"
    timeout 120 "$py" -m gc_tools.disc testset "$work" --dol "$dol" || exit 1
    out=$(timeout 120 $emu "$run" "$work/test.gcm" --frames 120 --png "$work/xfb.png" 2>&1) || { echo "$out"; exit 1; }
    echo "$out" | grep '^gc: boot:'
    echo "$out" | grep -q 'main DOL entry' || { echo "no main DOL entry in the log"; exit 1; }
    exec "$py" "$repo/consoles/nds/tests/png_compare.py" "$work/xfb.png" "$expected"
fi

info=$3
work=$4
rm -rf "$work"
timeout 300 "$py" -m gc_tools.disc testset "$work" || exit 1
fail=0
for img in "$work"/test.gcm "$work"/test.iso "$work"/test.ciso "$work"/test.gcz "$work"/*.rvz "$work"/*.wia; do
    if timeout 60 $emu "$info" "$img" >"$img.txt" && cmp -s "$img.txt" "$work/expected.txt"; then
        echo "ok   $(basename "$img")"
    else
        echo "FAIL $(basename "$img")"
        diff "$work/expected.txt" "$img.txt" | head -20
        fail=1
    fi
done
[ $fail -eq 0 ] || exit 1
# the expected DVD results (gc_tools.disc di_program): read status / address / length, disc ID status, audio
# position, bad command status, its error, the cover, FST pointer, arena high, boot magic, game code, done marker
for img in test.gcm test-zstd.rvz; do
    out=$(timeout 60 $emu "$run" "$work/$img" --frames 30 --peek 80006000:13 --peek 80020000:4 --peek 80021000:8 \
        2>"$work/$img.log") || { cat "$work/$img.log"; echo "$out"; exit 1; }
    grep '^gc: boot:' "$work/$img.log"
    "$py" - "$work" "$out" <<'EOF' || exit 1
import os, struct, sys
work, out = sys.argv[1], sys.argv[2]
peeks = []
for line in out.splitlines():
    if line.startswith("peek "):
        peeks.append([int(x, 16) for x in line.split()[1:]])
    elif peeks and line and all(c in "0123456789ABCDEF " for c in line):
        peeks[-1] += [int(x, 16) for x in line.split()]
iso = open(os.path.join(work, "test.gcm"), "rb").read()
fst_off, fst_size, fst_max = struct.unpack(">III", iso[0x424:0x430])
fst_addr = (0x81800000 - fst_max) & ~31
data_off = struct.unpack(">I", iso[fst_off + 16:fst_off + 20])[0]  # entry 1: data.bin
n32 = (0x3010 + 31) & ~31
want = [0x10, 0x20000 + n32, 0, 0x10, 0, 0x04, 0x052000, 0, fst_addr, (fst_addr - 0x2000) & ~31, 0x0D15EA5E,
        struct.unpack(">I", iso[0:4])[0], 0xC0DEC0DE]
data = list(struct.unpack(">4I", iso[data_off:data_off + 16]))
disc_id = list(struct.unpack(">8I", iso[0:32]))
got = peeks
if got != [want, data, disc_id]:
    print("DVD results differ:\n got  %s\n want %s" % (got, [want, data, disc_id]))
    sys.exit(1)
print("ok   DVD results")
EOF
done
# multi-disc: a second disc (disc number 1: key ...-D2) swapped in at field 10; the cover opens, then closes on it
timeout 300 "$py" -m gc_tools.disc testset "$work/d2" --disc 1 || exit 1
for img in test.gcm test-zstd.rvz; do
    timeout 60 $emu "$info" "$work/d2/$img" 2>/dev/null | cmp -s - "$work/d2/expected.txt" \
        || { echo "FAIL disc 2 $img"; exit 1; }
done
grep -q '^key GXRE01-0-D2$' "$work/d2/expected.txt" || { echo "FAIL disc 2 key"; exit 1; }
timeout 60 $emu "$run" "$work/test.gcm" --disc2 "$work/d2/test-zstd.rvz" --swap-at 10 --frames 60 >/dev/null \
    2>"$work/swap.log" || { cat "$work/swap.log"; exit 1; }
grep '^gc: dvd:' "$work/swap.log"
grep -q 'cover closed on disc GXRE01-0-D2' "$work/swap.log" || { echo "FAIL disc change"; exit 1; }
echo "ok   disc change"
