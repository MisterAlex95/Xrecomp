#!/bin/sh
# ctest helper (XBA-160 / XBA-161 / XBA-162): recomp_exe.sh PS1_RUN BIOS INPUT WORKDIR FRAMES [ps1_run args...]
#
# Builds INPUT's title pack (a PS-X EXE or a disc image) with the discovery loop (tools/consoles/ps1_recomp_build.py:
# ps1_tools.recomp, the host cc at -O1, runs until no new code is recorded), then runs INPUT under ps1_run --pack
# --lockstep: the recompiled machine and an interpreter-only one side by side, the whole state (CPU, cycles,
# scheduler, RAM, scratchpad, GTE, VRAM) compared after every frame. Passes when the run agrees frame for frame and
# ps1_run's own verdict (--result / --until-tty) passes. Prints the interpreter's and the recompiled run's speed and
# the native share. Exits 77 (skipped) when INPUT, a --disc file or the BIOS is absent, or python3 / cc are missing.
run=$1
bios=$2
input=$3
work=$4
frames=$5
shift 5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ "$bios" != hle ] && [ ! -f "$bios" ]; then
    echo "skipped: BIOS $bios not built (tools/consoles/build_ps1_bios.sh)"
    exit 77
fi
if [ ! -f "$input" ]; then
    echo "skipped: $input not there"
    exit 77
fi
prev=
for a in "$@"; do
    if [ "$prev" = "--disc" ] && [ ! -f "$a" ]; then
        echo "skipped: disc $a not built"
        exit 77
    fi
    prev=$a
done
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
command -v "${CC:-cc}" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
rm -rf "$work"
args="--bios $bios $*"
timeout 1200 "$py" "$repo/tools/consoles/ps1_recomp_build.py" "$input" "$work" --count --loop 5 --ps1-run "$run" \
    --frames "$frames" --run-args "$args" || exit 1
echo "interpreter:"
timeout 300 "$run" "$input" --bios "$bios" --frames "$frames" --hash "$@" 2>&1 | grep -v "table\[" | tail -4
echo "recompiled (lockstep with the interpreter):"
out=$(timeout 600 "$run" "$input" --bios "$bios" --frames "$frames" --hash "$@" --pack "$work/libtitle.so" --lockstep \
    --stats 2>&1)
rc=$?
echo "$out" | grep -v "table\[" | tail -6
echo "$out" | grep -q "^LOCKSTEP OK" || exit 1
echo "recompiled alone:"
timeout 300 "$run" "$input" --bios "$bios" --frames "$frames" --hash "$@" --pack "$work/libtitle.so" 2>&1 \
    | grep -v "table\[" | tail -3
exit $rc
