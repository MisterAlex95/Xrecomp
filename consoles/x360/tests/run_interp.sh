#!/bin/sh
# ctest helper (XBA-283): run_interp.sh X360_RUN WORKDIR MODE EXPECTED_TXT [ELF] [OMIT_FLAGS] -- code the pack does not
# hold is interpreted and the run goes on. The test title: MODE basic (make_hello.py) or c (the devkitPPC ELF,
# skipped (77) when not built).
# 1. The interpreter: functions left out of the pack on purpose (choose_omit.py OMIT_FLAGS: every second function,
#    --all: every one, --scalar: not those with vector code). The run must still print EXPECTED_TXT through DbgPrint
#    and end with exit code 0, --missed naming only left-out entries.
# 2. The self-healing loop, as on a title: lifted from the entry point only (--no-pdata), the functions only a pointer
#    reaches (a call table, a thread's start) are missed. Round 1 must print EXPECTED_TXT and name them; round 2,
#    lifted with what round 1 found (--discovered), must print the same and miss nothing: one run found them all.
# 3. --stop-at-miss stops at the first missed address (exit 1).
# Exits 77 when python3 or cc is missing.
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
lift() { # lift RECOMP_ARGS...
    rm -rf "$work/src"
    "$py" -m x360_tools.recomp "$work/t.xex" "$work/src" --no-comments "$@" >"$work/recomp.txt" ||
        { cat "$work/recomp.txt"; exit 1; }
    "$cc" -shared -fPIC -O1 -ffp-contract=off -DXE_RUNTIME=1 -I "$repo/consoles/x360/runtime" \
        -I "$repo/consoles/gc/runtime" -o "$work/libtitle.so" "$work"/src/*.c -lm || exit 1
}
play() { # play NAME: run, check the output, missed addresses sorted in NAME.m
    timeout 300 "$run" "$work/t.xex" --pack "$work/libtitle.so" --disc "$work/disc" --dbg "$work/$1.dbg" \
        --log "$work/$1.log" --stats --missed "$work/$1.missed"
    rc=$?
    grep -v '^#' "$work/$1.missed" 2>/dev/null | sort >"$work/$1.m"
    echo "$1: exit $rc, $(wc -l <"$work/$1.m") missed, $(grep -o "interpreted [0-9]*" "$work/$1.log" | tail -n 1)"
    [ "$rc" = 0 ] || { echo "$1: x360_run exited with $rc"; tail -n 20 "$work/$1.log"; exit 1; }
    cmp -s "$work/$1.dbg" "$expected" || {
        echo "$1: DbgPrint output differs:"; diff "$expected" "$work/$1.dbg" | head -n 20; exit 1; }
}
# 1. functions left out on purpose
lift
# shellcheck disable=SC2086
"$py" "$here/choose_omit.py" "$work/src" "$work/omit.txt" $flags || exit 1
lift --omit "$work/omit.txt"
play omit
[ -s "$work/omit.m" ] || { echo "omit: missed nothing: the left-out code was not reached"; exit 1; }
sort "$work/omit.txt" >"$work/omit_sorted.txt"
extra=$(comm -23 "$work/omit.m" "$work/omit_sorted.txt")
[ -z "$extra" ] || { echo "omit: names addresses that are not left-out entries: $extra"; exit 1; }
# 2. the self-healing loop
lift --no-pdata
play round1
[ -s "$work/round1.m" ] || { echo "round1: missed nothing"; exit 1; }
lift --no-pdata --discovered "$work/round1.m"
play round2
[ ! -s "$work/round2.m" ] || { echo "round2 still missed: $(cat "$work/round2.m")"; exit 1; }
# 3. the old behaviour
lift --no-pdata
timeout 300 "$run" "$work/t.xex" --pack "$work/libtitle.so" --disc "$work/disc" --dbg "$work/stop.dbg" \
    --log "$work/stop.log" --stop-at-miss
rc=$?
[ "$rc" = 1 ] || { echo "--stop-at-miss should stop at the first missed address (exit $rc)"; exit 1; }
echo "interpreter fallback: $(wc -l <"$work/omit.m") left-out function(s) interpreted; the self-healing loop found" \
    "$(wc -l <"$work/round1.m") missed function(s) in one run, none left in round 2"
