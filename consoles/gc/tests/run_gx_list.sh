#!/bin/sh
# ctest helper (XBA-262): run_gx_list.sh GC_RUN DOL WORKDIR
#
# The per-title shader list (layer 2 of the shader plan): a first run of DOL with --gx-list writes every TEV state
# (T lines) and vertex format (V lines) it met; a second run reads the list back, compiles those states before the
# first frame ("N states queued") and writes nothing new. Exits 77 like run_gx.sh when the DOL or GLES is missing.
# A cross build's binaries run under the emulator ctest uses (XBR_TEST_EMULATOR, comma separated: qemu-aarch64
# for the arm64 CI); empty on a native build.
emu=$(printf '%s' "${XBR_TEST_EMULATOR:-}" | tr ',' ' ')
run=$1
dol=$2
work=$3
if [ ! -f "$dol" ]; then
    echo "skipped: $dol not built (tools/consoles/build_homebrew.sh gc)"
    exit 77
fi
rm -rf "$work"
mkdir -p "$work"
list="$work/gx_shaders.txt"
LIBGL_ALWAYS_SOFTWARE=1 timeout 300 $emu "$run" "$dol" --frames 10 --gx --gx-list "$list" >"$work/run1.log" 2>&1
rc=$?
[ $rc -eq 77 ] && { cat "$work/run1.log"; exit 77; }
[ $rc -eq 0 ] || { cat "$work/run1.log"; exit 1; }
[ -f "$list" ] || { echo "no list written"; cat "$work/run1.log"; exit 1; }
t=$(grep -c '^T ' "$list")
v=$(grep -c '^V ' "$list")
echo "first run: $t TEV states, $v vertex formats"
[ "$t" -ge 5 ] && [ "$v" -ge 1 ] || { echo "too few states in the list"; exit 1; }
cp "$list" "$work/first.txt"
LIBGL_ALWAYS_SOFTWARE=1 timeout 300 $emu "$run" "$dol" --frames 10 --gx --gx-list "$list" >"$work/run2.log" 2>&1 || {
    cat "$work/run2.log"
    exit 1
}
grep -q "shader list .*: $t states queued" "$work/run2.log" || { echo "second run did not queue the $t states"; cat "$work/run2.log"; exit 1; }
cmp -s "$list" "$work/first.txt" || { echo "the second run changed the list"; diff "$work/first.txt" "$list" | head; exit 1; }
echo "second run: $t states compiled ahead, list unchanged"
