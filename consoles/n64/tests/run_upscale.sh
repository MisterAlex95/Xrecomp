#!/bin/sh
# ctest helper (XBA-185): run_upscale.sh N64_RUN ROM WORKDIR. gfx.z64 on the Vulkan RDP at 1x and at 2x
# (RECOMP_UPSCALE=2): the 2x run must start upscaled, pass, and write a 1280 x 480 picture (640 VI pixels x 2,
# 240 lines x 2) that matches the 1x one sampled at the same places (upscale_compare.py). Skipped (77) when the ROM
# is not built (tools/consoles/build_homebrew.sh n64) or there is no Vulkan device (lavapipe without a GPU:
# VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json).
run=$1
rom=$2
work=$3
here=$(dirname "$0")
if [ ! -f "$rom" ]; then
    echo "skipped: $rom not built (tools/consoles/build_homebrew.sh n64)"
    exit 77
fi
mkdir -p "$work" || exit 1
for f in 1 2; do
    RECOMP_N64_RDP=vulkan RECOMP_N64_UPSCALE=$f timeout 150 "$run" "$rom" --result --frames 900 --quiet \
        --ppm "$work/up$f.ppm" > "$work/up$f.log" 2>&1
    rc=$?
    cat "$work/up$f.log"
    if grep -q "software RDP instead" "$work/up$f.log"; then
        echo "skipped: no Vulkan device for the Vulkan RDP"
        exit 77
    fi
    [ $rc -eq 0 ] || { echo "FAIL: gfx.z64 at ${f}x (exit $rc)"; exit 1; }
done
grep -q "upscale 2x, scanout 2x" "$work/up2.log" || { echo "FAIL: the 2x run did not start upscaled"; exit 1; }
size=$(head -c 16 "$work/up2.ppm" | sed -n 2p)
[ "$size" = "1280 480" ] || { echo "FAIL: 2x picture is '$size', want '1280 480'"; exit 1; }
PY=${PYTHON:-python3}
if ! command -v "$PY" > /dev/null 2>&1; then
    echo "PASS (size only: no python3 for the comparison)"
    exit 0
fi
"$PY" -I "$here/upscale_compare.py" "$work/up1.ppm" "$work/up2.ppm" || exit 1
echo PASS
