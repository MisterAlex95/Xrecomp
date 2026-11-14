#!/bin/sh
# ctest helper (XBA-162): run_exe.sh PS1_RUN BIOS FILE [ps1_run args...]. Runs FILE (an .exe or a disc image) under
# ps1_run with the BIOS and a timeout, or exits 77 (skipped) when FILE, the BIOS or a --disc file among the arguments
# is not there: they are build outputs of tools/consoles/build_homebrew.sh and build_ps1_bios.sh, which need Docker.
run=$1
bios=$2
file=$3
shift 3
if [ "$bios" != hle ] && [ ! -f "$bios" ]; then
    echo "skipped: BIOS $bios not built (tools/consoles/build_ps1_bios.sh)"
    exit 77
fi
if [ ! -f "$file" ]; then
    echo "skipped: $file not built"
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
exec timeout 150 "$run" --bios "$bios" "$file" "$@"
