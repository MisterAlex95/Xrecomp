#!/bin/sh
# ctest helper (XBA-283): run_host_threads.sh X360_RUN WORKDIR HOMEBREW_DIR -- guest threads on host threads
# (x360_run --host-threads, experimental). The devkitPPC titles (tools/consoles/build_homebrew.sh x360; skipped (77)
# when not built): atomics (four threads on shared counters with lwarx / stwcx. and ldarx / stdcx. loops and a spin
# lock) must print atomics.txt cooperatively and, three times, in parallel; hello_c (two workers, a critical section,
# events) must print hello_c.txt in parallel. A pack without atomic reservations (no xe_rc_features) is refused
# (exit 2). Exits 77 when python3 or cc is missing.
run=$1
work=$2
hb=$3
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
for p in atomics hello_c; do
    [ -f "$hb/$p.elf" ] || { echo "skipped: $hb/$p.elf not built (tools/consoles/build_homebrew.sh x360)"; exit 77; }
done
rm -rf "$work"
mkdir -p "$work/disc/data"
printf 'Hello from the disc!' >"$work/disc/data/hello.txt"
printf 'binary data' >"$work/disc/data/b.bin"
printf 'readme\n' >"$work/disc/readme.txt"
export PYTHONPATH="$repo:$repo/consoles/x360/tools"
pack() { # pack NAME
    "$py" "$here/homebrew/elf2xex.py" "$hb/$1.elf" "$work/$1.xex" || exit 1
    "$py" -m x360_tools.recomp "$work/$1.xex" "$work/$1.src" --no-comments >/dev/null || exit 1
    "$cc" -shared -fPIC -O1 -ffp-contract=off -DXE_RUNTIME=1 -I "$repo/consoles/x360/runtime" \
        -I "$repo/consoles/gc/runtime" -o "$work/$1.so" "$work/$1.src"/*.c -lm || exit 1
}
check() { # check NAME EXPECTED LABEL [x360_run options]
    name=$1 expected=$2 label=$3
    shift 3
    timeout 120 "$run" "$work/$name.xex" --pack "$work/$name.so" --disc "$work/disc" --dbg "$work/$label.dbg" \
        --log "$work/$label.log" --stats "$@"
    rc=$?
    [ "$rc" = 0 ] || { echo "$label: exit $rc"; tail -n 5 "$work/$label.log"; exit 1; }
    cmp -s "$work/$label.dbg" "$expected" || {
        echo "$label: DbgPrint output differs:"; diff "$expected" "$work/$label.dbg"; exit 1; }
    echo "$label: ok ($(grep -o 'switches [0-9]*' "$work/$label.log"))"
}
pack atomics
pack hello_c
check atomics "$here/atomics.txt" atomics-cooperative
for i in 1 2 3; do
    check atomics "$here/atomics.txt" "atomics-host-$i" --host-threads
done
check hello_c "$here/hello_c.txt" hello_c-host --host-threads
# a pack made before XBA-283 (no xe_rc_features): refused
grep -v xe_rc_features "$work/atomics.src/x360_rc_table.c" >"$work/old_table.c"
"$cc" -shared -fPIC -O1 -ffp-contract=off -DXE_RUNTIME=1 -I "$repo/consoles/x360/runtime" \
    -I "$repo/consoles/gc/runtime" -o "$work/old.so" "$work"/atomics.src/x360_rc_0*.c "$work/old_table.c" -lm || exit 1
timeout 60 "$run" "$work/atomics.xex" --pack "$work/old.so" --host-threads >"$work/old.txt" 2>&1
rc=$?
[ "$rc" = 2 ] || { echo "an old pack should be refused with --host-threads (exit $rc)"; cat "$work/old.txt"; exit 1; }
echo "host threads: exact totals in parallel, hello_c the same, old packs refused"
