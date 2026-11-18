#!/bin/sh
# ctest helper (XBA-274): run_framegen.sh GC_PLAY DOL WORKDIR
#
# Frame generation of a picture GX draws on the GPU (host_window_present_texture reads it back for the pacer): DOL
# shown twice in a window, once as usual and once under RECOMP_LSFG=1 with the fake generator. The drawable is
# captured (host_window_capture) at the second field without frame generation and at the first picture the pacer's
# FIFO brings with it: the same bytes (same orientation, same colours), and the run's [LSFG] line counts the pictures
# GX presented. Needs a display (Xvfb works: xvfb-run -a ctest -R gc_framegen) and a build with frame generation
# (XBOXRECOMP_LSFG); exits 77 (skipped) without one, or when DOL is absent (tools/consoles/build_homebrew.sh gc).
play=$1
dol=$2
work=$3
if [ ! -f "$dol" ]; then
    echo "skipped: $dol not built (tools/consoles/build_homebrew.sh gc)"
    exit 77
fi
if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    echo "skipped: no display (xvfb-run -a ctest -R gc_framegen)"
    exit 77
fi
rm -rf "$work"
mkdir -p "$work"
SDL_AUDIODRIVER=dummy timeout 120 "$play" "$dol" --frames 320 --capture 300 "$work/direct.ppm" \
    >"$work/direct.txt" 2>&1 || { cat "$work/direct.txt"; echo "gc_play failed"; exit 1; }
RECOMP_LSFG=1 RECOMP_LSFG_FAKE=1 SDL_AUDIODRIVER=dummy timeout 120 "$play" "$dol" --frames 320 --capture 2 \
    "$work/fg.ppm" >"$work/fg.txt" 2>&1 || { cat "$work/fg.txt"; echo "gc_play (frame generation) failed"; exit 1; }
cat "$work/fg.txt"
if ! grep -q "^\[LSFG\] console window" "$work/fg.txt"; then
    echo "skipped: built without frame generation (XBOXRECOMP_LSFG)"
    exit 77
fi
grep -q "XFB to RGB on the GPU" "$work/fg.txt" || { echo "skipped: no GX renderer here"; exit 77; }
grep "^\[LSFG\] console at close" "$work/fg.txt" | grep -q "pictures 320 " \
    || { echo "the pacer did not get every GX picture"; exit 1; }
[ -f "$work/fg.ppm" ] || { echo "nothing drawn from the frame generation FIFO"; exit 1; }
cmp -s "$work/direct.ppm" "$work/fg.ppm" || { echo "the read-back picture differs from the one GX drew"; exit 1; }
echo "gc_framegen: ok"
