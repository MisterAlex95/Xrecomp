#!/bin/sh
# ctest helper (XBA-270): run_draw.sh TEST WORKDIR [--render] -- make_draw.py's shaders and shader cache, then the
# renderer test on them (xenos_draw_test WORKDIR, or x360_hle_test --render WORKDIR). Exits 77 when python3 is
# missing or EGL / GLES 3.0 cannot be had (the test's own 77).
test=$1
work=$2
here=$(cd "$(dirname "$0")" && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
"$py" "$here/make_draw.py" "$work" || exit 1
if [ "${3:-}" = "--render" ]; then
    EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 "$test" --render "$work"
else
    EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 "$test" "$work"
fi
