#!/bin/sh
# ctest helper (XBA-270): run_msr.sh X360_RUN WORKDIR -- make_msr.py's two threads: with their mtmsrd sections no
# update is lost (exit code 0); without them (--no-msr) the scheduler splits load and store and updates are lost
# (a non-zero exit code: the test can fail). Exits 77 when python3 or cc is missing.
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
for variant in msr no-msr; do
    "$py" "$here/make_msr.py" "$work/$variant.xex" "--$variant" || exit 1
    rm -rf "$work/src"
    "$py" -m x360_tools.recomp "$work/$variant.xex" "$work/src" --no-comments >/dev/null || exit 1
    "$cc" -shared -fPIC -O1 -ffp-contract=off -DXE_RUNTIME=1 -I "$repo/consoles/x360/runtime" \
        -I "$repo/consoles/gc/runtime" -o "$work/$variant.so" "$work"/src/*.c -lm || exit 1
    timeout 120 "$run" "$work/$variant.xex" --pack "$work/$variant.so" --log "$work/$variant.log" --stats
    rc=$(sed -n 's/^x360_run: the title ended (exit code \([0-9]*\))/\1/p' "$work/$variant.log")
    [ -n "$rc" ] || { echo "$variant: the title did not end"; cat "$work/$variant.log"; exit 1; }
    echo "$variant: count - 100000 = $rc as a 32-bit word ($(grep '^stats' "$work/$variant.log"))"
    if [ "$variant" = msr ] && [ "$rc" != 0 ]; then
        echo "updates were lost inside mtmsrd sections"
        exit 1
    fi
    if [ "$variant" = no-msr ] && [ "$rc" = 0 ]; then
        echo "without mtmsrd no update was lost: the test does not exercise preemption"
        exit 1
    fi
done
echo "MSR[EE] sections are not preempted"
