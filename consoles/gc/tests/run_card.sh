#!/bin/sh
# ctest helper (XBA-264): run_card.sh GC_PLAY CARDTEST_DOL WORKDIR
#
# Our memory card test (homebrew/cardtest) through gc_runtime headless, with a save dir: the cards' files across
# launches, as the app keeps them.
#   1. a first launch: no file, a blank 251-block card; the program formats it, creates its file and writes run 1;
#      cardA.raw (2 MB) holds "XBACARD1 runs=0001 prev=none" (written at exit)
#   2. a second launch on that file: the program finds run 1 intact and writes "runs=0002 prev=ok"
#   3. a 59-block card (--card-size), paced for 200 fields (over 3 s): written two seconds after the game's last
#      write, before the exit; 512 KB
#   4. RECOMP_GC_CARD_SIZE=1019 with slot B on: cardA.raw is 8 MB, cardB.raw never written (the program uses slot A)
#   5. a file of no card's size is left alone
# Exits 77 (skipped) when the DOL is absent (tools/consoles/build_homebrew.sh gc).
play=$1
dol=$2
work=$3
if [ ! -f "$dol" ]; then
    echo "skipped: $dol not built (tools/consoles/build_homebrew.sh gc)"
    exit 77
fi
rm -rf "$work"
mkdir -p "$work"
fail() {
    echo "FAIL: $*"
    exit 1
}

# run NAME SAVE_DIR [gc_play options...]
run() {
    name=$1
    save=$2
    shift 2
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 120 "$play" "$dol" --headless --frames 240 \
        --save-dir "$save" "$@" >"$work/$name.log" 2>&1
    rc=$?
    sed "s/^/[$name] /" "$work/$name.log" | grep -E "memory card|gc: [0-9]+ fields|stopped"
    [ "$rc" -eq 0 ] || fail "$name: gc_play exit $rc"
}

# card SAVE_DIR SLOT: the card file's path (the key is the DOL's)
card() {
    ls "$1"/gc-*/card"$2".raw 2>/dev/null | head -n 1
}

size() {
    wc -c <"$1" | tr -d ' '
}

run first "$work/save" --unpaced
a=$(card "$work/save" A)
[ -n "$a" ] || fail "first: no cardA.raw under $work/save"
[ "$(size "$a")" = 2097152 ] || fail "first: $a is $(size "$a") bytes, want 2097152 (251 blocks)"
grep -aq "XBACARD1 runs=0001 prev=none" "$a" || fail "first: run 1 not in $a"
grep -q "memory card A: blank, 251 blocks" "$work/first.log" || fail "first: no blank card logged"
grep -q "memory card A written (exit)" "$work/first.log" || fail "first: not written at exit"
[ ! -e "$a.tmp" ] || fail "a temporary file was left"

run second "$work/save" --unpaced
grep -q "memory card A (251 blocks) loaded from" "$work/second.log" || fail "second: card not loaded"
grep -aq "XBACARD1 runs=0002 prev=ok" "$a" || fail "second: run 1 not found or run 2 not written"

run small "$work/save59" --card-size 59 --frames 200
s=$(card "$work/save59" A)
[ -n "$s" ] && [ "$(size "$s")" = 524288 ] || fail "59 blocks: ${s:-no file}, want 524288 bytes"
grep -q "memory card A written (after the game's writes)" "$work/small.log" || fail "59 blocks: no write after the game's"
grep -aq "XBACARD1 runs=0001 prev=none" "$s" || fail "59 blocks: run 1 not in $s"

RECOMP_GC_CARD_SIZE=1019 run big "$work/save1019" --unpaced --card-b
b=$(card "$work/save1019" A)
[ -n "$b" ] && [ "$(size "$b")" = 8388608 ] || fail "1019 blocks: ${b:-no file}, want 8388608 bytes"
grep -q "memory card B: blank, 1019 blocks" "$work/big.log" || fail "slot B not inserted"
[ -z "$(card "$work/save1019" B)" ] || fail "cardB.raw written though slot B was never used"

dir=$(dirname "$a")
mkdir -p "$work/savebad/${dir##*/}"
head -c 1000 /dev/zero >"$work/savebad/${dir##*/}/cardA.raw"
run bad "$work/savebad" --unpaced
grep -q "is no memory card image (1000 bytes), left alone" "$work/bad.log" || fail "bad file: no warning"
[ "$(size "$work/savebad/${dir##*/}/cardA.raw")" = 1000 ] || fail "bad file overwritten"

# 6. a card that cannot be written (a read-only folder): an error, tried again two seconds later, not every field
ro="$work/savero/${dir##*/}"
mkdir -p "$ro"
chmod 555 "$ro"
if [ -w "$ro" ]; then
    echo "read-only folder: writable anyway (root?), skipped"
else
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 120 "$play" "$dol" --headless --frames 300 \
        --save-dir "$work/savero" >"$work/ro.log" 2>&1
    rc=$?
    chmod 755 "$ro"
    [ "$rc" -eq 0 ] || fail "read-only: gc_play exit $rc"
    n=$(grep -c "cannot write memory card A" "$work/ro.log")
    echo "read-only folder: $n failed write(s) in 300 fields"
    [ "$n" -ge 1 ] && [ "$n" -le 4 ] || fail "read-only: $n failed writes, want 1-4 (one every 2 s, then exit)"
fi
chmod 755 "$ro" 2>/dev/null
echo "run_card: all passed"
