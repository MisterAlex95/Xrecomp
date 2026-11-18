#!/bin/sh
# ctest helper (XBA-270): run_draw.sh XENOS_DRAW_TEST WORKDIR -- make_draw.py's shaders and shader cache, then the
# renderer test on them. Exits 77 when python3 is missing or EGL / GLES 3.0 cannot be had (the test's own 77).
test=$1
work=$2
here=$(cd "$(dirname "$0")" && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
"$py" "$here/make_draw.py" "$work" || exit 1
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 "$test" "$work"
