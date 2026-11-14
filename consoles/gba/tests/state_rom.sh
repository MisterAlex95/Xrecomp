#!/bin/sh
# ctest helper (XBA-182): state_rom.sh GBA_RUN ROM WORKDIR MODE [gba_run args...]
#
# The save state round trip on one ROM: run N + M frames saving a state after frame N, hashing the picture and the
# audio of the M frames after it (gba_run --state-save N:FILE --run-hash); then load that state into a new machine
# and run M frames: the hash must be the same. A run of M frames from power-on must differ (the state mattered).
#   MODE interp   the interpreter alone
#   MODE pack     also with the ROM's title pack (built with the discovery loop as recomp_rom.sh does): the state
#                 saved by the recompiled run loads in the interpreter and the other way round, same hash each time,
#                 and the recompiled runs from the state stay in lockstep with an interpreter twin
#   MODE other:ROM2  the state of ROM loaded into ROM2 must be refused (exit 5)
# Exits 77 (skipped) when ROM is absent (or ROM2, python3, cc for the modes that need them).
run=$1
rom=$2
work=$3
mode=$4
shift 4
N=${STATE_N:-120}
M=${STATE_M:-180}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
[ -f "$rom" ] || { echo "skipped: $rom not built"; exit 77; }
prev=
for a in "$@"; do
    if [ "$prev" = "--bios" ] && [ ! -f "$a" ]; then
        echo "skipped: BIOS $a not built"
        exit 77
    fi
    prev=$a
done
rm -rf "$work"
mkdir -p "$work"
st=$work/state.st
hash_of() { echo "$1" | sed -n 's/^run-hash \([0-9a-f]*\) frames .*/\1/p'; }

a=$(timeout 300 "$run" "$rom" --quiet --frames $((N + M)) --state-save "$N:$st" --run-hash "$@") || exit 1
ha=$(hash_of "$a")
[ -n "$ha" ] && [ -s "$st" ] || { echo "FAIL: no hash or no state: $a"; exit 1; }
echo "saved after frame $N, frames $((N + 1))..$((N + M)): $ha"

case "$mode" in
other:*)
    rom2=${mode#other:}
    [ -f "$rom2" ] || { echo "skipped: $rom2 not built"; exit 77; }
    out=$(timeout 60 "$run" "$rom2" --quiet --frames 10 --state-load "$st" "$@")
    rc=$?
    echo "$out"
    [ $rc -eq 5 ] && echo "$out" | grep -q "STATE REFUSED" || { echo "FAIL: another game's state was not refused"; exit 1; }
    echo "OK: refused"
    exit 0
    ;;
esac

check() { # label, then gba_run args
    label=$1
    shift
    out=$(timeout 300 "$run" "$rom" --quiet --frames $M --run-hash "$@") || { echo "FAIL ($label): $out"; exit 1; }
    h=$(hash_of "$out")
    echo "$label: $h"
    [ "$h" = "$ha" ] || { echo "FAIL ($label): $h != $ha"; echo "$out"; exit 1; }
}
check "loaded, interpreter" --state-load "$st" "$@"
cold=$(hash_of "$(timeout 300 "$run" "$rom" --quiet --frames $M --run-hash "$@")")
echo "from power-on: $cold"
[ "$cold" != "$ha" ] || { echo "FAIL: the state changed nothing"; exit 1; }

if [ "$mode" = pack ]; then
    py=${PYTHON:-python3}
    command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
    command -v "${CC:-cc}" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
    timeout 900 "$py" "$repo/tools/consoles/gba_recomp_build.py" "$rom" "$work/pack" --loop 4 --gba-run "$run" \
        --run-args "$*" >"$work/build.log" 2>&1 || { tail -20 "$work/build.log"; exit 1; }
    lib=$work/pack/libtitle.so
    check "loaded, recompiled (lockstep)" --state-load "$st" --pack "$lib" --lockstep "$@"
    # Saved by the recompiled run: the same state, it loads in both.
    st2=$work/state-rc.st
    b=$(timeout 300 "$run" "$rom" --quiet --frames $((N + M)) --state-save "$N:$st2" --run-hash --pack "$lib" "$@") ||
        exit 1
    hb=$(hash_of "$b")
    echo "recompiled run, saved after frame $N: $hb"
    [ "$hb" = "$ha" ] || { echo "FAIL: recompiled run $hb != $ha"; exit 1; }
    check "recompiled state, interpreter" --state-load "$st2" "$@"
    check "recompiled state, recompiled" --state-load "$st2" --pack "$lib" "$@"
fi
echo "OK"
