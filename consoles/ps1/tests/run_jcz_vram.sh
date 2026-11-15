#!/bin/sh
# ctest helper (XBA-200): run_jcz_vram.sh PS1_RUN BIOS EXE REF.png WORKDIR [compare_vram.py options...]. Runs one of
# JaCzekanski's ps1-tests GPU programs (tools/consoles/fetch_ps1_tests.sh) for 120 frames under ps1_run, writes the
# whole VRAM (ps1_run --vram) and compares it with the reference VRAM dump the release ships next to the program
# (made on the hardware), 5 bits per channel (compare_vram.py; options: --region / --ignore rectangles). Exits 77
# (skipped) when the EXE, its reference or the BIOS is not there.
run=$1
bios=$2
exe=$3
ref=$4
work=$5
shift 5
if [ "$bios" != hle ] && [ ! -f "$bios" ]; then
    echo "skipped: BIOS $bios not built (tools/consoles/build_ps1_bios.sh)"
    exit 77
fi
if [ ! -f "$exe" ] || [ ! -f "$ref" ]; then
    echo "skipped: $exe or $ref not fetched (tools/consoles/fetch_ps1_tests.sh)"
    exit 77
fi
mkdir -p "$work" || exit 1
out=$work/vram.ppm
rm -f "$out"
timeout 150 "$run" --bios "$bios" --exe "$exe" --frames 120 --quiet --vram "$out" >"$work/tty.txt" 2>&1
rc=$?
if [ $rc -ne 0 ] || [ ! -f "$out" ]; then
    cat "$work/tty.txt"
    echo "FAIL: ps1_run exit $rc"
    exit 1
fi
exec python3 "$(dirname "$0")/compare_vram.py" "$ref" "$out" "$@"
