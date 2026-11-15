#!/usr/bin/env bash
# The GBA core's external test inputs, cached in the host's Docker (XBA-193; CI: the homebrew job).
#
#   tools/consoles/gba_test_inputs.sh [OUTDIR]        (default build/gba-inputs)
#       -> OUTDIR/gba-bios/cult-of-gba-bios.bin    (tools/consoles/build_gba_bios.sh: Cult-of-GBA BIOS, MIT, from source)
#       -> OUTDIR/gba-tests/*.gba                  (tools/consoles/fetch_gba_tests.sh: jsmolka/gba-tests, MIT)
#       -> OUTDIR/gba-suite/suite.gba              (tools/consoles/build_gba_suite.sh: the mGBA test suite, MIT, XBA-201)
#
# The scripts pin their upstream commit and its sha256. The result is kept as a data-only image
# xba-gba-inputs:<key>, the key being the sha256 of the scripts (so of the pins): when it exists the files are
# copied out of it (no network, about a second), otherwise the scripts run and the image is created from their
# output, and older xba-gba-inputs tags are removed. The image is never pushed and nothing here is committed or
# uploaded. Like the other scripts, `docker cp` only (CI drives the host's Docker from a container: no bind mounts).
set -euo pipefail

out=${1:-build/gba-inputs}
repo=$(cd "$(dirname "$0")/../.." && pwd)
key=$(cat "$repo/tools/consoles/build_gba_bios.sh" "$repo/tools/consoles/fetch_gba_tests.sh" \
    "$repo/tools/consoles/build_gba_suite.sh" | sha256sum | cut -c1-16)
image=xba-gba-inputs:$key

command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
rm -rf "$out"
mkdir -p "$out"

cid=
work=
cleanup() {
    [ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true
    [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT

if docker image inspect "$image" >/dev/null 2>&1; then
    echo "== $image cached"
else
    echo "== $image not cached: building the BIOS and the mGBA suite, fetching the ROMs"
    bash "$repo/tools/consoles/build_gba_bios.sh" "$out/gba-bios"
    bash "$repo/tools/consoles/fetch_gba_tests.sh" "$out/gba-tests"
    bash "$repo/tools/consoles/build_gba_suite.sh" "$out/gba-suite"
    work=$(mktemp -d)
    printf 'FROM scratch\nCOPY gba-bios /gba-bios\nCOPY gba-tests /gba-tests\nCOPY gba-suite /gba-suite\n' >"$work/Dockerfile"
    cp -r "$out/gba-bios" "$out/gba-tests" "$out/gba-suite" "$work/"
    tar -C "$work" -cf - . | docker build -q -t "$image" - >/dev/null
    docker images --format '{{.Repository}}:{{.Tag}}' xba-gba-inputs | grep -vxF "$image" |
        xargs -r docker rmi >/dev/null 2>&1 || true
    rm -rf "$out"
    mkdir -p "$out"
fi
cid=$(docker create "$image" /none)
docker cp "$cid:/gba-bios" "$out/gba-bios"
docker cp "$cid:/gba-tests" "$out/gba-tests"
docker cp "$cid:/gba-suite" "$out/gba-suite"
echo "== $out"
(cd "$out" && sha256sum gba-bios/cult-of-gba-bios.bin gba-tests/*.gba gba-suite/suite.gba)
