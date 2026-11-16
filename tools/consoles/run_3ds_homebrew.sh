#!/usr/bin/env bash
# The 3DS homebrew corpus through the 3DS spike's ctest, in Docker (CI: the homebrew job; XBA-228).
#
#   tools/consoles/run_3ds_homebrew.sh [CORPUS_DIR] [OUT_DIR]      (default: build/homebrew build/3ds-homebrew)
#
# CORPUS_DIR is what tools/consoles/build_homebrew.sh wrote (CORPUS_DIR/3ds/<prog>.3dsx, our own programs). The linux
# job has no corpus, so there the 3DS tests skip (77); this runs them for real, in a throwaway container of the linux
# job's dependency stage (tools/docker/linux.Dockerfile --target deps, tag xba-linux-deps, shared with
# run_gba_packs.sh, run_nds_homebrew.sh and run_n64_homebrew.sh), without network: consoles/3ds/tests built with
# CTR_HOMEBREW_DIR pointing at the corpus, then ctest -R '^ctr_' (ctr_hello: the interpreter; ctr_rc_hello:
# recompiled by the discovery loop; both compare the top screen with consoles/3ds/tests/hello_top.png). A test that skips although the corpus is here
# fails the run. Sources go in and results come out with `docker cp` (no bind mounts: CI drives the host's Docker
# from a container). OUT_DIR gets build.log, ctest.log and the screens. Exits 1 when a test fails or skips.
#
# Env: JOBS (default 4: the machine is shared), XBA_3DS_HB_TIMEOUT (seconds for the image build and for the run,
# default 1800), DOCKER_BUILD_ARGS (extra `docker build` flags).
set -euo pipefail

corpus=${1:-build/homebrew}
out=${2:-build/3ds-homebrew}
jobs=${JOBS:-4}
tmo=${XBA_3DS_HB_TIMEOUT:-1800}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-linux-deps

command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
ls "$corpus"/3ds/*.3dsx >/dev/null 2>&1 || {
    echo "$corpus/3ds has no .3dsx: run tools/consoles/build_homebrew.sh 3ds $corpus first" >&2
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
cmake -S consoles/3ds/tests -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCTR_HOMEBREW_DIR=/usr/src/hb \
    >/out/build.log 2>&1 && cmake --build /tmp/build -j"$JOBS" >>/out/build.log 2>&1 \
  || { tail -n 30 /out/build.log; exit 1; }
rc=0
t0=$(date +%s)
echo "== ctest 3ds (interpreter, discovery-loop recompilation)"
ctest --test-dir /tmp/build -R "^ctr_" -j"$JOBS" --output-on-failure --timeout 900 -V >/out/ctest.log 2>&1 || rc=1
grep -E "^ *[0-9]+/[0-9]+ Test|tests passed|tests failed|: round|: lifted|png_compare" /out/ctest.log
grep -q "Skipped" /out/ctest.log && { echo "a test skipped although the corpus is here"; rc=1; }
[ "$rc" = 0 ] || grep -A30 "The following tests FAILED" /out/ctest.log || true
cp /tmp/build/hello_interp/top.png /out/hello_interp.png 2>/dev/null || true
cp /tmp/build/hello_rc/top.png /out/hello_rc.png 2>/dev/null || true
echo "== 3DS ctest in $(($(date +%s) - t0)) s"
exit $rc'

cid=$(docker create --network none -e JOBS="$jobs" "$image" bash -c "$cmd")
# consoles/nds/tools and consoles/gba/tools: the lifter ctr_tools builds on; png_compare.py: the picture comparison
tar -C "$repo" --exclude=__pycache__ -cf - consoles/3ds consoles/nds/tools consoles/nds/tests/png_compare.py \
    consoles/gba/tools | docker cp - "$cid:/usr/src"
docker cp "$corpus/3ds" "$cid:/usr/src/hb"
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
