#!/bin/sh
# ctest helper (XBA-201): the mGBA test suite (mgba-emu/suite, MIT; tools/consoles/build_gba_suite.sh) under gba_run.
#
#   mgba_suite.sh GBA_RUN SUITE_ROM [--recomp WORKDIR] GROUP=MIN... [-- gba_run args...]
#
# Each GROUP runs from a fresh boot (the suite's SRAM log, which some tests read back, then starts the same): the pad
# script walks the menu to the group, starts it and waits for the suite's own "END: passed/total" line on mGBA's
# debug-print port (gba_run --pad-script, --debug-log). Groups: memory io_read timing timers timer_irq shifter carry
# multiply_long bios_math dma sio_read sio_timing misc, and video: each of its 7 scenes drawn by the code under test
# and by the suite's reference code, the two frames hashed (the menu sprite hidden), equal = passed.
# Passes when every group's count is at least its MIN (the counts reached so far: a regression fails) and gba_run
# exits 0. --recomp WORKDIR: first builds the ROM's title pack with the discovery loop (tools/consoles/
# gba_recomp_build.py), then runs every group with it in lockstep with the interpreter (gba_run --pack --lockstep).
# Exits 77 (skipped) when the ROM or a --bios file is absent (they need Docker / the network), or python3 / cc for
# --recomp.
run=$1
rom=$2
shift 2
work=
if [ "$1" = "--recomp" ]; then
    work=$2
    shift 2
fi
groups=
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
    groups="$groups $1"
    shift
done
[ "$1" = "--" ] && shift
if [ ! -f "$rom" ]; then
    echo "skipped: $rom not built (tools/consoles/build_gba_suite.sh)"
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
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

menu_index() {
    i=0
    for n in memory io_read timing timers timer_irq shifter carry multiply_long bios_math dma sio_read sio_timing misc; do
        [ "$n" = "$1" ] && { echo $i; return 0; }
        i=$((i + 1))
    done
    return 1
}

# The pad script of a group: down to its line of the menu, A, then wait for its END line.
group_pad() {
    echo "wait 10"
    k=0
    while [ $k -lt "$1" ]; do
        echo "press DOWN 2"
        echo "wait 2"
        k=$((k + 1))
    done
    echo "press A 2"
    echo "until-log END: 30000"
}

# The video group: menu's last line (up from the first), A; for each scene: A (the code under test), START (hide the
# menu sprite), hash; RIGHT (the reference), hash; B; down to the next.
video_scenes() {
    for s in 0 1 2 3 4 5 6; do
        printf 'press A 2\npress START 2\nwait 20\nhash %s.actual\npress RIGHT 2\nwait 20\nhash %s.expected\n' "$s" "$s"
        printf 'press B 2\nwait 4\npress DOWN 2\nwait 2\n'
    done
}
video_pad() {
    printf 'wait 10\npress UP 2\nwait 2\npress A 2\nwait 4\n'
    video_scenes
}

pack_args=
if [ -n "$work" ]; then
    py=${PYTHON:-python3}
    command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
    command -v "${CC:-cc}" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
    # Discovery over every group, one after the other in one boot (B back to the menu, down to the next).
    {
        echo "wait 10"
        for g in 0 1 2 3 4 5 6 7 8 9 10 11 12; do
            printf 'press A 2\nuntil-log END: 30000\npress B 2\nwait 2\npress DOWN 2\nwait 2\n'
        done
        printf 'press A 2\nwait 4\n' # the menu is on its last line: video
        video_scenes
    } >"$tmp/all.pad"
    rm -rf "$work"
    timeout 900 "$py" "$repo/tools/consoles/gba_recomp_build.py" "$rom" "$work" --loop 4 --gba-run "$run" \
        --frames 10000000 --run-args "--pad-script $tmp/all.pad $*" || exit 1
    pack_args="--pack $work/libtitle.so --lockstep"
fi

rc=0
for gm in $groups; do
    g=${gm%%=*}
    min=${gm#*=}
    if [ "$g" = video ]; then
        video_pad >"$tmp/video.pad"
        # shellcheck disable=SC2086
        out=$(timeout 300 "$run" "$rom" --quiet --pad-script "$tmp/video.pad" $pack_args "$@")
        r=$?
        passed=0
        for s in 0 1 2 3 4 5 6; do
            a=$(echo "$out" | sed -n "s/^hash $s.actual //p")
            e=$(echo "$out" | sed -n "s/^hash $s.expected //p")
            [ -n "$a" ] && [ "$a" = "$e" ] && passed=$((passed + 1))
        done
        total=7
    else
        idx=$(menu_index "$g") || { echo "unknown group $g"; exit 2; }
        group_pad "$idx" >"$tmp/$g.pad"
        # shellcheck disable=SC2086
        out=$(timeout 300 "$run" "$rom" --quiet --pad-script "$tmp/$g.pad" --debug-log $pack_args "$@")
        r=$?
        line=$(echo "$out" | sed -n 's/^mgba: [0-9] END: \([0-9]*\)\/\([0-9]*\)$/\1 \2/p' | tail -n 1)
        passed=${line% *}
        total=${line#* }
        [ -n "$line" ] || { passed=0; total=?; }
    fi
    echo "$out" | grep -E "^(LOCKSTEP|PAD TIMEOUT)" | sed "s/^/  $g: /"
    if [ "$r" -eq 0 ] && [ "$passed" -ge "$min" ]; then
        echo "mgba $g: $passed/$total (at least $min) ok"
    else
        echo "mgba $g: $passed/$total (at least $min) FAIL (gba_run exit $r)"
        rc=1
    fi
done
exit $rc
