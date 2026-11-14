#!/bin/sh
# ctest helper (XBA-166): run_jcz_cd.sh PS1_RUN BIOS EXE WORKDIR getloc|timing|disc-swap. Runs one of JaCzekanski's
# ps1-tests cdrom programs (tools/consoles/fetch_ps1_tests.sh) with a disc in the drive: a 74:10 synthetic image (a
# sparse .iso of 333600 empty 2048-byte sectors, which ps1_disc gives headers: nothing on disk, no game data), the
# size of the CD the hardware logs were made with (it seeks to 40:00 and 74:00, and past the end at 74:30).
#   getloc  passes when the program prints "Test passed" (its own checks: GetlocL / GetlocP after reset, seeks, a
#           read, a seek past the end)
#   timing  prints the measured times and checks them against the ranges of the program's hardware log (psx.log):
#           command acknowledges, Init, sector periods at 1x / 2x, ReadN to the first sector after a 100-sector seek,
#           Pause's second response
#   disc-swap  the lid opened and closed again (ps1_run --swap): passes when the program sees INT5 (01h, as on the
#           hardware), Getstat with bit 4, then the disc back
# Exits 77 (skipped) when the EXE or the BIOS is not there.
run=$1
bios=$2
exe=$3
work=$4
what=$5
if [ ! -f "$bios" ]; then
    echo "skipped: BIOS $bios not built (tools/consoles/build_ps1_bios.sh)"
    exit 77
fi
if [ ! -f "$exe" ]; then
    echo "skipped: $exe not fetched (tools/consoles/fetch_ps1_tests.sh)"
    exit 77
fi
mkdir -p "$work" || exit 1
disc=$work/disc-74m10s.iso
rm -f "$disc"
# 333600 sectors x 2048 bytes, a hole
dd if=/dev/zero of="$disc" bs=2048 count=0 seek=333600 2>/dev/null || exit 1
case "$what" in
getloc)
    out=$(timeout 250 "$run" --bios "$bios" --exe "$exe" --disc "$disc" --tty --until-tty "Test " --frames 6000 --quiet)
    rc=$?
    rm -f "$disc"
    printf '%s\n' "$out"
    [ $rc -eq 0 ] || { echo "FAIL: no verdict (ps1_run exit $rc)"; exit 1; }
    printf '%s\n' "$out" | grep -q "^Test passed" || { echo "FAIL: the test failed"; exit 1; }
    ;;
disc-swap)
    # the lid opens at frame 200 and closes 60 frames later (ps1_run --swap): the program sees INT5, then Getstat
    # with bit 4 until the disc is back
    out=$(timeout 250 "$run" --bios "$bios" --exe "$exe" --disc "$disc" --swap "200:$disc" --tty --until-tty \
        "Disc detected" --frames 2000 --quiet)
    rc=$?
    rm -f "$disc"
    printf '%s\n' "$out"
    [ $rc -eq 0 ] || { echo "FAIL: the disc swap was not seen (ps1_run exit $rc)"; exit 1; }
    printf '%s\n' "$out" | grep -q "Got IRQ 5 (expected 5), status 0x01" || { echo "FAIL: no INT5 01h"; exit 1; }
    printf '%s\n' "$out" | grep -q "Getstat: 0x1" || { echo "FAIL: no Getstat with bit 4"; exit 1; }
    ;;
timing)
    out=$(timeout 280 "$run" --bios "$bios" --exe "$exe" --disc "$disc" --tty --frames 4000 --quiet)
    rm -f "$disc"
    printf '%s\n' "$out"
    printf '%s\n' "$out" | awk '
        function check(name, v, lo, hi) {
            if (v < lo || v > hi) { printf "FAIL: %s = %d, hardware %d..%d\n", name, v, lo, hi; bad = 1 }
            n++
        }
        function num(line, key,    i, s) {
            i = index(line, key); if (!i) return -1
            s = substr(line, i + length(key)); sub(/^[ =]*/, "", s); return s + 0
        }
        /^init complete:/ { check("init complete", num($0, "average"), 400000, 620000) }
        /^CdlNop:/ { check("Getstat acknowledge", num($0, "average"), 24000, 140000) }
        /timing: Setloc ACK/ {
            fast = ($0 ~ /^double/); runs[fast]++
            check((fast ? "2x" : "1x") " remaining sectors", num($0, "avg"), fast ? 215000 : 440000, fast ? 232000 : 455000)
            if (runs[fast] > 1)    # the first read of each speed starts from elsewhere (spin-up, LBA 0)
                check((fast ? "2x" : "1x") " first sector", num($0, "First Sector"), fast ? 550000 : 650000, 1030000)
            check((fast ? "2x" : "1x") " pause complete", num($0, "Pause Complete"), 990000, 1060000)
        }
        END {
            if (n < 20) { printf "FAIL: %d timings measured (the run did not finish?)\n", n; exit 1 }
            exit bad
        }' || exit 1
    echo "timings within the hardware log's ranges"
    ;;
*)
    echo "usage: $0 PS1_RUN BIOS EXE WORKDIR getloc|timing|disc-swap" >&2
    exit 2
    ;;
esac
