#!/bin/sh
# ctest helper (XBA-270): run_bringup.sh X360_RUN WORKDIR -- the one-command path Alex runs on his own dumps
# (python3 -m x360_tools.bringup) on the encrypted, LZX-compressed test title: parse with the (made-up) key, write
# it plain, imports covered, sweep, recompile, pack, run; the summary must say every import is covered and the
# title ended with exit code 0, and must not contain the title's own text. Exits 77 when python3 or cc is missing.
run=$1
work=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
command -v "${CC:-cc}" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
rm -rf "$work"
mkdir -p "$work/disc/data"
printf 'Hello from the disc!' >"$work/disc/data/hello.txt"
printf 'binary data' >"$work/disc/data/b.bin"
printf 'readme\n' >"$work/disc/readme.txt"
printf 'xba-270-made-up!' >"$work/test.key"
export PYTHONPATH="$repo:$repo/consoles/x360/tools"
"$py" "$here/make_hello.py" "$work/enc.xex" --compression normal --key-file "$work/test.key" || exit 1
timeout 600 "$py" -m x360_tools.bringup "$work/enc.xex" "$work/out" --key-file "$work/test.key" --disc "$work/disc" \
    --x360-run "$run" || exit 1
s=$work/out/summary.txt
grep -q "missing: 0" "$s" || { echo "imports not all covered"; exit 1; }
grep -q "the title ended (exit code 0)" "$s" || { echo "the title did not end"; exit 1; }
if grep -q "Hello from the disc" "$s"; then
    echo "the summary holds the title's own text"
    exit 1
fi
echo "bringup: the summary is complete and holds no title text"
