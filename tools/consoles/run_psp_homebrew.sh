#!/usr/bin/env bash
# The PSP homebrew corpus through the PSP spike's ctest, in Docker (CI: the homebrew job; XBA-224).
#
#   tools/consoles/run_psp_homebrew.sh [CORPUS_DIR] [OUT_DIR]      (default: build/homebrew build/psp-homebrew)
#
# CORPUS_DIR is what tools/consoles/build_homebrew.sh wrote (CORPUS_DIR/psp/<prog>.prx and .pbp, our own programs).
# The linux job has no corpus, so there the PSP tests skip (77); this runs them for real, in a throwaway container of
# the linux job's dependency stage (tools/docker/linux.Dockerfile --target deps, tag xba-linux-deps, shared with
# run_gba_packs.sh, run_n64_homebrew.sh and run_nds_homebrew.sh), without network: consoles/psp/tests built with
# PSP_HOMEBREW_DIR pointing at the corpus, then ctest -R '^psp_' (psp_hello / psp_pbp_hello: the interpreter on the
# PRX / the EBOOT.PBP; psp_rc_hello: recompiled by the discovery loop; all compare the framebuffer with
# consoles/psp/tests/hello.png). A test that skips although the corpus is here fails the run. Sources go in and
# results come out with `docker cp` (no bind mounts: CI drives the host's Docker from a container). OUT_DIR gets
# build.log, ctest.log and the screens. Exits 1 when a test fails or skips.
#
# Env: JOBS (default 4: the machine is shared), XBA_PSP_HB_TIMEOUT (seconds for the image build and for the run,
# default 1800), DOCKER_BUILD_ARGS (extra `docker build` flags).
set -euo pipefail

corpus=${1:-build/homebrew}
out=${2:-build/psp-homebrew}
jobs=${JOBS:-4}
tmo=${XBA_PSP_HB_TIMEOUT:-1800}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-linux-deps

command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
ls "$corpus"/psp/*.prx >/dev/null 2>&1 || {
    echo "$corpus/psp has no .prx: run tools/consoles/build_homebrew.sh psp $corpus first" >&2
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
cmake -S consoles/psp/tests -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPSP_HOMEBREW_DIR=/usr/src/hb \
    >/out/build.log 2>&1 && cmake --build /tmp/build -j"$JOBS" >>/out/build.log 2>&1 \
  || { tail -n 30 /out/build.log; exit 1; }
rc=0
t0=$(date +%s)
echo "== ctest psp (interpreter on the PRX and the EBOOT, discovery-loop recompilation)"
ctest --test-dir /tmp/build -R "^psp_" -j"$JOBS" --output-on-failure --timeout 900 -V >/out/ctest.log 2>&1 || rc=1
grep -E "^ *[0-9]+/[0-9]+ Test|tests passed|tests failed|: round|: lifted|png_compare" /out/ctest.log
grep -q "Skipped" /out/ctest.log && { echo "a test skipped although the corpus is here"; rc=1; }
[ "$rc" = 0 ] || grep -A30 "The following tests FAILED" /out/ctest.log || true
cp /tmp/build/hello_interp/screen.png /out/hello_interp.png 2>/dev/null || true
cp /tmp/build/hello_rc/screen.png /out/hello_rc.png 2>/dev/null || true
echo "== PSP ctest in $(($(date +%s) - t0)) s"
exit $rc'

cid=$(docker create --network none -e JOBS="$jobs" "$image" bash -c "$cmd")
tar -C "$repo" --exclude=__pycache__ -cf - consoles/psp consoles/ps1/tools | docker cp - "$cid:/usr/src"
docker cp "$corpus/psp" "$cid:/usr/src/hb"
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
