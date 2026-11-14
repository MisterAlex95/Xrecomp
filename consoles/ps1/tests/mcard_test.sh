#!/bin/sh
# ctest helper (XBA-167): mcard_test.sh PS1_RUN BIOS MCARD_EXE WORKDIR [ps1_run args...]. The mcard homebrew saves
# through the BIOS memory card functions (consoles/ps1/tests/homebrew/mcard); ps1_run --card keeps the card in a file:
#   1. no file: the core's blank formatted card; the homebrew creates XBAMCARD (stage 1), the card is written out and
#      checked (check_mcard.py: header, directory and broken-sector checksums, the file entry, its data block);
#   2. the same file again: the save is found and read back (stage 2), the card is unchanged;
#   3. a card of zeros (unformatted): the homebrew formats it (BIOS format, table[1] = 1), then saves (stage 1).
# Exits 77 (skipped) when the BIOS or the EXE is not built, or python3 is missing.
run=$1
bios=$2
exe=$3
work=$4
shift 4
here=$(cd "$(dirname "$0")" && pwd)
[ -f "$bios" ] || { echo "skipped: BIOS $bios not built (tools/consoles/build_ps1_bios.sh)"; exit 77; }
[ -f "$exe" ] || { echo "skipped: $exe not built"; exit 77; }
command -v python3 >/dev/null 2>&1 || { echo "skipped: no python3"; exit 77; }
rm -rf "$work"
mkdir -p "$work" || exit 1
card=$work/card1.mcr

step() { # step NAME STAGE FORMATTED CARD [ps1_run args...]
    name=$1 stage=$2 fmt=$3 c=$4
    shift 4
    out=$(timeout 150 "$run" --bios "$bios" "$exe" --result --frames 1800 --card "$c" "$@" 2>&1)
    rc=$?
    echo "== $name"
    echo "$out"
    [ $rc -eq 0 ] || { echo "FAIL: $name: ps1_run exit $rc"; exit 1; }
    echo "$out" | grep -q "stage=$stage " || { echo "FAIL: $name: want stage $stage"; exit 1; }
    echo "$out" | grep -q "table\[1\] = 0x0000000$fmt" || { echo "FAIL: $name: want table[1] = $fmt (formatted)"; exit 1; }
}

step "new card" 1 0 "$card" "$@"
python3 -I "$here/check_mcard.py" "$card" || exit 1
cp "$card" "$work/first.mcr"
step "saved card, reloaded" 2 0 "$card" "$@"
cmp "$card" "$work/first.mcr" || { echo "FAIL: reading the save changed the card"; exit 1; }
python3 -I -c "open('$work/zero.mcr', 'wb').write(bytes(131072))" || exit 1
step "unformatted card" 1 1 "$work/zero.mcr" "$@"
python3 -I "$here/check_mcard.py" "$work/zero.mcr" || exit 1
echo "PASS"
