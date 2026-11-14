#!/bin/sh
# ctest helper (XBA-159 / XBA-168): run_chd.sh PS1_RUN BIOS DISC.bin CODEC OUT.chd [ps1_run args...]. Writes the raw
# data track DISC.bin (our homebrew's disc) as a CHD v5 with ps1_tools.chd (consoles/ps1/tools: the pure-Python reader's
# own writer, chdman's shape: Huffman-coded map, cdzl / cdlz hunks) and boots it under ps1_run, which reads it through
# libchdr: the two CHD readers, the Prepare's and the player's, agree on the same image. Exits 77 (skipped) when the
# BIOS, the disc or python3 is not there.
run=$1
bios=$2
bin=$3
codec=$4
out=$5
shift 5
if [ "$bios" != hle ] && [ ! -f "$bios" ]; then
    echo "skipped: BIOS $bios not built (tools/consoles/build_ps1_bios.sh)"
    exit 77
fi
if [ ! -f "$bin" ]; then
    echo "skipped: $bin not built (tools/consoles/build_homebrew.sh ps1)"
    exit 77
fi
command -v python3 >/dev/null || {
    echo "skipped: no python3"
    exit 77
}
tools=$(cd "$(dirname "$0")/../tools" && pwd)
mkdir -p "$(dirname "$out")"
PYTHONPATH=$tools timeout 120 python3 -m ps1_tools.chd write --codec "$codec" "$out" "$bin" || exit 1
PYTHONPATH=$tools timeout 60 python3 -m ps1_tools.chd info "$out" || exit 1
exec timeout 150 "$run" --bios "$bios" "$out" "$@"
