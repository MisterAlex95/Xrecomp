#!/bin/sh
# ctest helper (XBA-169 / XBA-170 / XBA-171): recomp_rom.sh N64_RUN ROM WORKDIR FRAMES [n64_run args...]
#
# Builds ROM's title pack with the discovery loop (tools/consoles/n64_recomp_build.py: n64_tools.recomp from the ROM
# alone, the host cc at -O1, runs until no new code is recorded; with ROM.elf next to the ROM -- our homebrew -- the
# share of its function symbols the discovery found is measured and must be at least 99 %), then runs ROM under
# n64_run --pack --lockstep: the recompiled machine and an interpreter-only one side by side, the whole state (CPU,
# COP0, TLB, FPU, time, instruction count, scheduler, MI, RDRAM, RSP memory) and the picture compared after every
# frame. Passes when the run agrees frame for frame and n64_run's own verdict (--result) passes. Prints the
# interpreter's and the recompiled run's speed and the native share. Exits 77 (skipped) when ROM is absent, or
# python3 / cc are missing.
run=$1
rom=$2
work=$3
frames=$4
shift 4
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ ! -f "$rom" ]; then
    echo "skipped: $rom not built (tools/consoles/build_homebrew.sh n64)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
command -v "${CC:-cc}" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
rm -rf "$work"
elf="${rom%.z64}.elf"
cov=
[ -f "$elf" ] && cov="--elf $elf"
timeout 2400 "$py" "$repo/tools/consoles/n64_recomp_build.py" "$rom" "$work" --count --loop 5 --n64-run "$run" \
    --frames "$frames" --run-args="$*" $cov || exit 1
if [ -n "$cov" ]; then
    "$py" - "$work/report.json" <<'EOF' || exit 1
import json, sys
c = json.load(open(sys.argv[1]))["coverage"]
print("discovery: %d / %d ELF function symbols found (%.2f%%)" % (c["found"], c["symbols"], c["found_percent"]))
sys.exit(0 if c["found_percent"] >= 99.0 else 1)
EOF
fi
echo "interpreter:"
timeout 600 "$run" "$rom" --frames "$frames" --hash "$@" 2>&1 | grep -v "table\[" | tail -4
echo "recompiled (lockstep with the interpreter):"
out=$(timeout 1200 "$run" "$rom" --frames "$frames" --hash "$@" --pack "$work/libtitle.so" --lockstep --stats 2>&1)
rc=$?
echo "$out" | grep -v "table\[" | tail -7
echo "$out" | grep -q "^LOCKSTEP OK" || exit 1
echo "recompiled alone:"
timeout 600 "$run" "$rom" --frames "$frames" --hash "$@" --pack "$work/libtitle.so" 2>&1 | grep -v "table\[" | tail -3
exit $rc
