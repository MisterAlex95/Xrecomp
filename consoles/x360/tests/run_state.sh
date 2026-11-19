#!/bin/sh
# ctest helper (XBA-270): run_state.sh X360_RUN WORKDIR EXPECTED_TXT -- save states: the test title runs until an
# instruction budget stops it and a state is saved, then a fresh x360_run loads the state and finishes; the two runs'
# DbgPrint text together must equal EXPECTED_TXT, for several stopping points (with threads waiting, files on the
# disc, the scheduler between slices, a pad script). Exits 77 when python3 or cc is missing.
run=$1
work=$2
expected=$3
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
rm -rf "$work"
mkdir -p "$work/disc/data"
printf 'Hello from the disc!' >"$work/disc/data/hello.txt"
printf 'binary data' >"$work/disc/data/b.bin"
printf 'readme\n' >"$work/disc/readme.txt"
printf '0 0 1000 # A on pad 0\n2 0 0 0 0 -32768 32767 0 0\n4 1 8000\n6 0 -\n' >"$work/pads.txt"
export PYTHONPATH="$repo:$repo/consoles/x360/tools"
"$py" "$here/make_hello.py" "$work/hello.xex" --compression basic || exit 1
"$py" -m x360_tools.recomp "$work/hello.xex" "$work/src" --no-comments >/dev/null || exit 1
# X360_HOST_PAGE (the *_16k tests): a pack that follows the host's page size, on x86 too (xenon_rt.h)
"$cc" -shared -fPIC -O1 -ffp-contract=off -DXE_RUNTIME=1 ${X360_HOST_PAGE:+-DXE_VIEW4K_DYNAMIC=1} \
    -I "$repo/consoles/x360/runtime" \
    -I "$repo/consoles/gc/runtime" -o "$work/libtitle.so" "$work"/src/*.c -lm || exit 1
timeout 60 "$run" "$work/hello.xex" --pack "$work/libtitle.so" --disc "$work/disc" --pad-script "$work/pads.txt" \
    --stats --dbg "$work/full.txt" \
    --log "$work/full.log" || exit 1
# instructions, HLE calls and the clock carry over a state; thread switches and threads created are per process
want=$(grep '^stats' "$work/full.log" | sed 's/ switches [0-9]*//; s/ threads [0-9]*//')
for budget in 1 300 900 1500 2100; do
    rm -f "$work/s.st"
    timeout 60 "$run" "$work/hello.xex" --pack "$work/libtitle.so" --disc "$work/disc" --pad-script "$work/pads.txt" \
        --insns "$budget" \
        --save-state "$work/s.st" --dbg "$work/a.txt" --log "$work/a.log"
    [ -f "$work/s.st" ] || { echo "budget $budget: no state saved"; cat "$work/a.log"; exit 1; }
    timeout 60 "$run" "$work/hello.xex" --pack "$work/libtitle.so" --disc "$work/disc" --pad-script "$work/pads.txt" \
        --load-state "$work/s.st" \
        --stats --dbg "$work/b.txt" --log "$work/b.log" || { echo "budget $budget: the resumed run failed"
        cat "$work/b.log"; exit 1; }
    got=$(grep '^stats' "$work/b.log" | sed 's/ switches [0-9]*//; s/ threads [0-9]*//')
    [ "$want" = "$got" ] || { echo "budget $budget: the resumed run's counts differ from an uninterrupted run's:"
        echo "  want $want"; echo "  got  $got"; exit 1; }
    cat "$work/a.txt" "$work/b.txt" >"$work/ab.txt"
    if ! cmp -s "$work/ab.txt" "$expected"; then
        echo "budget $budget: the two halves differ from $(basename "$expected"):"
        diff "$expected" "$work/ab.txt" | head -20
        exit 1
    fi
    echo "budget $budget: $(wc -l <"$work/a.txt") + $(wc -l <"$work/b.txt") lines, state $(wc -c <"$work/s.st") bytes"
done
echo "save states: every stopping point resumes to the same text"
