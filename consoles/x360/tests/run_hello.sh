#!/bin/sh
# ctest helper (XBA-270): run_hello.sh X360_RUN WORKDIR MODE EXPECTED_TXT [ELF]
#
# Builds the synthetic test title (make_hello.py: our own PowerPC assembly in a synthetic XEX2), recompiles it
# (python3 -m x360_tools.recomp), builds the C into a pack (the host cc, -DXE_RUNTIME -shared -ffp-contract=off) and
# runs it under x360_run; what the title prints through DbgPrint must equal EXPECTED_TXT and the title must end with
# exit code 0, every instruction run natively or by the small interpreter, no import without an HLE but the one the
# title never calls. The title reads a file and lists the root of its disc: a directory made here (modes basic and c)
# or an XDVDFS image of it (mode lzx, tools/xiso/mkxiso.py). MODE:
#   basic   the XEX as the loader takes it (unencrypted, basic compression)
#   lzx     encrypted (a key made up here) and LZX-compressed, then rewritten plain by python3 -m tools.xex --plain,
#           the path a retail title takes with the user's own key
#   c       ELF: a C test title built with devkitPPC (tests/homebrew, tools/consoles/build_homebrew.sh x360) packed by
#           homebrew/elf2xex.py; skipped (77) when the ELF is not built
# Exits 77 (skipped) when python3 or cc is missing.
run=$1
work=$2
mode=$3
expected=$4
elf=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
export PYTHONPATH="$repo:$repo/consoles/x360/tools"
if [ "$mode" = c ] && [ ! -f "$elf" ]; then
    echo "skipped: $elf not built (tools/consoles/build_homebrew.sh x360)"
    exit 77
fi
if [ "$mode" = c ]; then
    "$py" "$here/homebrew/elf2xex.py" "$elf" "$work/hello.xex" || exit 1
elif [ "$mode" = lzx ]; then
    printf 'xba-270-made-up!' >"$work/test.key"
    "$py" "$here/make_hello.py" "$work/enc.xex" --compression normal --key-file "$work/test.key" || exit 1
    "$py" -m tools.xex "$work/enc.xex" --key-file "$work/test.key" --plain "$work/hello.xex" >"$work/xex.txt" || exit 1
    grep -q "compression normal (LZX)" "$work/xex.txt" || { echo "the test XEX is not LZX-compressed"; exit 1; }
else
    "$py" "$here/make_hello.py" "$work/hello.xex" --compression basic || exit 1
fi
mkdir -p "$work/disc/data"
printf 'Hello from the disc!' >"$work/disc/data/hello.txt"
printf 'binary data' >"$work/disc/data/b.bin"
printf 'readme\n' >"$work/disc/readme.txt"
disc=$work/disc
if [ "$mode" = lzx ]; then
    "$py" "$repo/tools/xiso/mkxiso.py" "$work/disc" "$work/disc.iso" >/dev/null || exit 1
    disc=$work/disc.iso
fi
timeout 300 "$py" -m x360_tools.recomp "$work/hello.xex" "$work/src" --no-comments >"$work/recomp.txt" || {
    cat "$work/recomp.txt"; exit 1; }
cat "$work/recomp.txt"
"$cc" -shared -fPIC -O1 -ffp-contract=off -DXE_RUNTIME=1 -I "$repo/consoles/x360/runtime" \
    -I "$repo/consoles/gc/runtime" -o "$work/libtitle.so" "$work"/src/*.c -lm || exit 1
timeout 120 "$run" "$work/hello.xex" --pack "$work/libtitle.so" --disc "$disc" --dbg "$work/dbg.txt" --log "$work/log.txt" \
    --stats --missed "$work/missed.txt"
rc=$?
cat "$work/log.txt"
if [ "$rc" -ne 0 ]; then
    echo "x360_run exited with $rc"
    exit 1
fi
if grep -q "has no HLE" "$work/log.txt"; then
    echo "an import without an HLE was called"
    exit 1
fi
if cmp -s "$work/dbg.txt" "$expected"; then
    echo "DbgPrint output matches $(basename "$expected") ($(wc -l <"$expected") lines)"
    exit 0
fi
echo "DbgPrint output differs from $expected:"
diff "$expected" "$work/dbg.txt" | head -n 40
exit 1
