#!/bin/sh
# ctest helper (XBA-197): expect_output.sh 'TEXT1|TEXT2|...' COMMAND [ARGS...]. Runs the command (one of the consoles'
# run_rom.sh / run_exe.sh wrappers, which exit 77 when the homebrew is not built) and passes when it exits 0 and its
# output holds every TEXT (fixed strings, '|'-separated). Exit 77 is passed on (skipped).
want=$1
shift
out=$("$@" 2>&1)
rc=$?
printf '%s\n' "$out"
[ "$rc" -eq 77 ] && exit 77
if [ "$rc" -ne 0 ]; then
    echo "expect_output: exit $rc"
    exit 1
fi
fail=0
old_ifs=$IFS
IFS='|'
for t in $want; do
    case "$out" in
    *"$t"*) ;;
    *)
        echo "expect_output: missing \"$t\""
        fail=1
        ;;
    esac
done
IFS=$old_ifs
exit $fail
