#!/usr/bin/env bash
# Builds lemmy-64/n64-systemtest (MIT, https://github.com/lemmy-64/n64-systemtest), the N64 hardware test ROM, from
# source at a pinned commit, for the N64 core's tests (consoles/n64/tests: n64_systemtest). XBA-171.
#
#   tools/consoles/build_n64_systemtest.sh [OUTDIR]     (default build/n64-systemtest -> OUTDIR/n64-systemtest.z64)
#
# The project publishes no release ROM, so tools/docker/n64-systemtest.Dockerfile (tag xba-n64-systemtest; Docker's
# layer cache makes a rebuild instant) compiles it (Rust nightly-2022-07-10, nust64 with libdragon's open IPL3) and
# the ROM, its MIT licence and a VERSION line are copied out with `docker cp`. Test code and open-source boot code
# only; the output is never committed (build/ is ignored). The image is about 2.3 GB: remove it with
# `docker rmi xba-n64-systemtest` when done.
#
# Env: JOBS (default 4: the machine is shared), XBA_HOMEBREW_TIMEOUT (seconds for the docker build, default 3600),
# DOCKER_BUILD_ARGS (extra `docker build` flags).
set -euo pipefail

out=${1:-build/n64-systemtest}
jobs=${JOBS:-4}
tmo=${XBA_HOMEBREW_TIMEOUT:-3600}
repo=$(cd "$(dirname "$0")/../.." && pwd)
command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
mkdir -p "$out"
out=$(cd "$out" && pwd)

echo "== image xba-n64-systemtest"
# shellcheck disable=SC2086
timeout "$tmo" docker build ${DOCKER_BUILD_ARGS:-} --build-arg JOBS="$jobs" \
    -f "$repo/tools/docker/n64-systemtest.Dockerfile" -t xba-n64-systemtest "$repo/tools/docker"
cid=$(docker create xba-n64-systemtest)
trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT
docker cp "$cid:/out/." "$out/"
echo "== $out: $(cat "$out/VERSION")"
