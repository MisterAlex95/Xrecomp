#!/usr/bin/env bash
# Builds every self-contained C test project under tests/ and runs its ctest.
# The root build only lists a couple of them; this covers the whole directory,
# then consoles/<name>/tests (XBA-146) as console_<name>.
#
#   tools/run_c_tests.sh [build-dir] [report.md]
#
# XBR_CMAKE_ARGS adds configure arguments, e.g. a toolchain file:
#   XBR_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64-cross.cmake" tools/run_c_tests.sh
#
# Each project is configured, built and tested on its own, so one broken
# project does not hide the others. A ctest skip (exit 77, SKIP_RETURN_CODE)
# counts as skipped. The report is a Markdown table: project, configure,
# build, tests passed / skipped / failed. Exit status: 0 when nothing failed.
#
# XBR_BUILD_ONLY=1 configures and builds each project and runs no test (the macOS build check: its jobs run in a
# desktop session and must not open a window). The report then shows "-" for the test columns.
#
# XBR_JUNIT_DIR=dir also writes ctest's JUnit file per project, dir/<project>.xml (ctest 3.21+), which
# tools/ci/check_expected.py reads (the arm64 job, XBA-53).
#
# Runs on macOS's bash 3.2: an empty array is expanded as ${a[@]+"${a[@]}"}, since "${a[@]}" of an empty
# array is an unbound variable there under set -u. Tests that open an SDL audio device stay silent unless
# SDL_AUDIODRIVER says otherwise.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/c-tests}
REPORT=${2:-$OUT/report.md}
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
GEN=()
read -r -a EXTRA <<< "${XBR_CMAKE_ARGS:-}"
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
JUNIT_DIR=${XBR_JUNIT_DIR:-}
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
[ -z "$JUNIT_DIR" ] || { mkdir -p "$JUNIT_DIR" && JUNIT_DIR=$(cd "$JUNIT_DIR" && pwd); }   # ctest: relative to the build dir

# Projects that only build for Windows (a FATAL_ERROR under if(NOT WIN32), or an unconditional <windows.h>).
# Listed by hand on purpose: a new Windows-only project fails the run until someone looks at it and adds it here.
WINDOWS_ONLY=" apu_vp fb_present idex_channel input_xinput_imports kernel_dispatch kernel_irql_abi
               kernel_physical_address kernel_regressions mmio_decode nv2a_pvideo xaudio2 "
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS_ONLY="" ;; esac

# The console runtimes' test projects (consoles/<name>/tests, XBA-146), reported as console_<name>: POSIX hosts only,
# like the host layer they link (xbr_host).
CONSOLE_TESTS=()
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) for dir in "$ROOT"/consoles/*/tests/; do [ -f "$dir/CMakeLists.txt" ] && CONSOLE_TESTS+=("$dir"); done ;;
esac

mkdir -p "$OUT"
fail=0
{
    echo "| Project | Configure | Build | Passed | Skipped | Failed |"
    echo "|---|---|---|---|---|---|"
} > "$REPORT"

for dir in "$ROOT"/tests/*/ ${CONSOLE_TESTS[@]+"${CONSOLE_TESTS[@]}"}; do
    name=$(basename "$dir")
    case "$dir" in "$ROOT"/consoles/*) name=console_$(basename "$(dirname "$dir")") ;; esac
    [ -f "$dir/CMakeLists.txt" ] || continue
    bdir="$OUT/$name"
    log="$OUT/$name.log"
    conf=ok; build=ok; pass=0; skip=0; failed=0
    if [[ "$WINDOWS_ONLY" =~ [[:space:]]$name[[:space:]] ]]; then
        echo "| $name | windows | - | - | - | - |" >> "$REPORT"
        printf '%-32s windows only\n' "$name"
        continue
    fi
    if ! cmake -S "$dir" -B "$bdir" ${GEN[@]+"${GEN[@]}"} -DCMAKE_BUILD_TYPE=RelWithDebInfo ${EXTRA[@]+"${EXTRA[@]}"} >"$log" 2>&1; then
        conf=FAIL; build=-; fail=1
    elif ! cmake --build "$bdir" -j"$JOBS" >>"$log" 2>&1; then
        build=FAIL; fail=1
    elif [ -n "${XBR_BUILD_ONLY:-}" ]; then
        pass=-; skip=-; failed=-
    else
        junit=()
        [ -z "$JUNIT_DIR" ] || junit=(--output-junit "$JUNIT_DIR/$name.xml")
        ctest --test-dir "$bdir" --output-on-failure --timeout 300 -j"$JOBS" ${junit[@]+"${junit[@]}"} >>"$log" 2>&1
        # ctest's summary lines: "Test #n: name ...   Passed", "***Skipped", "***Failed", "***Timeout", "***Exception"
        pass=$(grep -cE 'Test +#[0-9]+: .* Passed' "$log")
        skip=$(grep -cE 'Test +#[0-9]+: .*\*\*\*(Skipped|Not Run)' "$log")
        failed=$(grep -cE 'Test +#[0-9]+: .*(\*\*\*Failed|\*\*\*Timeout|\*\*\*Exception|SEGFAULT|Child aborted)' "$log")
        [ "$failed" -eq 0 ] || fail=1
    fi
    echo "| $name | $conf | $build | $pass | $skip | $failed |" >> "$REPORT"
    printf '%-32s configure=%-4s build=%-4s pass=%-3s skip=%-3s fail=%s\n' "$name" "$conf" "$build" "$pass" "$skip" "$failed"
done

echo
echo "Report: $REPORT"
exit $fail
