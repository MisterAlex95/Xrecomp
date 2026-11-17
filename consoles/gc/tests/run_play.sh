#!/bin/sh
# ctest helper (XBA-259): run_play.sh GC_PLAY DOL WORKDIR EXPECTED_PNG [gc_play options...]
#
# DOL through gc_runtime (the app's runtime: the host layer's frame loop, pads into the serial interface) headless
# and unpaced (SDL's dummy video driver), its last XFB written as a PNG and compared with EXPECTED_PNG
# (consoles/nds/tests/png_compare.py); the run must log the perf line. Exits 77 (skipped) when DOL is absent
# (tools/consoles/build_homebrew.sh gc) or python3 is missing.
# A cross build's binaries run under the emulator ctest uses (XBR_TEST_EMULATOR, comma separated: qemu-aarch64
# for the arm64 CI); empty on a native build.
emu=$(printf '%s' "${XBR_TEST_EMULATOR:-}" | tr ',' ' ')
play=$1
dol=$2
work=$3
expected=$4
shift 4
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ ! -f "$dol" ]; then
    echo "skipped: $dol not built (tools/consoles/build_homebrew.sh gc)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 120 $emu "$play" "$dol" --headless --unpaced \
    --screenshot "$work/xfb.png" "$@" >"$work/log.txt" 2>&1
rc=$?
cat "$work/log.txt"
[ "$rc" -eq 0 ] || { echo "gc_play failed ($rc)"; exit 1; }
grep -q "^gc: perf over the run" "$work/log.txt" || { echo "no perf line"; exit 1; }
exec "$py" "$repo/consoles/nds/tests/png_compare.py" "$work/xfb.png" "$expected"
