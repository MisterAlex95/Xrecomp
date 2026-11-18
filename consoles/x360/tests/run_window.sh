#!/bin/sh
# ctest helper (XBA-280): run_window.sh X360_WINDOW_TEST WORKDIR -- make_draw.py's shaders, then the renderer on a
# window's context (x360_window_test). Needs a display (xvfb-run -a ctest -R x360_window); exits 77 without one.
test=$1
work=$2
here=$(cd "$(dirname "$0")" && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    echo "skipped: no display (xvfb-run -a ctest -R x360_window)"
    exit 77
fi
rm -rf "$work"
mkdir -p "$work"
"$py" "$here/make_draw.py" "$work" || exit 1
SDL_AUDIODRIVER=dummy "$test" "$work" "$work/window.ppm"
