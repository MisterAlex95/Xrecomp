#!/bin/sh
# ctest helper (XBA-182): state_exe.sh PS1_RUN BIOS INPUT WORKDIR MODE [ps1_run args...]
#
# The save state round trip on one program (a PS-X EXE or a disc image), the GBA's state_rom.sh: run N + M frames
# saving a state after frame N, hashing the picture and audio of the M frames after it (ps1_run --state-save N:FILE
# --run-hash); load that state into a new machine and run M frames: same hash. A run of M frames from power-on must
# differ (the state mattered).
#   MODE interp   the interpreter alone
#   MODE pack     also with the program's title pack (ps1_recomp_build.py, the discovery loop): the state saved by the
#                 recompiled run loads in the interpreter and the other way round, same hash each time, and the
#                 recompiled run from the state stays in lockstep with an interpreter twin
#   MODE other:INPUT2  the state of INPUT loaded with INPUT2 must be refused (exit 5)
#   MODE bios:BIOS2    the state loaded under another BIOS (e.g. hle) must be refused (exit 5)
# Exits 77 (skipped) when INPUT, the BIOS or a file a mode needs is absent, or python3 / cc for MODE pack.
run=$1
bios=$2
input=$3
work=$4
mode=$5
shift 5
N=${STATE_N:-150}
M=${STATE_M:-150}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ "$bios" != hle ] && [ ! -f "$bios" ]; then
    echo "skipped: BIOS $bios not built (tools/consoles/build_ps1_bios.sh)"
    exit 77
fi
[ -f "$input" ] || { echo "skipped: $input not there"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
st=$work/state.st
hash_of() { echo "$1" | sed -n 's/^run-hash \([0-9a-f]*\) frames .*/\1/p'; }

a=$(timeout 300 "$run" "$input" --bios "$bios" --quiet --frames $((N + M)) --state-save "$N:$st" --run-hash "$@") ||
    exit 1
ha=$(hash_of "$a")
[ -n "$ha" ] && [ -s "$st" ] || { echo "FAIL: no hash or no state: $a"; exit 1; }
echo "saved after frame $N ($(wc -c <"$st") bytes), frames $((N + 1))..$((N + M)): $ha"

refused() { # what, then ps1_run args
    what=$1
    shift
    out=$(timeout 60 "$run" "$@" --quiet --frames 10 --state-load "$st")
    rc=$?
    echo "$out"
    [ $rc -eq 5 ] && echo "$out" | grep -q "STATE REFUSED" || { echo "FAIL: the state was loaded with $what"; exit 1; }
    echo "OK: refused with $what"
    exit 0
}
case "$mode" in
other:*)
    in2=${mode#other:}
    [ -f "$in2" ] || { echo "skipped: $in2 not built"; exit 77; }
    refused "another game" "$in2" --bios "$bios" "$@"
    ;;
bios:*)
    b2=${mode#bios:}
    [ "$b2" = hle ] || [ -f "$b2" ] || { echo "skipped: $b2 not built"; exit 77; }
    refused "another BIOS" "$input" --bios "$b2" "$@"
    ;;
esac

check() { # label, then ps1_run args
    label=$1
    shift
    out=$(timeout 300 "$run" "$input" --bios "$bios" --quiet --frames $M --run-hash "$@") ||
        { echo "FAIL ($label): $out"; exit 1; }
    h=$(hash_of "$out")
    echo "$label: $h"
    [ "$h" = "$ha" ] || { echo "FAIL ($label): $h != $ha"; echo "$out"; exit 1; }
}
check "loaded, interpreter" --state-load "$st" "$@"
cold=$(hash_of "$(timeout 300 "$run" "$input" --bios "$bios" --quiet --frames $M --run-hash "$@")")
echo "from power-on: $cold"
[ "$cold" != "$ha" ] || { echo "FAIL: the state changed nothing"; exit 1; }

if [ "$mode" = pack ]; then
    py=${PYTHON:-python3}
    command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
    command -v "${CC:-cc}" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
    timeout 1200 "$py" "$repo/tools/consoles/ps1_recomp_build.py" "$input" "$work/pack" --loop 5 --ps1-run "$run" \
        --frames $((N + M)) --run-args "--bios $bios $*" >"$work/build.log" 2>&1 || { tail -20 "$work/build.log"; exit 1; }
    lib=$work/pack/libtitle.so
    out=$(timeout 300 "$run" "$input" --bios "$bios" --quiet --frames $M --run-hash --state-load "$st" --pack "$lib" \
        --lockstep "$@")
    echo "$out" | grep -q "^LOCKSTEP OK" || { echo "FAIL: the recompiled run from the state left lockstep: $out"; exit 1; }
    check "loaded, recompiled" --state-load "$st" --pack "$lib" "$@"
    st2=$work/state-rc.st
    b=$(timeout 300 "$run" "$input" --bios "$bios" --quiet --frames $((N + M)) --state-save "$N:$st2" --run-hash \
        --pack "$lib" "$@") || exit 1
    hb=$(hash_of "$b")
    echo "recompiled run, saved after frame $N: $hb"
    [ "$hb" = "$ha" ] || { echo "FAIL: recompiled run $hb != $ha"; exit 1; }
    check "recompiled state, interpreter" --state-load "$st2" "$@"
    check "recompiled state, recompiled" --state-load "$st2" --pack "$lib" "$@"
fi
echo "OK"
