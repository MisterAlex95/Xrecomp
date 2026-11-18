#!/bin/sh
# ctest helper (XBA-283): run_interp.sh X360_RUN WORKDIR MODE EXPECTED_TXT [ELF] [OMIT_FLAGS] -- missed code is
# interpreted and the run goes on. The test title (MODE basic: make_hello.py; c: the devkitPPC ELF, skipped (77)
# when not built) is recompiled with functions left out of the pack on purpose (choose_omit.py OMIT_FLAGS: every
# second function by default, --all: every one). Round 1: the run must still print EXPECTED_TXT through DbgPrint and
# end with exit code 0, and --missed must name only left-out entries. Round 2 lifts what round 1 found (the rest
# stays out): the run must print the same and miss nothing, so one run found every missed function it reached.
# Round 3: --stop-at-miss stops at the first missed address (exit 1). Exits 77 when python3 or cc is missing.
run=$1
work=$2
mode=$3
expected=$4
elf=$5
flags=$6
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
if [ "$mode" = c ] && [ ! -f "$elf" ]; then
    echo "skipped: $elf not built (tools/consoles/build_homebrew.sh x360)"
    exit 77
fi
rm -rf "$work"
mkdir -p "$work/disc/data"
export PYTHONPATH="$repo:$repo/consoles/x360/tools"
if [ "$mode" = c ]; then
    "$py" "$here/homebrew/elf2xex.py" "$elf" "$work/t.xex" || exit 1
else
    "$py" "$here/make_hello.py" "$work/t.xex" --compression basic || exit 1
fi
printf 'Hello from the disc!' >"$work/disc/data/hello.txt"
printf 'binary data' >"$work/disc/data/b.bin"
printf 'readme\n' >"$work/disc/readme.txt"
lift() { # lift OMIT_FILE
    rm -rf "$work/src"
    "$py" -m x360_tools.recomp "$work/t.xex" "$work/src" --no-comments --omit "$1" >"$work/recomp.txt" ||
        { cat "$work/recomp.txt"; exit 1; }
    "$cc" -shared -fPIC -O1 -ffp-contract=off -DXE_RUNTIME=1 -I "$repo/consoles/x360/runtime" \
        -I "$repo/consoles/gc/runtime" -o "$work/libtitle.so" "$work"/src/*.c -lm || exit 1
}
: >"$work/none.txt"
lift "$work/none.txt"
# shellcheck disable=SC2086
"$py" "$here/choose_omit.py" "$work/src" "$work/omit.txt" $flags || exit 1
lift "$work/omit.txt"
for round in 1 2; do
    timeout 300 "$run" "$work/t.xex" --pack "$work/libtitle.so" --disc "$work/disc" --dbg "$work/dbg$round.txt" \
        --log "$work/log$round.txt" --stats --missed "$work/missed$round.txt"
    rc=$?
    grep -v '^#' "$work/missed$round.txt" | sort >"$work/m$round.txt"
    echo "round $round: exit $rc, $(wc -l <"$work/m$round.txt") missed, $(grep -o "interpreted [0-9]*" "$work/log$round.txt" | tail -n 1)"
    [ "$rc" = 0 ] || { echo "round $round: x360_run exited with $rc"; tail -n 20 "$work/log$round.txt"; exit 1; }
    cmp -s "$work/dbg$round.txt" "$expected" || {
        echo "round $round: DbgPrint output differs:"; diff "$expected" "$work/dbg$round.txt" | head -n 20; exit 1; }
    if [ "$round" = 1 ]; then
        [ -s "$work/m1.txt" ] || { echo "round 1 missed nothing: the left-out code was not reached"; exit 1; }
        sort "$work/omit.txt" >"$work/omit_sorted.txt"
        extra=$(comm -23 "$work/m1.txt" "$work/omit_sorted.txt")
        [ -z "$extra" ] || { echo "round 1 names addresses that are not left-out entries: $extra"; exit 1; }
        comm -23 "$work/omit_sorted.txt" "$work/m1.txt" >"$work/omit2.txt"
        lift "$work/omit2.txt"
    else
        [ ! -s "$work/m2.txt" ] || { echo "round 2 still missed: $(cat "$work/m2.txt")"; exit 1; }
    fi
done
lift "$work/omit.txt"
timeout 300 "$run" "$work/t.xex" --pack "$work/libtitle.so" --disc "$work/disc" --dbg "$work/dbg3.txt" \
    --log "$work/log3.txt" --stop-at-miss
rc=$?
[ "$rc" = 1 ] || { echo "round 3: --stop-at-miss should stop at the first missed address (exit $rc)"; exit 1; }
echo "interpreter fallback: $(wc -l <"$work/m1.txt") missed function(s) found in one run, none left in round 2"
