#!/bin/sh
# ctest helper (XBA-223): run_hello.sh NDS_RUN ROM WORKDIR interp|rc EXPECTED_PNG
#
# interp: ROM under nds_run (interpreter only) for 120 frames, the top screen compared with EXPECTED_PNG.
# rc:     ROM's ARM9 recompiled (python3 -m nds_tools.recomp, the host cc into a pack), with the discovery loop: run,
#         lift again with the addresses the interpreter had to run (--missed / --discovered), until a run finds none
#         new (4 rounds at most); then the last run's top screen compared with EXPECTED_PNG, and most instructions
#         must have run natively (the pack is built with -DNDS_RC_COUNT).
# Exits 77 (skipped) when ROM is absent (tools/consoles/build_homebrew.sh nds), or python3 / cc are missing.
run=$1
rom=$2
work=$3
mode=$4
expected=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ ! -f "$rom" ]; then
    echo "skipped: $rom not built (tools/consoles/build_homebrew.sh nds)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
if [ "$mode" = interp ]; then
    timeout 120 "$run" "$rom" --frames 120 --png "$work/top.png" --stats || exit 1
    exec "$py" "$here/png_compare.py" "$work/top.png" "$expected"
fi
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
: >"$work/discovered.txt"
round=1
while :; do
    PYTHONPATH="$repo/consoles/nds/tools" timeout 300 "$py" -m nds_tools.recomp "$rom" "$work/src" \
        --discovered "$work/discovered.txt" --no-comments --report "$work/report.json" >/dev/null || exit 1
    "$cc" -shared -fPIC -O1 -fno-tree-slp-vectorize -DNDS_RC_COUNT -I "$repo/consoles/nds/runtime" \
        -o "$work/libtitle.so" "$work"/src/*.c || exit 1
    out=$(timeout 120 "$run" "$rom" --frames 120 --pack "$work/libtitle.so" --png "$work/top.png" --stats \
        --missed "$work/missed.txt") || { echo "$out"; exit 1; }
    new=$(grep -v '^#' "$work/missed.txt" | sort -u | comm -23 - "$work/discovered.txt" | wc -l)
    echo "round $round: $(echo "$out" | grep '^stats'), $new new address(es)"
    [ "$new" -eq 0 ] || [ "$round" -ge 4 ] && break
    grep -v '^#' "$work/missed.txt" | cat - "$work/discovered.txt" | sort -u >"$work/d.tmp"
    mv "$work/d.tmp" "$work/discovered.txt"
    round=$((round + 1))
done
"$py" - "$work/report.json" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))
print("lifted: %d functions, %d instructions, %d handed to the interpreter, segments %s" % (
    r["functions"], r["instructions"], r["interp_fallback_insns"],
    ", ".join("%s+%d (%s)" % (s["vma"], s["size"], s["what"]) for s in r["image"]["segments"])))
EOF
native=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\1/p')
interp=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\2/p')
if [ -z "$native" ] || [ "$native" -le $((interp * 9)) ]; then
    echo "too little native code: native ${native:-?}, interpreted ${interp:-?} (want 90 % native)"
    exit 1
fi
exec "$py" "$here/png_compare.py" "$work/top.png" "$expected"
