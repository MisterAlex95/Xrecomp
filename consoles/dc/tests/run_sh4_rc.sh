#!/bin/sh
# dc_rc_sh4_ops (XBA-227): dc_sh4_test's synthetic programs lifted by dc_tools.recomp, compiled, run by dc_run with
# the pack: the words each stores must be the expected ones (the interpreter's, checked by dc_sh4_ops), and the
# interpreter must not have run any instruction of the program. First, the C decoder (dc_run --decode-table) must
# agree with dc_tools.sh4 on all 65536 opcodes (sh4_optab.h is generated from sh4.py; this catches a stale header).
#   run_sh4_rc.sh DC_SH4_TEST DC_RUN WORKDIR
test_bin=$1
run=$2
work=$3
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
py=${PYTHON:-python3}
cc=${CC:-cc}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
command -v "$cc" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
rm -rf "$work"
mkdir -p "$work"
"$run" --decode-table "$work/decode_c.txt" || exit 1
PYTHONPATH="$repo/consoles/dc/tools" "$py" -m dc_tools.sh4 --table "$work/decode_py.txt" || exit 1
cmp "$work/decode_c.txt" "$work/decode_py.txt" || { echo "the C and Python decoders differ"; exit 1; }
echo "decoder: C and Python agree on 65536 opcodes"
"$test_bin" --write "$work" >/dev/null || exit 1
rc=0
for bin in "$work"/*.bin; do
    name=$(basename "$bin" .bin)
    nwords=$(tr ' ' '\n' <"$work/$name.expect" | grep -c '^[0-9A-F]\{8\}$')
    PYTHONPATH="$repo/consoles/dc/tools" timeout 120 "$py" -m dc_tools.recomp "$bin" "$work/$name.src" \
        --no-comments >/dev/null || exit 1
    "$cc" -shared -fPIC -O1 -ffp-contract=off ${RC_FLAGS:-} -DDC_RC_COUNT \
        -I "$repo/consoles/dc/runtime" -o "$work/$name.so" \
        "$work/$name.src"/*.c -lm || exit 1
    out=$(timeout 60 "$run" "$bin" --frames 2 --pack "$work/$name.so" --stats --peek "8C001000:$nwords")
    got=$(echo "$out" | sed -n '/^peek /,$p')
    interp=$(echo "$out" | sed -n 's/^stats: native [0-9]* interpreted \([0-9]*\).*/\1/p')
    if [ "$got" != "$(cat "$work/$name.expect")" ]; then
        echo "$name: lifted code stored other words"
        echo "  got:  $got"
        echo "  want: $(cat "$work/$name.expect")"
        rc=1
    elif [ "${interp:-1}" != 0 ]; then
        echo "$name: the interpreter ran $interp instruction(s)"
        rc=1
    else
        echo "$name: ok (lifted)"
    fi
done
exit $rc
