#!/usr/bin/env bash
# The Xbox 360 test titles through the Xbox 360 runtime's ctest, in Docker (CI: the homebrew job; XBA-270).
#
#   tools/consoles/run_x360_homebrew.sh [CORPUS_DIR] [OUT_DIR]       (default: build/homebrew build/x360-homebrew)
#
# CORPUS_DIR is what tools/consoles/build_homebrew.sh wrote (CORPUS_DIR/x360/<prog>.elf, our own programs). The linux
# job has no corpus, so there x360_hello_c skips (77); this runs consoles/x360/tests for real in a throwaway container
# of the linux job's dependency stage (tools/docker/linux.Dockerfile --target deps, tag xba-linux-deps), without
# network: x360_hello / x360_hello_lzx (the assembly title) and x360_hello_c (the C title packed by elf2xex.py,
# recompiled, run under x360_run, its DbgPrint text against consoles/x360/tests/hello_c.txt). A test that skips
# although the corpus is here fails the run. Sources go in and results come out with `docker cp`. OUT_DIR gets
# build.log and ctest.log. Exits 1 when a test fails or skips.
#
# Env: JOBS (default 4), XBA_X360_HB_TIMEOUT (seconds for the image build and for the run, default 1800),
# DOCKER_BUILD_ARGS (extra `docker build` flags).
set -euo pipefail

corpus=${1:-build/homebrew}
out=${2:-build/x360-homebrew}
jobs=${JOBS:-4}
tmo=${XBA_X360_HB_TIMEOUT:-1800}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-linux-deps

command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
ls "$corpus"/x360/*.elf >/dev/null 2>&1 || {
    echo "$corpus/x360 has no .elf: run tools/consoles/build_homebrew.sh x360 $corpus first" >&2
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
cmake -S consoles/x360/tests -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DX360_HOMEBREW_DIR=/usr/src/hb \
    >/out/build.log 2>&1 && cmake --build /tmp/build -j"$JOBS" >>/out/build.log 2>&1 \
  || { tail -n 30 /out/build.log; exit 1; }
rc=0
t0=$(date +%s)
echo "== ctest x360"
ctest --test-dir /tmp/build -R "^(x360_|xenos_)" -j"$JOBS" --output-on-failure --timeout 900 -V >/out/ctest.log 2>&1 || rc=1
grep -E "^ *[0-9]+/[0-9]+ Test|tests passed|tests failed|recomp:|stats:|DbgPrint output" /out/ctest.log
grep -q "Skipped" /out/ctest.log && { echo "a test skipped although the corpus is here"; rc=1; }
[ "$rc" = 0 ] || grep -A30 "The following tests FAILED" /out/ctest.log || true
echo "== Xbox 360 ctest in $(($(date +%s) - t0)) s"
exit $rc'

cid=$(docker create --network none -e JOBS="$jobs" "$image" bash -c "$cmd")
# tools/ppc: the shared PowerPC frontend; tools/xex: the XEX2 side; tools/xiso: mkxiso.py for the disc image;
# consoles/gc/runtime: ppc_ops.h; src/xiso: the disc image reader the runtime links; src/input: console_pad, which
# x360_pad_map (x360_run's title defaults, WP-25) is built on
tar -C "$repo" --exclude=__pycache__ -cf - consoles/x360 consoles/gc/runtime tools/__init__.py tools/ppc tools/xex \
    tools/xiso src/xiso src/input \
    | docker cp - "$cid:/usr/src"
docker cp "$corpus/x360" "$cid:/usr/src/hb"
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
