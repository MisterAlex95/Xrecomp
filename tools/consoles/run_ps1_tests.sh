#!/usr/bin/env bash
# The PS1 core's tests with all their inputs, in Docker (XBA-200; CI: the homebrew job).
#
#   tools/consoles/run_ps1_tests.sh [CORPUS_DIR] [OUT_DIR] [INPUTS_DIR]
#                                   (default: build/homebrew build/ps1-tests-run build/ps1-inputs)
#
# CORPUS_DIR is what tools/consoles/build_homebrew.sh wrote (CORPUS_DIR/ps1/<prog>.exe / .cue, our own programs);
# INPUTS_DIR what tools/consoles/ps1_test_inputs.sh wrote (OpenBIOS built from source, the JaCzekanski/ps1-tests
# release; both MIT). The linux job has none of them, so there the EXE / BIOS tests skip (77); this runs them for real.
# In a throwaway container of the linux job's dependency stage (tools/docker/linux.Dockerfile --target deps, tag
# xba-linux-deps, shared with run_gba_packs.sh / run_n64_homebrew.sh), without network: consoles/ps1/tests built with
# PS1_HOMEBREW_DIR / PS1_BIOS / PS1_TESTS_DIR pointing at them, then the whole PS1 ctest but the tests in
# PS1_CI_EXCLUDE: the homebrew under OpenBIOS and the HLE BIOS (ps1_hb_* / ps1_hle_*, discs, mcard), recompiled in
# lockstep (ps1_rc_* / ps1_hle_rc_*), ps1-tests (ps1_jcz_* / ps1_hle_jcz_*, GPU VRAM dumps compared), save states,
# rewind, cheats, ps1_lockref_*, plus the unit tests. A test that skips although its inputs are here fails the run.
# Sources go in and results come out with `docker cp` (no bind mounts: CI drives the host's Docker from a container).
# OUT_DIR gets build.log and ctest.log. Exits 1 when a test fails or skips.
#
# Env: JOBS (default 4: the machine is shared), XBA_PS1_TESTS_TIMEOUT (seconds for the image build and for the run,
# default 1800), DOCKER_BUILD_ARGS (extra `docker build` flags), PS1_CI_EXCLUDE (ctest -E regex of tests left to
# local runs, default below: the slowest lockstep-reference variants).
set -euo pipefail

corpus=${1:-build/homebrew}
out=${2:-build/ps1-tests-run}
inputs=${3:-build/ps1-inputs}
jobs=${JOBS:-4}
tmo=${XBA_PS1_TESTS_TIMEOUT:-1800}
exclude=${PS1_CI_EXCLUDE-}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-linux-deps

command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
ls "$corpus"/ps1/*.exe >/dev/null 2>&1 || {
    echo "$corpus/ps1 has no .exe: run tools/consoles/build_homebrew.sh ps1 $corpus first" >&2
    exit 1
}
[ -f "$inputs/ps1-bios/openbios.bin" ] && [ -d "$inputs/ps1-tests" ] || {
    echo "$inputs/ps1-bios/openbios.bin or $inputs/ps1-tests missing: run tools/consoles/ps1_test_inputs.sh $inputs" >&2
    exit 1
}

echo "== image $image (linux.Dockerfile, stage deps)"
start=$(date +%s)
# No build context: the deps stage copies nothing.
# shellcheck disable=SC2086
timeout "$tmo" docker build ${DOCKER_BUILD_ARGS:-} --target deps -t "$image" - <"$repo/tools/docker/linux.Dockerfile"
echo "== image ready in $(($(date +%s) - start)) s"

cid=
cleanup() { [ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true; }
trap cleanup EXIT

# shellcheck disable=SC2016
cmd='mkdir -p /out && cd /usr/src
export SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy
t0=$(date +%s)
cmake -S consoles/ps1/tests -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS1_HOMEBREW_DIR=/usr/src/hb \
    -DPS1_BIOS=/usr/src/inputs/ps1-bios/openbios.bin -DPS1_TESTS_DIR=/usr/src/inputs/ps1-tests \
    >/out/build.log 2>&1 && cmake --build /tmp/build -j"$JOBS" >>/out/build.log 2>&1 \
  || { tail -n 30 /out/build.log; exit 1; }
echo "== built in $(($(date +%s) - t0)) s"
rc=0
t0=$(date +%s)
echo "== ctest PS1 (homebrew, ps1-tests, OpenBIOS + HLE, recompiled in lockstep, states, rewind, cheats, lockref)"
[ -z "$EXCLUDE" ] || echo "   left to local runs: $EXCLUDE"
ctest --test-dir /tmp/build ${EXCLUDE:+-E "$EXCLUDE"} -j"$JOBS" --output-on-failure --timeout 900 \
    >/out/ctest.log 2>&1 || rc=1
grep -E "^ *[0-9]+/[0-9]+ Test|tests passed|tests failed" /out/ctest.log
grep -q "Skipped" /out/ctest.log && { echo "a test skipped although its inputs are here"; rc=1; }
[ "$rc" = 0 ] || grep -A40 "The following tests FAILED" /out/ctest.log || true
echo "== PS1 ctest in $(($(date +%s) - t0)) s"
exit $rc'

cid=$(docker create --network none -e JOBS="$jobs" -e EXCLUDE="$exclude" "$image" bash -c "$cmd")
tar -C "$repo" --exclude=__pycache__ -cf - consoles tools src third_party/libchdr | docker cp - "$cid:/usr/src"
docker cp "$corpus/ps1" "$cid:/usr/src/hb"
docker cp "$inputs/." "$cid:/usr/src/inputs"
rc=0
timeout "$tmo" docker start -a "$cid" || rc=$?
rm -rf "$out"
mkdir -p "$out"
docker cp "$cid:/out/." "$out" || true
docker rm "$cid" >/dev/null
cid=
echo "== results in $out"
ls "$out"
exit "$rc"
