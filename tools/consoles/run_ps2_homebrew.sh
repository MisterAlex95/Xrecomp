#!/usr/bin/env bash
# The PS2 homebrew corpus through the PS2 spike's ctest, in Docker (CI: the homebrew job; XBA-226).
#
#   tools/consoles/run_ps2_homebrew.sh [CORPUS_DIR] [OUT_DIR]      (default: build/homebrew build/ps2-homebrew)
#
# CORPUS_DIR is what tools/consoles/build_homebrew.sh wrote (CORPUS_DIR/ps2/<prog>.elf, our own programs). The linux
# job has no corpus, so there the PS2 tests skip (77); this runs them for real, in a throwaway container of the linux
# job's dependency stage (tools/docker/linux.Dockerfile --target deps, tag xba-linux-deps, shared with
# run_psp_homebrew.sh and the others), without network: consoles/ps2/tests built with PS2_HOMEBREW_DIR pointing at
# the corpus, then ctest -R '^ps2_' (ps2_hello: the interpreter; ps2_rc_hello: recompiled by the discovery loop; both
# compare what the program wrote to the EE's SIO with consoles/ps2/tests/hello.txt). A test that skips although the
# corpus is here fails the run. Sources go in and results come out with `docker cp` (no bind mounts: CI drives the
# host's Docker from a container). OUT_DIR gets build.log, ctest.log and the SIO outputs. Exits 1 when a test fails or
# skips.
#
# Env: JOBS (default 4: the machine is shared), XBA_PS2_HB_TIMEOUT (seconds for the image build and for the run,
# default 1800), DOCKER_BUILD_ARGS (extra `docker build` flags).
set -euo pipefail

corpus=${1:-build/homebrew}
out=${2:-build/ps2-homebrew}
jobs=${JOBS:-4}
tmo=${XBA_PS2_HB_TIMEOUT:-1800}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-linux-deps

command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
ls "$corpus"/ps2/*.elf >/dev/null 2>&1 || {
    echo "$corpus/ps2 has no .elf: run tools/consoles/build_homebrew.sh ps2 $corpus first" >&2
    exit 1
}

echo "== image $image (linux.Dockerfile, stage deps)"
start=$(date +%s)
# shellcheck disable=SC2086
timeout "$tmo" docker build ${DOCKER_BUILD_ARGS:-} --target deps -t "$image" - <"$repo/tools/docker/linux.Dockerfile"
echo "== image ready in $(($(date +%s) - start)) s"

cid=
cleanup() { [ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true; }
trap cleanup EXIT

# shellcheck disable=SC2016
cmd='mkdir -p /out && cd /usr/src
cmake -S consoles/ps2/tests -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS2_HOMEBREW_DIR=/usr/src/hb \
    >/out/build.log 2>&1 && cmake --build /tmp/build -j"$JOBS" >>/out/build.log 2>&1 \
  || { tail -n 30 /out/build.log; exit 1; }
rc=0
t0=$(date +%s)
echo "== ctest ps2 (interpreter, discovery-loop recompilation)"
ctest --test-dir /tmp/build -R "^ps2_" -j"$JOBS" --output-on-failure --timeout 900 -V >/out/ctest.log 2>&1 || rc=1
grep -E "^ *[0-9]+/[0-9]+ Test|tests passed|tests failed|: round|: lifted|: MMI|SIO output" /out/ctest.log
grep -q "Skipped" /out/ctest.log && { echo "a test skipped although the corpus is here"; rc=1; }
[ "$rc" = 0 ] || grep -A30 "The following tests FAILED" /out/ctest.log || true
cp /tmp/build/hello_interp/sio.txt /out/hello_interp.txt 2>/dev/null || true
cp /tmp/build/hello_rc/sio.txt /out/hello_rc.txt 2>/dev/null || true
echo "== PS2 ctest in $(($(date +%s) - t0)) s"
exit $rc'

cid=$(docker create --network none -e JOBS="$jobs" "$image" bash -c "$cmd")
tar -C "$repo" --exclude=__pycache__ -cf - consoles/ps2 consoles/ps1/tools | docker cp - "$cid:/usr/src"
docker cp "$corpus/ps2" "$cid:/usr/src/hb"
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
