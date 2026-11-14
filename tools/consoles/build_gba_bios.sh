#!/usr/bin/env bash
# Builds the open-source Cult-of-GBA replacement BIOS (MIT, https://github.com/Cult-of-GBA/BIOS) from source, for
# the GBA core's LLE mode (gba_run --bios, gba_create with a BIOS image). XBA-153. Never a Nintendo BIOS dump.
#
#   tools/consoles/build_gba_bios.sh [OUTDIR]          (default build/gba-bios -> OUTDIR/cult-of-gba-bios.bin)
#
# Downloads the source at a pinned commit (checked against its sha256), deletes the prebuilt bios.bin the
# repository ships, and assembles entrypoint.s in a throwaway container of the xba-gba-homebrew image
# (tools/docker/gba-homebrew.Dockerfile, built if missing; devkitARM), without network. The output is never
# committed (build/ is ignored).
#
# Env: XBA_GBA_BIOS_TIMEOUT (seconds per docker step, default 600).
set -euo pipefail

COMMIT=a30e9a96df083628b650724b7d4d7112b4070b98 # 2025-04-06, "Fix BG2/BG3 scaling parameters after RegisterRamReset"
SHA256=e1112540e94f517d11456c4f4a28ffb5fd2dc9d3f0d40f29f9f9cbcbed2fa338
URL=https://codeload.github.com/Cult-of-GBA/BIOS/tar.gz/$COMMIT

out=${1:-build/gba-bios}
tmo=${XBA_GBA_BIOS_TIMEOUT:-600}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-gba-homebrew

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

echo "== Cult-of-GBA BIOS $COMMIT"
curl -fsSL --retry 3 -o "$work/src.tgz" "$URL"
echo "$SHA256  $work/src.tgz" | sha256sum -c --quiet -
mkdir "$work/src"
tar xzf "$work/src.tgz" -C "$work/src" --strip-components=1
rm -f "$work/src/bios.bin" # built here, not taken from the repository

if ! docker image inspect "$image" >/dev/null 2>&1; then
    timeout "$tmo" docker build -t "$image" - <"$repo/tools/docker/gba-homebrew.Dockerfile"
fi
cid=$(docker create --network none -w /tmp/work "$image" bash -ec \
    '"$DEVKITARM"/bin/arm-none-eabi-as entrypoint.s -mcpu=arm7tdmi -o /tmp/entrypoint.o &&
     "$DEVKITARM"/bin/arm-none-eabi-objcopy /tmp/entrypoint.o -O binary /tmp/bios.bin')
docker cp "$work/src/." "$cid:/tmp/work"
timeout "$tmo" docker start -a "$cid"
docker cp "$cid:/tmp/bios.bin" "$out/cult-of-gba-bios.bin"
size=$(stat -c %s "$out/cult-of-gba-bios.bin")
if [ "$size" -gt 16384 ]; then
    echo "unexpected BIOS size $size" >&2
    exit 1
fi
cp "$work/src/LICENSE" "$out/cult-of-gba-bios.LICENSE"
(cd "$out" && sha256sum cult-of-gba-bios.bin >cult-of-gba-bios.sha256)
echo "== $out/cult-of-gba-bios.bin ($size bytes)"
cat "$out/cult-of-gba-bios.sha256"
