#!/bin/sh
# ctest helper (XBA-270): run_draw.sh TEST WORKDIR [--render] -- make_draw.py's shaders and shader cache, then the
# renderer test on them (xenos_draw_test WORKDIR, or x360_hle_test --render WORKDIR). Exits 77 when python3 is
# missing or EGL / GLES 3.0 cannot be had (the test's own 77).
test=$1
work=$2
here=$(cd "$(dirname "$0")" && pwd)
py=${PYTHON:-python3}
# A cross build's binaries run under the emulator ctest uses (XBR_TEST_EMULATOR, comma separated: qemu-aarch64 in the
# arm64 CI); ctest's own emulator wraps only this script's sh.
emu=$(printf '%s' "${XBR_TEST_EMULATOR:-}" | tr ',' ' ')
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
"$py" "$here/make_draw.py" "$work" || exit 1
if [ "${3:-}" = "--render" ]; then
    EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 $emu "$test" --render "$work"
else
    EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 $emu "$test" "$work"
fi
