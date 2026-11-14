#!/bin/sh
# ctest helper (XBA-162 / XBA-164): run_jcz.sh PS1_RUN BIOS EXE [EXPECT | -IGNORED]... Runs one of JaCzekanski's
# ps1-tests (tools/consoles/fetch_ps1_tests.sh) under ps1_run until it prints "Done.", and passes when its console text
# has no "fail -" / "Assertion failed" line and contains every EXPECT string. -NAME ignores the failures of the checks
# whose name contains NAME (checks the release has but the hardware log in the ps1-tests repository does not). Exits
# 77 (skipped) when the EXE or the BIOS is not there. The console text is printed, so ctest --output-on-failure shows the failing checks.
run=$1
bios=$2
exe=$3
shift 3
if [ "$bios" != hle ] && [ ! -f "$bios" ]; then
    echo "skipped: BIOS $bios not built (tools/consoles/build_ps1_bios.sh)"
    exit 77
fi
if [ ! -f "$exe" ]; then
    echo "skipped: $exe not fetched (tools/consoles/fetch_ps1_tests.sh)"
    exit 77
fi
out=$(timeout 280 "$run" --bios "$bios" --exe "$exe" --tty --until-tty "Done." --frames 12000 --quiet)
rc=$?
printf '%s\n' "$out"
if [ $rc -ne 0 ]; then
    echo "FAIL: no \"Done.\" (ps1_run exit $rc)"
    exit 1
fi
fails=$(printf '%s\n' "$out" | grep -e '^fail - ' -e 'Assertion failed')
for e in "$@"; do
    case "$e" in
    -*) fails=$(printf '%s\n' "$fails" | grep -v -F -e "${e#-}") ;;
    esac
done
if [ -n "$fails" ]; then
    echo "FAIL: failing checks above"
    exit 1
fi
for e in "$@"; do
    case "$e" in -*) continue ;; esac
    if ! printf '%s\n' "$out" | grep -q -F -e "$e"; then
        echo "FAIL: \"$e\" not in the output"
        exit 1
    fi
done
echo "PASS: $(printf '%s\n' "$out" | grep -c '^pass - ') checks passed"
