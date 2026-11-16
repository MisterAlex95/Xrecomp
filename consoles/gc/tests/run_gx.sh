#!/bin/sh
# ctest helper (XBA-262): run_gx.sh GC_RUN DOL WORKDIR EXPECTED_PNG [FIELDS] [GC_RUN ARGS...]
#
# Runs one of our GX test programs under gc_run with the GLES renderer on a headless EGL context (Mesa's llvmpipe:
# LIBGL_ALWAYS_SOFTWARE=1, so the picture does not depend on the machine's GPU) for FIELDS video fields (default 40),
# then compares the XFB with EXPECTED_PNG within gx_compare.py's tolerance. Each program also checks pixels itself
# through EFB peeks and draws a green square (bottom left) when they all passed, a red one otherwise, so a wrong
# reference cannot hide a wrong pixel there.
# Exits 77 (skipped) when DOL is absent (tools/consoles/build_homebrew.sh gc), python3 is missing, or there is no
# GLES 3 context to be had (gc_run exits 77). GX_UPDATE=1 rewrites EXPECTED_PNG from this run.
run=$1
dol=$2
work=$3
expected=$4
fields=${5:-40}
shift 4
[ $# -gt 0 ] && shift
here=$(cd "$(dirname "$0")" && pwd)
if [ ! -f "$dol" ]; then
    echo "skipped: $dol not built (tools/consoles/build_homebrew.sh gc)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
LIBGL_ALWAYS_SOFTWARE=1 timeout 300 "$run" "$dol" --frames "$fields" --gx --png "$work/xfb.png" --stats "$@"
rc=$?
[ $rc -eq 77 ] && exit 77
[ $rc -eq 0 ] || exit 1
if [ "${GX_UPDATE:-0}" = 1 ]; then
    exec "$py" "$here/gx_compare.py" "$work/xfb.png" "$expected" --update
fi
exec "$py" "$here/gx_compare.py" "$work/xfb.png" "$expected"
