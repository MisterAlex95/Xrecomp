#!/usr/bin/env bash
# Builds the mGBA test suite (MIT, https://github.com/mgba-emu/suite) from source: the GBA core's compatibility
# measure (XBA-201, consoles/gba/tests/mgba_suite.py). Its repository publishes only the source, so this assembles it.
#
#   tools/consoles/build_gba_suite.sh [OUTDIR]          (default build/gba-suite -> OUTDIR/suite.gba)
#
# Downloads the source at a pinned commit (checked against its sha256) and runs its Makefile in a throwaway container
# of the xba-gba-homebrew image (tools/docker/gba-homebrew.Dockerfile, built if missing: devkitARM + libgba), without
# network. Sources go in and the ROM comes out with `docker cp` (CI drives the host's Docker from a container). The
# output is never committed (build/ is ignored).
#
# Env: XBA_GBA_SUITE_TIMEOUT (seconds per docker step, default 900), JOBS (default 4).
set -euo pipefail

COMMIT=e6942030d25ffe3ba76c72b73a86da073ec857cc # 2026-07-09, "Fix video test display bug on cleanup"
SHA256=bc053b62519867d003d0cec4afa232a60d6224de80bc1fc3f692ec880605478f
URL=https://codeload.github.com/mgba-emu/suite/tar.gz/$COMMIT

out=${1:-build/gba-suite}
tmo=${XBA_GBA_SUITE_TIMEOUT:-900}
jobs=${JOBS:-4}
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

echo "== mgba-emu/suite $COMMIT"
curl -fsSL --retry 3 -o "$work/src.tgz" "$URL"
echo "$SHA256  $work/src.tgz" | sha256sum -c --quiet -
mkdir "$work/src"
tar xzf "$work/src.tgz" -C "$work/src" --strip-components=1

if ! docker image inspect "$image" >/dev/null 2>&1; then
    timeout "$tmo" docker build -t "$image" - <"$repo/tools/docker/gba-homebrew.Dockerfile"
fi
cid=$(docker create --network none -e JOBS="$jobs" -w /tmp/work "$image" bash -ec \
    'make -j"$JOBS" >/tmp/build.log 2>&1 || { tail -n 40 /tmp/build.log; exit 1; }; test -f suite.gba')
docker cp "$work/src/." "$cid:/tmp/work"
timeout "$tmo" docker start -a "$cid"
docker cp "$cid:/tmp/work/suite.gba" "$out/suite.gba"
cp "$work/src/LICENSE" "$out/suite.LICENSE"
(cd "$out" && sha256sum suite.gba >suite.sha256)
echo "== $out/suite.gba ($(stat -c %s "$out/suite.gba") bytes)"
cat "$out/suite.sha256"
