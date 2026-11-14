#!/bin/sh
# ctest helper (XBA-170 / XBA-171): recomp_systemtest.sh N64_RUN ROM KNOWN HAVE_RCP WORKDIR
#
# lemmy-64/n64-systemtest recompiled: its pack is built with the discovery loop (tools/consoles/n64_recomp_build.py),
# then the suite runs under n64_run --pack (run_systemtest.sh: the same pass / known-gap set as the interpreter must
# come out), and once more in lockstep with the interpreter (the whole state compared after every frame). The suite
# raises every exception the VR4300 has, from lifted code too (overflow, traps, address errors, TLB misses,
# coprocessor unusable, FPE / unimplemented operation, in and out of delay slots). Exits 77 when the ROM is not built
# (tools/consoles/build_n64_systemtest.sh) or python3 / cc are missing.
run=$1
rom=$2
known=$3
rcp=$4
work=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ ! -f "$rom" ]; then
    echo "skipped: $rom not built (tools/consoles/build_n64_systemtest.sh)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
command -v "${CC:-cc}" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
if [ ! -f "$work/libtitle.so" ] || [ "$rom" -nt "$work/libtitle.so" ] || [ -n "$N64_RC_REBUILD" ]; then
    rm -rf "$work"
    timeout 3000 "$py" "$repo/tools/consoles/n64_recomp_build.py" "$rom" "$work" --count --loop 4 --n64-run "$run" \
        --frames 1200 || exit 1
fi
echo "recompiled:"
sh "$here/run_systemtest.sh" "$run" "$rom" "$known" "$rcp" --pack "$work/libtitle.so" || exit 1
echo "lockstep with the interpreter:"
out=$(timeout 600 "$run" "$rom" --frames 1200 --quiet --pack "$work/libtitle.so" --lockstep --stats 2>&1)
echo "$out" | grep -E "^LOCKSTEP|^recomp:"
echo "$out" | grep -q "^LOCKSTEP OK" || exit 1
exit 0
