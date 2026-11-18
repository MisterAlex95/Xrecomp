#!/bin/sh
# ctest helper (XBA-270): run_missed.sh X360_RUN WORKDIR -- the discovery loop on a title with a function only a
# pointer reaches (make_missed.py): the first recompiled run stops there and names it (--missed), recompiling with
# that address (--discovered) makes the title end with exit code 42. Exits 77 when python3 or cc is missing.
run=$1
work=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
export PYTHONPATH="$repo:$repo/consoles/x360/tools"
hidden=$("$py" "$here/make_missed.py" "$work/t.xex") || exit 1
: >"$work/discovered.txt"
for round in 1 2; do
    rm -rf "$work/src"
    "$py" -m x360_tools.recomp "$work/t.xex" "$work/src" --no-comments --discovered "$work/discovered.txt" \
        >"$work/recomp$round.txt" || { cat "$work/recomp$round.txt"; exit 1; }
    "$cc" -shared -fPIC -O1 -ffp-contract=off -DXE_RUNTIME=1 -I "$repo/consoles/x360/runtime" \
        -I "$repo/consoles/gc/runtime" -o "$work/libtitle.so" "$work"/src/*.c -lm || exit 1
    timeout 60 "$run" "$work/t.xex" --pack "$work/libtitle.so" --missed "$work/missed.txt" --log "$work/log$round.txt"
    rc=$?
    echo "round $round: exit $rc, $(grep -v '^#' "$work/missed.txt" | tr '\n' ' ')"
    if [ "$round" = 1 ]; then
        [ "$rc" = 1 ] || { echo "the first run should stop on the hidden function"; cat "$work/log1.txt"; exit 1; }
        grep -qx "$hidden" "$work/missed.txt" || { echo "the hidden function $hidden is not in --missed"; exit 1; }
        grep -v '^#' "$work/missed.txt" >"$work/discovered.txt"
    else
        [ "$rc" = 42 ] || { echo "the second run should end with exit code 42"; cat "$work/log2.txt"; exit 1; }
    fi
done
echo "discovery loop converged in 2 rounds (hidden function $hidden)"
