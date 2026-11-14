#!/bin/sh
# ctest helper (XBA-163): compare_bios.sh PS1_RUN BIOS FILE [ps1_run args...]. Runs one of our homebrew programs (an
# .exe or a disc) with --result under the BIOS image (LLE, OpenBIOS) and under the HLE BIOS (--bios hle), and passes
# when both report PASS with the same result block (status, checksum, every table entry). Exits 77 (skipped) when the
# BIOS image or FILE is not built.
run=$1
bios=$2
file=$3
shift 3
[ -f "$bios" ] || { echo "skipped: BIOS $bios not built (tools/consoles/build_ps1_bios.sh)"; exit 77; }
[ -f "$file" ] || { echo "skipped: $file not built"; exit 77; }
block() { # block BIOS ARGS...: the result block without the frame count of the run
    b=$1
    shift
    timeout 150 "$run" --bios "$b" "$file" --result "$@" 2>/dev/null | grep -e '^result:' -e 'table\[' |
        sed 's/ ([0-9]* frames run)//'
}
lle=$(block "$bios" "$@")
hle=$(block hle "$@")
echo "== LLE"
printf '%s\n' "$lle" | head -3
echo "== HLE"
printf '%s\n' "$hle" | head -3
printf '%s\n' "$hle" | grep -q '^result: PASS' || { echo "FAIL: no PASS under the HLE BIOS"; exit 1; }
if [ "$lle" != "$hle" ]; then
    echo "FAIL: the result blocks differ"
    tmp="${TMPDIR:-/tmp}/ps1_lle.$$"
    printf '%s\n' "$lle" >"$tmp"
    printf '%s\n' "$hle" | diff "$tmp" -
    rm -f "$tmp"
    exit 1
fi
echo "PASS: same result block under both BIOSes"
