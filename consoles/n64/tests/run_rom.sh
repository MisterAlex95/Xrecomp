#!/bin/sh
# ctest helper (XBA-171): run_rom.sh N64_RUN ROM [n64_run args...]. Runs ROM under n64_run with a timeout, or exits 77
# (skipped) when ROM is not there: it is a build output of tools/consoles/build_homebrew.sh, which needs Docker.
run=$1
rom=$2
shift 2
if [ ! -f "$rom" ]; then
    echo "skipped: $rom not built (tools/consoles/build_homebrew.sh n64)"
    exit 77
fi
exec timeout 150 "$run" "$rom" "$@"
