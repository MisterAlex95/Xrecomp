#!/bin/sh
# ctest helper (XBA-282): run_seh.sh X360_RUN WORKDIR EXPECTED_TXT -- the exception test title (make_seh.py:
# __try / __except and __try / __finally scopes under the kernel's __C_specific_handler, a noncontinuable raise
# unwound to main's __except block, a continuable one resumed by its filter), recompiled, built into a pack and run
# under x360_run; its DbgPrint lines must equal EXPECTED_TXT and it must end with exit code 0. Exits 77 when python3
# or cc is missing.
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
mkdir -p "$work"
export PYTHONPATH="$repo:$repo/consoles/x360/tools"
"$py" "$here/make_seh.py" "$work/seh.xex" || exit 1
timeout 300 "$py" -m x360_tools.recomp "$work/seh.xex" "$work/src" --no-comments >"$work/recomp.txt" || {
    cat "$work/recomp.txt"; exit 1; }
cat "$work/recomp.txt"
# X360_HOST_PAGE (the *_16k tests): a pack that follows the host's page size, on x86 too (xenon_rt.h)
"$cc" -shared -fPIC -O1 -ffp-contract=off -DXE_RUNTIME=1 ${X360_HOST_PAGE:+-DXE_VIEW4K_DYNAMIC=1} \
    -I "$repo/consoles/x360/runtime" \
    -I "$repo/consoles/gc/runtime" -o "$work/libtitle.so" "$work"/src/*.c -lm || exit 1
timeout 120 "$run" "$work/seh.xex" --pack "$work/libtitle.so" --dbg "$work/dbg.txt" --log "$work/log.txt" --stats \
    --missed "$work/missed.txt"
rc=$?
cat "$work/log.txt"
[ "$rc" -eq 0 ] || { echo "x360_run exited with $rc"; exit 1; }
if cmp -s "$work/dbg.txt" "$expected"; then
    echo "DbgPrint output matches $(basename "$expected") ($(wc -l <"$expected") lines)"
    exit 0
fi
echo "DbgPrint output differs from $expected:"
diff "$expected" "$work/dbg.txt" | head -n 40
exit 1
