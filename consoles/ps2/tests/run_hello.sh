#!/bin/sh
# ctest helper (XBA-226): run_hello.sh PS2_RUN ELF WORKDIR interp|rc EXPECTED_TXT
#
# interp: ELF under ps2_run (interpreter only) until it exits, its SIO output compared with EXPECTED_TXT.
# rc:     ELF recompiled (python3 -m ps2_tools.recomp, the host cc into a pack, -ffp-contract=off like the core), with
#         the discovery loop: run, lift again with the addresses the interpreter had to start at (--missed /
#         --discovered), until a run finds none new (4 rounds at most); then the last run's SIO output compared with
#         EXPECTED_TXT, no pack entry may be stale, and most instructions must have run natively (the pack is built
#         with -DPS2_RC_COUNT).
# Exits 77 (skipped) when ELF is absent (tools/consoles/build_homebrew.sh ps2), or python3 / cc are missing.
run=$1
elf=$2
work=$3
mode=$4
expected=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ ! -f "$elf" ]; then
    echo "skipped: $elf not built (tools/consoles/build_homebrew.sh ps2)"
    exit 77
fi
rm -rf "$work"
mkdir -p "$work"

compare() {
    if cmp -s "$1" "$expected"; then
        echo "SIO output matches $(basename "$expected") ($(wc -l <"$expected") lines)"
        return 0
    fi
    echo "SIO output differs from $expected:"
    diff "$expected" "$1" | head -n 40
    return 1
}

if [ "$mode" = interp ]; then
    timeout 120 "$run" "$elf" --sio "$work/sio.txt" --stats || exit 1
    compare "$work/sio.txt"
    exit $?
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
: >"$work/discovered.txt"
round=1
while :; do
    PYTHONPATH="$repo/consoles/ps2/tools" timeout 300 "$py" -m ps2_tools.recomp "$elf" "$work/src" \
        --discovered "$work/discovered.txt" --no-comments --report "$work/report.json" >/dev/null || exit 1
    "$cc" -shared -fPIC -O1 -ffp-contract=off -DPS2_RC_COUNT -I "$repo/consoles/ps2/runtime" \
        -o "$work/libtitle.so" "$work"/src/*.c || exit 1
    out=$(timeout 120 "$run" "$elf" --pack "$work/libtitle.so" --sio "$work/sio.txt" --stats \
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
print("lifted: %d functions, %d instructions, %d handed to the interpreter, code %s-%s" % (
    r["functions"], r["instructions"], r["interp_fallback_insns"], r["elf"]["code"][0], r["elf"]["code"][1]))
print("MMI / COP2 lifted: %s" % (", ".join("%s %d" % kv for kv in r["mmi_and_cop2"].items()) or "none"))
EOF
stale=$(echo "$out" | sed -n 's/^stats: .* stale \([0-9]*\).*/\1/p')
if [ "${stale:-1}" != 0 ]; then
    echo "pack entries do not match the loaded executable: stale ${stale:-?} (ps2_load.c and ps2_tools.elf disagree)"
    exit 1
fi
native=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\1/p')
interp=$(echo "$out" | sed -n 's/^stats: native \([0-9]*\) interpreted \([0-9]*\).*/\2/p')
if [ -z "$native" ] || [ "$native" -le $((interp * 9)) ]; then
    echo "too little native code: native ${native:-?}, interpreted ${interp:-?} (want 90 % native)"
    exit 1
fi
compare "$work/sio.txt"
