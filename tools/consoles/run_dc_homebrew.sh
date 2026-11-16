#!/usr/bin/env bash
# The Dreamcast homebrew corpus through the DC spike's ctest, in Docker (CI: the homebrew job; XBA-227).
#
#   tools/consoles/run_dc_homebrew.sh [CORPUS_DIR] [OUT_DIR]      (default: build/homebrew build/dc-homebrew)
#
# CORPUS_DIR is what tools/consoles/build_homebrew.sh wrote (CORPUS_DIR/dc/<prog>.elf, our own programs). The linux
# job has no corpus, so there the DC hello tests skip (77); this runs them for real, in a throwaway container of the
# linux job's dependency stage (tools/docker/linux.Dockerfile --target deps, tag xba-linux-deps, shared with
# run_gba_packs.sh, run_n64_homebrew.sh and run_nds_homebrew.sh), without network: consoles/dc/tests built with
# DC_HOMEBREW_DIR pointing at the corpus, then ctest -R '^dc_' (dc_sh4_ops / dc_rc_sh4_ops: synthetic SH-4 programs,
# interpreted and lifted; dc_hello: the interpreter; dc_rc_hello: recompiled by the discovery loop; both compare the
# serial lines and the framebuffer with consoles/dc/tests). A test that skips although the corpus is here fails the
# run. Sources go in and results come out with `docker cp` (no bind mounts: CI drives the host's Docker from a
# container). OUT_DIR gets build.log, ctest.log, the framebuffers and the serial output. Exits 1 when a test fails
# or skips.
#
# Env: JOBS (default 4: the machine is shared), XBA_DC_HB_TIMEOUT (seconds for the image build and for the run,
# default 1800), DOCKER_BUILD_ARGS (extra `docker build` flags).
set -euo pipefail

corpus=${1:-build/homebrew}
out=${2:-build/dc-homebrew}
jobs=${JOBS:-4}
tmo=${XBA_DC_HB_TIMEOUT:-1800}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-linux-deps

command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
ls "$corpus"/dc/*.elf >/dev/null 2>&1 || {
    echo "$corpus/dc has no .elf: run tools/consoles/build_homebrew.sh dc $corpus first" >&2
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
cmake -S consoles/dc/tests -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDC_HOMEBREW_DIR=/usr/src/hb \
    >/out/build.log 2>&1 && cmake --build /tmp/build -j"$JOBS" >>/out/build.log 2>&1 \
  || { tail -n 30 /out/build.log; exit 1; }
rc=0
t0=$(date +%s)
echo "== ctest dc (synthetic SH-4 programs, interpreter, discovery-loop recompilation)"
ctest --test-dir /tmp/build -R "^dc_" -j"$JOBS" --output-on-failure --timeout 900 -V >/out/ctest.log 2>&1 || rc=1
grep -E "^ *[0-9]+/[0-9]+ Test|tests passed|tests failed|: round|: lifted|: serial|: decoder|: [a-z]+: ok|png_compare" \
    /out/ctest.log || true
grep -q "Skipped" /out/ctest.log && { echo "a test skipped although the corpus is here"; rc=1; }
[ "$rc" = 0 ] || grep -A30 "The following tests FAILED" /out/ctest.log || true
for m in interp rc; do
    cp /tmp/build/hello_$m/fb.png /out/hello_$m.png 2>/dev/null || true
    cp /tmp/build/hello_$m/serial.txt /out/hello_$m.serial.txt 2>/dev/null || true
done
echo "== DC ctest in $(($(date +%s) - t0)) s"
exit $rc'

cid=$(docker create --network none -e JOBS="$jobs" "$image" bash -c "$cmd")
tar -C "$repo" --exclude=__pycache__ -cf - consoles/dc consoles/nds/tests/png_compare.py | docker cp - "$cid:/usr/src"
docker cp "$corpus/dc" "$cid:/usr/src/hb"
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
