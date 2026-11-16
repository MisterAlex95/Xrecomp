#!/bin/sh
# ctest helper (XBA-225): run_hello.sh GC_RUN DOL WORKDIR interp|rc EXPECTED_PNG
#
# interp: DOL under gc_run (interpreter only) for 120 fields, the XFB compared with EXPECTED_PNG.
# rc:     DOL's text recompiled (python3 -m gc_tools.recomp, the host cc into a pack, -ffp-contract=off like the
#         core; RC_FLAGS adds flags, e.g. -O2), with the discovery loop: run, lift again with the addresses the
#         interpreter had to start at (--missed / --discovered), until a run finds none new (4 rounds at most); then
#         the last run's XFB compared with EXPECTED_PNG, and most instructions must have run natively (the pack is
#         built with -DGC_RC_COUNT).
# The comparison is consoles/nds/tests/png_compare.py (stdlib only, shared with the DS spike).
# Exits 77 (skipped) when DOL is absent (tools/consoles/build_homebrew.sh gc), or python3 / cc are missing.
run=$1
dol=$2
work=$3
mode=$4
expected=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
compare="$repo/consoles/nds/tests/png_compare.py"
if [ ! -f "$dol" ]; then
    echo "skipped: $dol not built (tools/consoles/build_homebrew.sh gc)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
if [ "$mode" = interp ]; then
    timeout 120 "$run" "$dol" --frames 120 --png "$work/xfb.png" --stats || exit 1
    exec "$py" "$compare" "$work/xfb.png" "$expected"
fi
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
jobs=${JOBS:-4}
: >"$work/discovered.txt"
round=1
while :; do
    PYTHONPATH="$repo/consoles/gc/tools" timeout 300 "$py" -m gc_tools.recomp "$dol" "$work/src" \
        --discovered "$work/discovered.txt" --no-comments --report "$work/report.json" >/dev/null || exit 1
    rm -f "$work"/src/*.o
    # one object per file, in parallel; RC_FLAGS / CC from the environment
    (cd "$work/src" && ls *.c | xargs -P "$jobs" -I{} "$cc" -c -fPIC -O1 -ffp-contract=off ${RC_FLAGS:-} \
        -DGC_RC_COUNT -I "$repo/consoles/gc/runtime" -o {}.o {}) || exit 1
    "$cc" -shared -o "$work/libtitle.so" "$work"/src/*.o -lm || exit 1
    out=$(timeout 120 "$run" "$dol" --frames 120 --pack "$work/libtitle.so" --png "$work/xfb.png" --stats \
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
print("lifted: %d functions, %d of %d text instructions, %d handed to the interpreter, %d resume points" % (
    r["functions"], r["instructions"], r["text_instructions"], r["interp_fallback_insns"], r["table_entries"]))
EOF
native=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\1/p')
interp=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\2/p')
if [ -z "$native" ] || [ "$native" -le $((interp * 9)) ]; then
    echo "too little native code: native ${native:-?}, interpreted ${interp:-?} (want 90 % native)"
    exit 1
fi
exec "$py" "$compare" "$work/xfb.png" "$expected"
