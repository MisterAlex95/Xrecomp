#!/bin/sh
# ctest helper (XBA-280): run_play.sh X360_PLAY WORKDIR -- the synthetic test title (make_hello.py) recompiled and
# packed as run_hello.sh does, then played by x360_play headless (SDL's dummy drivers), unpaced: the runtime must
# start it with its pack, run it frame by frame (x360_run_until) to its exit code 0, keep the title's DbgPrint text,
# turn save states on, write the WAV, and log the perf line and the run's summary. Exits 77 without python3 or cc.
play=$1
work=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
rm -rf "$work"
mkdir -p "$work/disc/data" "$work/saves" "$work/work"
export PYTHONPATH="$repo:$repo/consoles/x360/tools"
"$py" "$here/make_hello.py" "$work/hello.xex" --compression basic --linger 2000 || exit 1
printf 'Hello from the disc!' >"$work/disc/data/hello.txt"
printf 'binary data' >"$work/disc/data/b.bin"
printf 'readme\n' >"$work/disc/readme.txt"
timeout 300 "$py" -m x360_tools.recomp "$work/hello.xex" "$work/src" --no-comments >"$work/recomp.txt" || {
    cat "$work/recomp.txt"; exit 1; }
"$cc" -shared -fPIC -O1 -ffp-contract=off -DXE_RUNTIME=1 -I "$repo/consoles/x360/runtime" \
    -I "$repo/consoles/gc/runtime" -o "$work/libtitle.so" "$work"/src/*.c -lm || exit 1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 120 "$play" "$work/hello.xex" --pack "$work/libtitle.so" \
    --disc "$work/disc" --headless --unpaced --frames 600 --save-dir "$work/saves" --work "$work/work" \
    --wav "$work/out.wav" >"$work/out.txt" 2>&1
rc=$?
cat "$work/out.txt"
[ "$rc" -eq 0 ] || { echo "x360_play exited with $rc"; exit 1; }
for want in "the title ended at frame" "exit code 0" "x360: perf" "hello from main()" "save states in" \
            "audio frames written"; do
    grep -q "$want" "$work/out.txt" || { echo "missing from the output: $want"; exit 1; }
done
# main sleeps 2 s of the title's clock (--linger): about 120 frames of 1/60 s, one x360_run_until each
ended=$(sed -n 's/.*the title ended at frame \([0-9]*\).*/\1/p' "$work/out.txt")
[ -n "$ended" ] && [ "$ended" -ge 110 ] && [ "$ended" -le 130 ] || { echo "ended at frame '$ended', want ~120"; exit 1; }
echo "x360_play ran the test title frame by frame to its end (frame $ended)"
