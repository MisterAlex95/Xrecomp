#!/usr/bin/env bash
# The PS1 core's external test inputs, cached in the host's Docker (XBA-200; CI: the homebrew job).
#
#   tools/consoles/ps1_test_inputs.sh [OUTDIR]        (default build/ps1-inputs)
#       -> OUTDIR/ps1-bios/openbios.bin    (tools/consoles/build_ps1_bios.sh: OpenBIOS, MIT, from source)
#       -> OUTDIR/ps1-tests/...            (tools/consoles/fetch_ps1_tests.sh: JaCzekanski/ps1-tests release, MIT)
#
# Same scheme as tools/consoles/gba_test_inputs.sh: the scripts pin their upstream sources and sha256, and the result
# is kept as a data-only image xba-ps1-inputs:<key>, the key being the sha256 of the two scripts. Cached, the files
# are copied out of it (no network, about a second); otherwise the scripts run, the image is created from their
# output and older xba-ps1-inputs tags are removed. The BIOS build needs a MIPS toolchain image (xba-ps1-oracle when
# present, else PCSX-Redux's build image, pulled): a toolchain image pulled here is removed again afterwards, so the
# shared host keeps only the small data image. Never pushed, nothing committed or uploaded; `docker cp` only (CI
# drives the host's Docker from a container: no bind mounts).
set -euo pipefail

out=${1:-build/ps1-inputs}
repo=$(cd "$(dirname "$0")/../.." && pwd)
key=$(cat "$repo/tools/consoles/build_ps1_bios.sh" "$repo/tools/consoles/fetch_ps1_tests.sh" | sha256sum | cut -c1-16)
image=xba-ps1-inputs:$key

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
    echo "== $image not cached: building OpenBIOS, fetching ps1-tests"
    base=$(sed -n 's/^base=//p' "$repo/tools/consoles/build_ps1_bios.sh")
    pulled=
    if ! docker image inspect xba-ps1-oracle >/dev/null 2>&1 && ! docker image inspect "$base" >/dev/null 2>&1; then
        pulled=$base
    fi
    rc=0
    bash "$repo/tools/consoles/build_ps1_bios.sh" "$out/ps1-bios" || rc=$?
    [ -z "$pulled" ] || docker rmi "$pulled" >/dev/null 2>&1 || true
    [ "$rc" = 0 ] || exit "$rc"
    bash "$repo/tools/consoles/fetch_ps1_tests.sh" "$out/ps1-tests"
    work=$(mktemp -d)
    printf 'FROM scratch\nCOPY ps1-bios /ps1-bios\nCOPY ps1-tests /ps1-tests\n' >"$work/Dockerfile"
    cp -r "$out/ps1-bios" "$out/ps1-tests" "$work/"
    tar -C "$work" -cf - . | docker build -q -t "$image" - >/dev/null
    docker images --format '{{.Repository}}:{{.Tag}}' xba-ps1-inputs | grep -vxF "$image" |
        xargs -r docker rmi >/dev/null 2>&1 || true
    rm -rf "$out"
    mkdir -p "$out"
fi
cid=$(docker create "$image" /none)
docker cp "$cid:/ps1-bios" "$out/ps1-bios"
docker cp "$cid:/ps1-tests" "$out/ps1-tests"
echo "== $out ($(find "$out/ps1-tests" -name '*.exe' | wc -l) ps1-tests programs)"
cat "$out/ps1-bios/openbios.sha256" "$out/ps1-tests/VERSION"
