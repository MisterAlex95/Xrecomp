#!/bin/sh
# ctest helper (XBA-264): run_gx_programs.sh GC_RUN DOL WORKDIR [EXPECTED_PNG]
#
# Program binaries (layer 3 of the shader plan), on llvmpipe:
#  1. a run of DOL writes the shader list; gc_run --gx-warmup compiles it into gx_programs.bin (every state compiled),
#     a second warm-up finds them all cached;
#  2. a run with list + cache links every state from its binary ("N loaded, 0 rebuilt", "N ready from program
#     binaries": nothing compiled from source at the start) and leaves the file alone; drawn with those programs
#     only (--gx-shaders special), the XFB matches EXPECTED_PNG (gx_compare.py);
#  3. another driver (GC_GX_DRIVER_KEY, a pretend update) or a corrupt file: ignored, rebuilt at the end of the run,
#     loaded whole by the next;
#  4. no list and no warm-up: the programs compiled during play are written at the end, loaded by the next run.
# Exits 77 like run_gx.sh when the DOL or GLES is missing, and when the driver has no program binaries.
# A cross build's binaries run under the emulator ctest uses (XBR_TEST_EMULATOR, comma separated: qemu-aarch64
# for the arm64 CI); empty on a native build.
emu=$(printf '%s' "${XBR_TEST_EMULATOR:-}" | tr ',' ' ')
run=$1
dol=$2
work=$3
expected=$4
if [ ! -f "$dol" ]; then
    echo "skipped: $dol not built (tools/consoles/build_homebrew.sh gc)"
    exit 77
fi
rm -rf "$work"
mkdir -p "$work"
list="$work/gx_shaders.txt"
cache="$work/gx_programs.bin"
export LIBGL_ALWAYS_SOFTWARE=1
unset GC_GX_DRIVER_KEY GC_GX_PROGRAM_CACHE GC_GX_SHADER_LIST
fail() {
    echo "FAIL: $1"
    [ -n "$2" ] && cat "$2"
    exit 1
}
gx() { # gx LOG ARGS...: one run of the DOL
    log=$1
    shift
    timeout 300 $emu "$run" "$dol" --frames 10 --gx "$@" >"$log" 2>&1
    rc=$?
    [ $rc -eq 77 ] && { cat "$log"; exit 77; }
    [ $rc -eq 0 ] || fail "gc_run exited $rc" "$log"
}

# 1. the list, then the warm-up
gx "$work/list.log" --gx-list "$list"
t=$(grep -c '^T ' "$list")
[ "$t" -ge 5 ] || fail "too few states in the list ($t)" "$work/list.log"
timeout 300 $emu "$run" --gx-warmup "$work/none.txt,$list" "$cache" >"$work/warm1.log" 2>&1
rc=$?
[ $rc -eq 77 ] && { cat "$work/warm1.log"; exit 77; }
if [ $rc -eq 3 ]; then
    cat "$work/warm1.log"
    echo "skipped: this driver has no program binaries (GL_NUM_PROGRAM_BINARY_FORMATS 0)"
    exit 77
fi
[ $rc -eq 0 ] || fail "warm-up exited $rc" "$work/warm1.log"
grep -q "gx warm-up: rc 0, $t states, 0 cached, $t compiled, 0 failed" "$work/warm1.log" ||
    fail "the warm-up did not compile the $t states" "$work/warm1.log"
[ -f "$cache" ] || fail "no cache written" "$work/warm1.log"
timeout 300 $emu "$run" --gx-warmup "$list" "$cache" >"$work/warm2.log" 2>&1 || fail "second warm-up" "$work/warm2.log"
grep -q "gx warm-up: rc 0, $t states, $t cached, 0 compiled" "$work/warm2.log" ||
    fail "the second warm-up compiled again" "$work/warm2.log"
echo "warm-up: $t states compiled into $(wc -c <"$cache") bytes, then all cached"

# 2. a start with every state ready
cp "$cache" "$work/warm.bin"
gx "$work/run2.log" --gx-list "$list" --gx-programs "$cache"
grep -q "program binaries .*: $t loaded, 0 rebuilt (driver " "$work/run2.log" || fail "not all $t loaded" "$work/run2.log"
grep -q "shader list .*: $t states queued ($t ready from program binaries)" "$work/run2.log" ||
    fail "states compiled from source at the start" "$work/run2.log"
grep -q "programs written" "$work/run2.log" && fail "the cache was rewritten with nothing new" "$work/run2.log"
cmp -s "$cache" "$work/warm.bin" || fail "the cache changed"
if [ -n "$expected" ] && command -v python3 >/dev/null 2>&1; then
    cp "$cache" "$work/special.bin"
    timeout 300 $emu "$run" "$dol" --frames 40 --gx --png "$work/xfb.png" --gx-programs "$work/special.bin" \
        --gx-shaders special >"$work/special.log" 2>&1 || fail "the run drawn with the cached programs" "$work/special.log"
    grep -q "program binaries .*: $t loaded" "$work/special.log" || fail "special: not loaded" "$work/special.log"
    python3 "$(dirname "$0")/gx_compare.py" "$work/xfb.png" "$expected" || fail "the picture drawn by the cached programs"
fi
echo "start: $t programs from binaries, none compiled"

# 3. another driver, then a corrupt file (rebuilding runs compile on first use, --gx-shaders sync: every state is
#    ready at the end, where an async run may end with some still queued)
GC_GX_DRIVER_KEY=update gx "$work/run3.log" --gx-list "$list" --gx-programs "$cache" --gx-shaders sync
grep -q "program binaries .*: ignored (another driver or shader generator; was .*), rebuilt from scratch" \
    "$work/run3.log" || fail "a cache of another driver was not ignored" "$work/run3.log"
grep -q "program binaries .*: $t programs written ($t new)" "$work/run3.log" || fail "not rebuilt" "$work/run3.log"
GC_GX_DRIVER_KEY=update gx "$work/run4.log" --gx-list "$list" --gx-programs "$cache"
grep -q "program binaries .*: $t loaded, 0 rebuilt" "$work/run4.log" || fail "the rebuilt cache" "$work/run4.log"
gx "$work/run5.log" --gx-list "$list" --gx-programs "$cache" --gx-shaders sync
grep -q "ignored (another driver" "$work/run5.log" || fail "back on the first driver" "$work/run5.log"
size=$(wc -c <"$cache")
printf 'Z' | dd of="$cache" bs=1 seek=$((size / 2)) conv=notrunc 2>/dev/null
gx "$work/run6.log" --gx-list "$list" --gx-programs "$cache" --gx-shaders sync
grep -q "program binaries .*: ignored (checksum), rebuilt from scratch" "$work/run6.log" ||
    fail "a corrupt cache was not ignored" "$work/run6.log"
gx "$work/run7.log" --gx-list "$list" --gx-programs "$cache"
grep -q "program binaries .*: $t loaded, 0 rebuilt" "$work/run7.log" || fail "after the corrupt one" "$work/run7.log"
echo "driver change and corrupt file: ignored, rebuilt, loaded"

# 4. programs compiled during play, no list (sync, as above)
fresh="$work/fresh.bin"
gx "$work/run8.log" --gx-programs "$fresh" --gx-shaders sync
grep -q "program binaries .*: none yet" "$work/run8.log" || fail "a missing cache" "$work/run8.log"
n=$(sed -n 's/.*program binaries .*: \([0-9]*\) programs written (\1 new).*/\1/p' "$work/run8.log")
[ -n "$n" ] && [ "$n" -ge 5 ] || fail "the programs of the run were not written" "$work/run8.log"
gx "$work/run9.log" --gx-programs "$fresh"
grep -q "program binaries .*: $n loaded, 0 rebuilt" "$work/run9.log" || fail "the run's programs" "$work/run9.log"
echo "during play: $n programs written at the end, loaded by the next run"
