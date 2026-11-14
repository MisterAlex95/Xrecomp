#!/usr/bin/env bash
# Builds OpenBIOS (MIT, the open-source PlayStation BIOS of PCSX-Redux's "nugget" tree, src/mips/openbios) from
# source, for the PS1 core's LLE boot (ps1_run --bios, ps1_create). XBA-163. Never a Sony BIOS dump.
#
#   tools/consoles/build_ps1_bios.sh [OUTDIR]          (default build/ps1-bios -> OUTDIR/openbios.bin)
#
# The nugget commit is the one PCSX-Redux pins as its src/mips submodule at the commit of
# tools/docker/ps1-oracle.Dockerfile (b3eb3253), so our BIOS is the oracle's. The source tarball is checked against
# its sha256 and built with `make -C openbios` in a throwaway container, without network, by the toolchain the oracle
# uses: the xba-ps1-oracle image when present (tools/docker/ps1-oracle.Dockerfile), else that image's base, PCSX-Redux's
# CI build image pinned by digest (mipsel-none-elf GCC 16 with libstdc++ headers, which OpenBIOS's psyqo shell needs;
# PSn00bSDK's GCC has none).
# The output is never committed (build/ is ignored). Same inputs, same bytes: the sha256 is written next to it.
#
# Env: XBA_PS1_BIOS_TIMEOUT (seconds per docker step, default 900), JOBS (make -j, default 4).
set -euo pipefail

# pcsx-redux b3eb32535cac5c8dab7c966d2ae87f7abad59d8c -> src/mips = pcsx-redux/nugget at:
COMMIT=f044c6b863116cf851acb360ba09adaa54381bd7
SHA256=43ffd423caefe3f07ea9713df453ee96c06cc467795e942ab0a89ef736249c0c
URL=https://codeload.github.com/pcsx-redux/nugget/tar.gz/$COMMIT

out=${1:-build/ps1-bios}
tmo=${XBA_PS1_BIOS_TIMEOUT:-900}
jobs=${JOBS:-4}
base=ghcr.io/pcsx-redux/pcsx-redux-build:noble@sha256:20fece3d1e3b3c3e0a42bce9b986fb659b12bd441352df55abd862c381fb48f2
image=xba-ps1-oracle

command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
cid=
cleanup() {
    [ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT

echo "== OpenBIOS (pcsx-redux/nugget $COMMIT)"
curl -fsSL --retry 3 -o "$work/src.tgz" "$URL"
echo "$SHA256  $work/src.tgz" | sha256sum -c --quiet -
mkdir "$work/src"
tar xzf "$work/src.tgz" -C "$work/src" --strip-components=1

if ! docker image inspect "$image" >/dev/null 2>&1; then
    image=$base
    docker image inspect "$image" >/dev/null 2>&1 || timeout "$tmo" docker pull "$image"
fi
# SOURCE_DATE_EPOCH and a fixed path keep the image byte-for-byte reproducible.
cid=$(docker create --network none -e SOURCE_DATE_EPOCH=0 -w /tmp/nugget "$image" bash -ec \
    "make -C openbios -j$jobs all >/tmp/build.log 2>&1 || { tail -n 40 /tmp/build.log; exit 1; }")
docker cp "$work/src/." "$cid:/tmp/nugget"
timeout "$tmo" docker start -a "$cid"
docker cp "$cid:/tmp/nugget/openbios/openbios.bin" "$out/openbios.bin"
size=$(stat -c %s "$out/openbios.bin")
if [ "$size" -gt 524288 ] || [ "$size" -lt 4096 ]; then
    echo "unexpected BIOS size $size" >&2
    exit 1
fi
cp "$work/src/LICENSE" "$out/openbios.LICENSE"
echo "pcsx-redux/nugget $COMMIT ($(docker run --rm --network none "$image" mipsel-none-elf-gcc --version | head -n 1))" >"$out/openbios.VERSION"
(cd "$out" && sha256sum openbios.bin >openbios.sha256)
echo "== $out/openbios.bin ($size bytes)"
cat "$out/openbios.sha256"
