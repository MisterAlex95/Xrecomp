#!/usr/bin/env bash
# The GBA homebrew corpus through the recompilation chain, in Docker (XBA-180; CI: the homebrew job).
#
#   tools/consoles/run_gba_packs.sh [CORPUS_DIR] [OUT_DIR]        (default: build/homebrew build/gba-packs)
#
# CORPUS_DIR is what tools/consoles/build_homebrew.sh wrote (CORPUS_DIR/gba/<prog>.gba, our own programs). In a
# throwaway container of the linux job's dependency stage (tools/docker/linux.Dockerfile --target deps, tag
# xba-linux-deps; the host's layer cache shares it with the linux job), without network:
#   1. consoles/gba/tests built with GBA_CORE_ONLY=ON and GBA_HOMEBREW_DIR pointing at the corpus, then
#      ctest -R '^gba_(rc_)?hb_' -E _lle_: each program under gba_run --result (interpreter), and recompiled by the
#      discovery loop in lockstep with the interpreter (the LLE variants need the Cult-of-GBA BIOS: not built here)
#   2. tools/consoles/gba_corpus_packs.py: each program prepared into a desktop pack (python3 -m tools.prepare),
#      run with the pack in lockstep until its result block's verdict, a screenshot 30 frames later, its sha256
#      checked against consoles/gba/tests/corpus_screens.sha256
# Sources go in and results come out with `docker cp` (no bind mounts: CI drives the host's Docker from a container).
# OUT_DIR gets the screenshots (<prog>.png / .ppm), report.md, SHA256SUMS, the per-program logs and ctest.log.
# Exits 1 when a test, a verdict, a lockstep or a screenshot hash fails.
#
# Env: JOBS (default 4: the machine is shared), XBA_GBA_PACKS_TIMEOUT (seconds for the image build and for the run,
# default 1800), DOCKER_BUILD_ARGS (extra `docker build` flags).
set -euo pipefail

corpus=${1:-build/homebrew}
out=${2:-build/gba-packs}
jobs=${JOBS:-4}
tmo=${XBA_GBA_PACKS_TIMEOUT:-1800}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-linux-deps

command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
ls "$corpus"/gba/*.gba >/dev/null 2>&1 || {
    echo "$corpus/gba has no .gba: run tools/consoles/build_homebrew.sh gba $corpus first" >&2
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
cmake -S consoles/gba/tests -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGBA_CORE_ONLY=ON \
    -DGBA_HOMEBREW_DIR=/usr/src/hb >/out/build.log 2>&1 && cmake --build /tmp/build -j"$JOBS" >>/out/build.log 2>&1 \
  || { tail -n 30 /out/build.log; exit 1; }
rc=0
echo "== ctest gba_hb_* / gba_rc_hb_* (interpreter, discovery-loop recompilation in lockstep)"
ctest --test-dir /tmp/build -R "^gba_(rc_)?hb_" -E _lle_ -j"$JOBS" --output-on-failure --timeout 600 >/out/ctest.log 2>&1 \
  || rc=1
grep -E "^ *[0-9]+/[0-9]+ Test|tests passed|tests failed" /out/ctest.log
[ "$rc" = 0 ] || grep -A30 "The following tests FAILED" /out/ctest.log || true
echo "== Prepare packs (python3 -m tools.prepare), run headless, screenshots"
python3 tools/consoles/gba_corpus_packs.py /usr/src/hb /out --gba-run /tmp/build/gba_runtime/gba_run || rc=1
rm -rf /out/work /out/packs
exit $rc'

cid=$(docker create --network none -e JOBS="$jobs" "$image" bash -c "$cmd")
tar -C "$repo" --exclude=__pycache__ -cf - consoles tools | docker cp - "$cid:/usr/src"
docker cp "$corpus/gba" "$cid:/usr/src/hb"
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
