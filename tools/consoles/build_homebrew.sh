#!/usr/bin/env bash
# Builds the homebrew test corpus of the other consoles (XBA-149): our own test programs, never a
# commercial ROM, disc or BIOS.
#
#   tools/consoles/build_homebrew.sh [gba|ps1|n64|all] [OUTDIR]      (default: all build/homebrew)
#
# For each console: builds the toolchain image tools/docker/<console>-homebrew.Dockerfile (tag
# xba-<console>-homebrew; Docker's layer cache makes a rebuild instant), then builds
# consoles/<console>/tests/homebrew/ in a throwaway container of it, without network, and writes
# into OUTDIR/<console>/:
#   gba: <prog>.gba + <prog>.elf                           (hello cpu timer_irq gfx iwram_copy pad)
#   ps1: <prog>.exe + <prog>.elf + <prog>.bin/.cue disc    (hello cpu timer_irq gfx overlay)
#   n64: <prog>.z64 + <prog>.elf                           (hello cpu timer_irq gfx)
# plus SHA256SUMS. Outputs are never committed (build/ is ignored).
#
# Sources go in and outputs come out with `docker cp`, not bind mounts, so the script also works
# from a CI job that talks to the host's Docker daemon from inside a container.
#
# Env: JOBS (default 4: the machine is shared), XBA_HOMEBREW_TIMEOUT (seconds per docker step,
# default 1800), DOCKER_BUILD_ARGS (extra `docker build` flags, e.g. --cache-from).
set -euo pipefail

what=${1:-all}
out=${2:-build/homebrew}
jobs=${JOBS:-4}
tmo=${XBA_HOMEBREW_TIMEOUT:-1800}

repo=$(cd "$(dirname "$0")/../.." && pwd)
case "$what" in
gba | ps1 | n64) consoles=("$what") ;;
all) consoles=(gba ps1 n64) ;;
*)
    echo "usage: $0 [gba|ps1|n64|all] [OUTDIR]" >&2
    exit 2
    ;;
esac
command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
mkdir -p "$out"
out=$(cd "$out" && pwd)

# Build commands, run inside the image in /tmp/work (a copy of consoles/<c>/tests/homebrew); they
# leave their outputs in /out.
build_cmd_gba='make -j"$JOBS" OUT=/tmp/out BUILD=/tmp/build && cp /tmp/out/*.gba /tmp/out/*.elf /out/'
build_cmd_ps1='cmake -S /tmp/work -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$PSN00BSDK_LIBS/cmake/sdk.cmake" \
  && cmake --build /tmp/build -j "$JOBS" \
  && cd /tmp/build && cp *.exe *.elf *.bin *.cue /out/'
build_cmd_n64='make -j"$JOBS" BUILD_DIR=/tmp/build && cp *.z64 /out/ && cp /tmp/build/*.elf /out/'

cid=
cleanup() { [ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true; }
trap cleanup EXIT

for c in "${consoles[@]}"; do
    image="xba-$c-homebrew"
    dest="$out/$c"
    echo "== $c: image $image"
    start=$(date +%s)
    # No build context: the Dockerfiles download what they need, pinned by digest / sha256.
    # shellcheck disable=SC2086
    timeout "$tmo" docker build ${DOCKER_BUILD_ARGS:-} -t "$image" - <"$repo/tools/docker/$c-homebrew.Dockerfile"
    echo "== $c: image ready in $(($(date +%s) - start)) s"

    start=$(date +%s)
    cmd_var="build_cmd_$c"
    cid=$(docker create --network none -e JOBS="$jobs" -w /tmp/work "$image" \
        bash -ec "mkdir -p /out && ${!cmd_var}")
    docker cp "$repo/consoles/$c/tests/homebrew/." "$cid:/tmp/work"
    timeout "$tmo" docker start -a "$cid"
    rm -rf "$dest"
    mkdir -p "$dest"
    docker cp "$cid:/out/." "$dest"
    docker rm "$cid" >/dev/null
    cid=
    (cd "$dest" && sha256sum -- * >SHA256SUMS)
    echo "== $c: programs built in $(($(date +%s) - start)) s -> $dest"
    ls -l "$dest"
done
