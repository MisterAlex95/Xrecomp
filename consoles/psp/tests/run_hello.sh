#!/bin/sh
# ctest helper (XBA-224): run_hello.sh PSP_RUN MODULE WORKDIR interp|rc EXPECTED_PNG
#
# interp: MODULE under psp_run (interpreter only) until it exits (sceKernelExitGame after 30 VBlanks; 120 frames at
#         most), the framebuffer compared with EXPECTED_PNG.
# rc:     MODULE recompiled (python3 -m psp_tools.recomp, the host cc into a pack, -ffp-contract=off like the core),
#         with the discovery loop: run, lift again with the addresses the interpreter had to start at (--missed /
#         --discovered), until a run finds none new (4 rounds at most); then the last run's framebuffer compared with
#         EXPECTED_PNG, and most instructions must have run natively (the pack is built with -DPSP_RC_COUNT).
# Exits 77 (skipped) when MODULE is absent (tools/consoles/build_homebrew.sh psp), or python3 / cc are missing.
run=$1
mod=$2
work=$3
mode=$4
expected=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ ! -f "$mod" ]; then
    echo "skipped: $mod not built (tools/consoles/build_homebrew.sh psp)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
if [ "$mode" = interp ]; then
    timeout 120 "$run" "$mod" --frames 120 --png "$work/screen.png" --stats || exit 1
    exec "$py" "$here/png_compare.py" "$work/screen.png" "$expected"
fi
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
: >"$work/discovered.txt"
round=1
while :; do
    PYTHONPATH="$repo/consoles/psp/tools" timeout 300 "$py" -m psp_tools.recomp "$mod" "$work/src" \
        --discovered "$work/discovered.txt" --no-comments --report "$work/report.json" >/dev/null || exit 1
    "$cc" -shared -fPIC -O1 -ffp-contract=off -DPSP_RC_COUNT -I "$repo/consoles/psp/runtime" \
        -o "$work/libtitle.so" "$work"/src/*.c || exit 1
    out=$(timeout 120 "$run" "$mod" --frames 120 --pack "$work/libtitle.so" --png "$work/screen.png" --stats \
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
print("lifted: %d functions, %d instructions, %d handed to the interpreter, %d VFPU, module %s code %s-%s" % (
    r["functions"], r["instructions"], r["interp_fallback_insns"], r["vfpu_insns"], r["module"]["name"],
    r["module"]["code"][0], r["module"]["code"][1]))
EOF
stale=$(echo "$out" | sed -n 's/^stats: .* stale \([0-9]*\).*/\1/p')
if [ "${stale:-1}" != 0 ]; then
    echo "pack entries do not match the loaded module: stale ${stale:-?} (psp_load.c and psp_tools.prx disagree)"
    exit 1
fi
native=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\1/p')
interp=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\2/p')
if [ -z "$native" ] || [ "$native" -le $((interp * 9)) ]; then
    echo "too little native code: native ${native:-?}, interpreted ${interp:-?} (want 90 % native)"
    exit 1
fi
exec "$py" "$here/png_compare.py" "$work/screen.png" "$expected"
