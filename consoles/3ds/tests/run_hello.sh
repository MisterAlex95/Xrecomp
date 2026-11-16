#!/bin/sh
# ctest helper (XBA-228): run_hello.sh CTR_RUN PROG.3dsx WORKDIR interp|rc EXPECTED_PNG
#
# interp: PROG under ctr_run (interpreter only) for 60 frames, the top screen compared with EXPECTED_PNG.
# rc:     PROG's code recompiled (python3 -m ctr_tools.recomp, the host cc into a pack), with the discovery loop: run,
#         lift again with the addresses the interpreter had to run (--missed / --discovered), until a run finds none
#         new (4 rounds at most); then the last run's top screen compared with EXPECTED_PNG, and most instructions
#         must have run natively (the pack is built with -DCTR_RC_COUNT).
# The picture comparison is the DS spike's (consoles/nds/tests/png_compare.py).
# Exits 77 (skipped) when PROG is absent (tools/consoles/build_homebrew.sh 3ds), or python3 / cc are missing.
run=$1
prog=$2
work=$3
mode=$4
expected=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
compare="$repo/consoles/nds/tests/png_compare.py"
if [ ! -f "$prog" ]; then
    echo "skipped: $prog not built (tools/consoles/build_homebrew.sh 3ds)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
if [ "$mode" = interp ]; then
    timeout 120 "$run" "$prog" --frames 60 --png "$work/top.png" --stats || exit 1
    exec "$py" "$compare" "$work/top.png" "$expected"
fi
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
: >"$work/discovered.txt"
round=1
while :; do
    PYTHONPATH="$repo/consoles/3ds/tools" timeout 300 "$py" -m ctr_tools.recomp "$prog" "$work/src" \
        --discovered "$work/discovered.txt" --no-comments --report "$work/report.json" >/dev/null || exit 1
    "$cc" -shared -fPIC -O1 -fno-tree-slp-vectorize -ffp-contract=off -DCTR_RC_COUNT \
        -I "$repo/consoles/3ds/runtime" -o "$work/libtitle.so" "$work"/src/*.c || exit 1
    out=$(timeout 120 "$run" "$prog" --frames 60 --pack "$work/libtitle.so" --png "$work/top.png" --stats \
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
print("lifted: %d functions, %d instructions, %d handed to the interpreter, entries %s, relocations %d" % (
    r["functions"], r["instructions"], r["interp_fallback_insns"],
    ", ".join("%s %d" % kv for kv in sorted(r["entries_by_origin"].items())), r["image"]["relocations"]))
EOF
native=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\1/p')
interp=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\2/p')
if [ -z "$native" ] || [ "$native" -le $((interp * 9)) ]; then
    echo "too little native code: native ${native:-?}, interpreted ${interp:-?} (want 90 % native)"
    exit 1
fi
exec "$py" "$compare" "$work/top.png" "$expected"
