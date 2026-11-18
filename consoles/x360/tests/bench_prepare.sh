#!/bin/sh
# bench_prepare.sh WORKDIR [MB] [SAMPLES] -- what Prepare costs on a large Xbox 360 title (XBA-283), for a developer
# machine: make_big.py writes MB MB of generated code (default 12) as a synthetic XEX; x360_tools.recomp lifts it
# (time, peak RSS); SAMPLES generated files (default 3) and the resume table are compiled with tools/prepare/x360.py's
# flags (time, peak RSS per clang job), and the total is extrapolated from the C size. Needs GNU time (/usr/bin/time).
# CLANG: the compiler (default: the NDK's clang for aarch64-linux-android30 when ANDROID_NDK_HOME is set, else cc);
# in the Android image:
#
#   docker run --rm -v /usr/bin/time:/usr/bin/time:ro -v "$PWD:/src" -w /src xba-android:local \
#       sh consoles/x360/tests/bench_prepare.sh /src/build-bench 12
#
# (python3 in that image or on the host: the lift step can also run first on the host, the C stays in WORKDIR/gen.)
set -e
work=$1
mb=${2:-12}
samples=${3:-3}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
export PYTHONPATH="$repo:$repo/consoles/x360/tools"
mkdir -p "$work"
if [ -n "$CLANG" ]; then
    cc="$CLANG"
elif [ -n "$ANDROID_NDK_HOME" ]; then
    n=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64
    cc="$n/bin/clang --target=aarch64-linux-android30 --sysroot=$n/sysroot"
else
    cc=cc
fi
if [ ! -f "$work/gen/x360_rc_table.c" ]; then
    python3 "$here/make_big.py" "$work/big.xex" --mb "$mb"
    /usr/bin/time -f "lift: %e s, peak %M KB" python3 -m x360_tools.recomp "$work/big.xex" "$work/gen" --no-comments
fi
csize=$(cat "$work"/gen/x360_rc_[0-9]*.c | wc -c)
nfiles=$(ls "$work"/gen/x360_rc_[0-9]*.c | wc -l)
echo "generated: $nfiles files, $((csize / 1048576)) MB of C, table $(($(wc -c <"$work/gen/x360_rc_table.c") / 1048576)) MB"
mkdir -p "$work/obj"
total=0
done_bytes=0
for f in $(ls "$work"/gen/x360_rc_[0-9]*.c | head -n "$samples") "$work/gen/x360_rc_table.c"; do
    b=$(basename "$f" .c)
    # tools/prepare/x360.py compile_flags at the default -O1
    /usr/bin/time -o "$work/obj/$b.time" -f "%e %M" $cc -O1 -fno-tree-slp-vectorize -ffp-contract=off -DXE_RUNTIME=1 \
        -g0 -fPIC -std=gnu11 -w -DNDEBUG -I "$work/gen" -I "$repo/consoles/x360/runtime" \
        -I "$repo/consoles/gc/runtime" -c "$f" -o "$work/obj/$b.o"
    read -r secs kb <"$work/obj/$b.time"
    echo "  $b: $(($(wc -c <"$f") / 1024)) KB of C, $secs s, peak $((kb / 1024)) MB"
    if [ "$b" != x360_rc_table ]; then
        total=$(awk "BEGIN { print $total + $secs }")
        done_bytes=$((done_bytes + $(wc -c <"$f")))
    fi
done
echo "compile: about $(awk "BEGIN { printf \"%d\", $total * $csize / $done_bytes }") s of one core for the whole title" \
    "(from $samples files)"
