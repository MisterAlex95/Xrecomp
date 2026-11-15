#!/bin/sh
# ctest helper (XBA-183): state_rom.sh N64_RUN ROM WORKDIR MODE [n64_run args...]
#
# The save state round trip on one ROM, the PS1's state_exe.sh: run N + M frames saving a state after frame N,
# hashing the picture and audio of the M frames after it (n64_run --state-save N:FILE --run-hash); load that state
# into a new machine and run M frames: same hash. A run of M frames from power-on must differ (the state mattered).
#   MODE interp   the interpreter alone (software RDP; also loaded with 2 and 4 scanline-band RDP threads)
#   MODE vulkan   the same with the Vulkan RDP (RECOMP_N64_RDP=vulkan; skipped without a Vulkan device): its state
#                 round trip, and the software RDP's state loaded on it and the other way round (another back end's
#                 state loads its registers; the hash may then differ, the run must go on)
#   MODE pack     also with the ROM's title pack (n64_recomp_build.py, the discovery loop): the state saved by the
#                 recompiled run loads in the interpreter and the other way round, same hash each time, and the
#                 recompiled run from the state stays in lockstep with an interpreter twin
#   MODE other:ROM2  the state of ROM loaded with ROM2 must be refused (exit 5), and a truncated state too
# Exits 77 (skipped) when ROM or a file a mode needs is absent, python3 / cc for MODE pack, a Vulkan device for MODE
# vulkan.
run=$1
rom=$2
work=$3
mode=$4
shift 4
N=${STATE_N:-150}
M=${STATE_M:-150}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
[ -f "$rom" ] || { echo "skipped: $rom not built (tools/consoles/build_homebrew.sh n64)"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
st=$work/state.st
hash_of() { echo "$1" | sed -n 's/^run-hash \([0-9a-f]*\) frames .*/\1/p'; }

if [ "$mode" = vulkan ]; then
    export RECOMP_N64_RDP=vulkan
    probe=$(timeout 300 "$run" "$rom" --quiet --frames 2 2>&1)
    if echo "$probe" | grep -q "software RDP instead"; then
        echo "skipped: no Vulkan device for the Vulkan RDP"
        exit 77
    fi
fi

a=$(timeout 600 "$run" "$rom" --quiet --frames $((N + M)) --state-save "$N:$st" --run-hash "$@") || exit 1
ha=$(hash_of "$a")
[ -n "$ha" ] && [ -s "$st" ] || { echo "FAIL: no hash or no state: $a"; exit 1; }
echo "saved after frame $N ($(wc -c <"$st") bytes), frames $((N + 1))..$((N + M)): $ha"

case "$mode" in
other:*)
    rom2=${mode#other:}
    [ -f "$rom2" ] || { echo "skipped: $rom2 not built"; exit 77; }
    out=$(timeout 60 "$run" "$rom2" --quiet --frames 10 --state-load "$st" "$@")
    rc=$?
    echo "$out"
    [ $rc -eq 5 ] && echo "$out" | grep -q "STATE REFUSED.*another ROM" || { echo "FAIL: loaded with another ROM"; exit 1; }
    head -c 5000 "$st" >"$work/truncated.st"
    out=$(timeout 60 "$run" "$rom" --quiet --frames 10 --state-load "$work/truncated.st" "$@")
    rc=$?
    echo "$out"
    [ $rc -eq 5 ] || { echo "FAIL: a truncated state was loaded"; exit 1; }
    echo "OK: refused with another ROM and truncated"
    exit 0
    ;;
esac

check() { # label, then n64_run args
    label=$1
    shift
    out=$(timeout 600 "$run" "$rom" --quiet --frames $M --run-hash "$@") || { echo "FAIL ($label): $out"; exit 1; }
    h=$(hash_of "$out")
    echo "$label: $h"
    [ "$h" = "$ha" ] || { echo "FAIL ($label): $h != $ha"; echo "$out"; exit 1; }
}
check "loaded" --state-load "$st" "$@"
cold=$(hash_of "$(timeout 600 "$run" "$rom" --quiet --frames $M --run-hash "$@")")
echo "from power-on: $cold"
[ "$cold" != "$ha" ] || { echo "FAIL: the state changed nothing"; exit 1; }

case "$mode" in
interp)
    XBR_N64_RDP_THREADS=2 check "loaded, 2 RDP threads" --state-load "$st" "$@"
    XBR_N64_RDP_THREADS=4 check "loaded, 4 RDP threads" --state-load "$st" "$@"
    ;;
vulkan)
    sw=$work/state-sw.st
    RECOMP_N64_RDP=software timeout 600 "$run" "$rom" --quiet --frames $((N + 1)) --state-save "$N:$sw" "$@" >/dev/null ||
        exit 1
    out=$(timeout 600 "$run" "$rom" --quiet --frames $M --run-hash --state-load "$sw" "$@" 2>&1) ||
        { echo "FAIL: the software RDP's state on the Vulkan RDP: $out"; exit 1; }
    echo "software RDP state on the Vulkan RDP: $(hash_of "$out")"
    out=$(RECOMP_N64_RDP=software timeout 600 "$run" "$rom" --quiet --frames $M --run-hash --state-load "$st" "$@" 2>&1) ||
        { echo "FAIL: the Vulkan RDP's state on the software RDP: $out"; exit 1; }
    echo "Vulkan RDP state on the software RDP: $(hash_of "$out")"
    # Upscaled (XBA-185): the upscaled copy of RDRAM is derived, not in the state; after a load paraLLEl-RDP takes
    # the 1x pixels where RDRAM changed, so what the game draws again is upscaled again and what it never redraws
    # stays at 1x. The machine itself (RDRAM, instructions, cycles: n64_run --hash) is the same as the run's.
    up=$work/state-2x.st
    hm() { echo "$1" | sed -n 's/^hash: //p'; }
    a2=$(hm "$(RECOMP_N64_UPSCALE=2 timeout 600 "$run" "$rom" --quiet --frames $((N + M)) --state-save "$N:$up" --hash "$@")")
    b2=$(hm "$(RECOMP_N64_UPSCALE=2 timeout 600 "$run" "$rom" --quiet --frames $M --state-load "$up" --hash "$@")")
    echo "upscaled 2x: $a2 / loaded: $b2"
    [ -n "$a2" ] && [ "$a2" = "$b2" ] || { echo "FAIL: upscaled 2x, the machine after the load differs"; exit 1; }
    ;;
pack)
    py=${PYTHON:-python3}
    command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
    command -v "${CC:-cc}" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
    timeout 2400 "$py" "$repo/tools/consoles/n64_recomp_build.py" "$rom" "$work/pack" --loop 5 --n64-run "$run" \
        --frames $((N + M)) --run-args="$*" >"$work/build.log" 2>&1 || { tail -20 "$work/build.log"; exit 1; }
    lib=$work/pack/libtitle.so
    out=$(timeout 1200 "$run" "$rom" --quiet --frames $M --run-hash --state-load "$st" --pack "$lib" --lockstep "$@")
    echo "$out" | grep -q "^LOCKSTEP OK" || { echo "FAIL: the recompiled run from the state left lockstep: $out"; exit 1; }
    check "loaded, recompiled" --state-load "$st" --pack "$lib" "$@"
    st2=$work/state-rc.st
    b=$(timeout 600 "$run" "$rom" --quiet --frames $((N + M)) --state-save "$N:$st2" --run-hash --pack "$lib" "$@") ||
        exit 1
    hb=$(hash_of "$b")
    echo "recompiled run, saved after frame $N: $hb"
    [ "$hb" = "$ha" ] || { echo "FAIL: recompiled run $hb != $ha"; exit 1; }
    check "recompiled state, interpreter" --state-load "$st2" "$@"
    check "recompiled state, recompiled" --state-load "$st2" --pack "$lib" "$@"
    ;;
esac
echo "OK"
