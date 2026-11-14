#!/bin/sh
# ctest helper (XBA-152): run_rom.sh GBA_RUN ROM [gba_run args...]. Runs ROM under gba_run with a timeout, or
# exits 77 (skipped) when ROM, or a --bios file among the arguments, is not there: those are build outputs of
# tools/consoles/build_homebrew.sh, fetch_gba_tests.sh and build_gba_bios.sh, which need Docker or the network.
run=$1
rom=$2
shift 2
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
exec timeout 120 "$run" "$rom" --quiet "$@"
