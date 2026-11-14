#!/bin/sh
# ctest helper (XBA-171): run_systemtest.sh N64_RUN ROM KNOWN HAVE_RCP [n64_run args...]. Runs lemmy-64/n64-systemtest
# under n64_run (the extra arguments, e.g. --pack LIB: the recompiled run, recomp_systemtest.sh),
# collects its failing tests from the IS-Viewer text ("Test 'NAME' ... failed") and passes when every failing test is
# a known gap: a line of KNOWN (a test name prefix; # comments), or, while the RSP / RDP are the stubs (HAVE_RCP not
# ON), a test of theirs (names starting "RSP ", "SP ", "RDP "). Prints the suite's summary and the counts. Exits 77
# when the ROM is not built (tools/consoles/build_n64_systemtest.sh, needs Docker).
run=$1
rom=$2
known=$3
rcp=$4
shift 4
if [ ! -f "$rom" ]; then
    echo "skipped: $rom not built (tools/consoles/build_n64_systemtest.sh)"
    exit 77
fi
out=$(timeout 240 "$run" "$rom" --tty --frames 1200 --quiet "$@" 2>&1)
summary=$(printf '%s\n' "$out" | grep -E "^Finished in|Failed [0-9]+ of" | head -n 3)
if [ -z "$summary" ]; then
    printf '%s\n' "$out" | tail -n 30
    echo "FAIL: n64-systemtest did not finish"
    exit 1
fi
echo "$summary"
names=$(printf '%s\n' "$out" | sed -n "s/^Test '\([^']*\)'.*/\1/p" | sort | uniq -c | sort -rn)
total=0
rcp_n=0
known_n=0
bad=""
IFS='
'
for line in $names; do
    count=$(echo "$line" | awk '{print $1}')
    name=$(echo "$line" | sed 's/^ *[0-9]* //')
    total=$((total + count))
    case "$name" in
    "RSP "* | "SP "* | "RDP "*)
        if [ "$rcp" != "ON" ]; then
            rcp_n=$((rcp_n + count))
            continue
        fi
        ;;
    esac
    hit=0
    for k in $(grep -v '^#' "$known" | grep -v '^ *$'); do
        case "$name" in "$k"*) hit=1 ;; esac
    done
    if [ $hit = 1 ]; then
        known_n=$((known_n + count))
    else
        bad="$bad
  $count x $name"
    fi
done
echo "failing values: $total ($rcp_n RSP / RDP with the stubs, $known_n known gaps)"
if [ -n "$bad" ]; then
    echo "FAIL: unexpected failures:$bad"
    exit 1
fi
echo "n64-systemtest: OK"
exit 0
