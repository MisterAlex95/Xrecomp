#!/bin/sh
# ctest helper (XBA-150 / XBA-151 / XBA-152): recomp_rom.sh GBA_RUN ROM WORKDIR [gba_run args...]
#
# Builds ROM's title pack with the discovery loop (tools/consoles/gba_recomp_build.py: gba_tools.recomp, the host
# cc, runs until no new address shows up), then runs ROM under gba_run --pack --lockstep: the recompiled machine
# and an interpreter-only one side by side, the whole state (CPU, cycles, memories, frame) compared after every
# frame. Passes when the run agrees frame for frame and gba_run's own verdict (--result / --reg-result) passes.
# Exits 77 (skipped) when ROM or a --bios file is absent, or when python3 / cc are missing.
run=$1
rom=$2
work=$3
shift 3
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ ! -f "$rom" ]; then
    echo "skipped: $rom not built"
    exit 77
fi
prev=
for a in "$@"; do
    if [ "$prev" = "--bios" ] && [ ! -f "$a" ]; then
        echo "skipped: BIOS $a not built"
        exit 77
    fi
    prev=$a
done
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
command -v "${CC:-cc}" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
rm -rf "$work"
timeout 900 "$py" "$repo/tools/consoles/gba_recomp_build.py" "$rom" "$work" --count --loop 4 --gba-run "$run" \
    --run-args "$*" || exit 1
echo "interpreter:"
timeout 300 "$run" "$rom" --quiet --hash "$@" | grep -v "table\["
echo "recompiled (lockstep with the interpreter):"
out=$(timeout 600 "$run" "$rom" --quiet --hash "$@" --pack "$work/libtitle.so" --lockstep --stats)
rc=$?
echo "$out" | grep -v "table\["
echo "$out" | grep -q "^LOCKSTEP OK" || exit 1
exit $rc
