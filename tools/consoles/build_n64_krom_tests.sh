#!/usr/bin/env bash
# Builds krom's (PeterLemon/N64) test ROMs from source with an open boot code, for the N64 core's tests
# (consoles/n64/tests/run_krom_tests.sh). XBA-202.
#
#   tools/consoles/build_n64_krom_tests.sh [OUTDIR]   (default build/n64-krom-tests -> OUTDIR/roms/<dir>/<name>.z64 and
#                                                      the reference OUTDIR/roms/<dir>/<name>.png, OUTDIR/list.txt)
#
# PeterLemon/N64 is public domain (Unlicense) and ships, next to each test's source, a reference screenshot taken on
# the console (<name>.png): the tests draw their results on screen. Its prebuilt ROMs and LIB/N64_BOOTCODE.BIN carry
# a rebuild of Nintendo's 6102 IPL3, so we fetch neither: the repository is fetched at KROM_COMMIT as a partial,
# sparse clone that leaves out every *.N64, N64_BOOTCODE.BIN, the BOOTCODE/ directory and the large demos we do not
# test (their blobs are never downloaded). Each test's LIB/N64_BOOTCODE.BIN (the 4032 bytes after the header) is
# instead the boot code of libdragon's open IPL3 in its compat build (boot/bin/ipl3_compat.z64, Unlicense, from the
# libdragon commit tools/docker/n64-homebrew.Dockerfile pins): like the 6102 IPL3 it loads 1 MB from ROM 0x1000 to
# the header's entry point and jumps there, which is what krom's flat binaries expect.
#
# The assembler is ARM9/bass v14 (GPLv3; krom's sources use its syntax, later versions changed it), a build tool
# only: fetched at BASS_COMMIT and built with the tests in a throwaway container
# of the N64 homebrew image (xba-n64-homebrew, tools/docker/n64-homebrew.Dockerfile: built here when missing),
# without network; sources go in and ROMs come out with `docker cp` (no bind mounts). A test is every <name>.asm that
# inserts LIB/N64_BOOTCODE.BIN and has a <name>.png beside it; those that bass refuses are listed in
# OUTDIR/failed.txt. Sources are cached in OUTDIR (kept between runs); nothing here is ever committed (build/ is
# ignored).
#
# Env: JOBS (default 4), XBA_HOMEBREW_TIMEOUT (seconds per docker step, default 1800), DOCKER_BUILD_ARGS.
set -euo pipefail

out=${1:-build/n64-krom-tests}
jobs=${JOBS:-4}
tmo=${XBA_HOMEBREW_TIMEOUT:-1800}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-n64-homebrew
dockerfile=$repo/tools/docker/n64-homebrew.Dockerfile
krom_commit=7085543e4a19d8c539fc9e0a4d2869e788b4ed4b # KROM_COMMIT
bass_commit=78e297331587eff0b2107dabe81ee036d2d01780 # BASS_COMMIT (tag v14)
libdragon_commit=$(sed -n 's/^ARG LIBDRAGON_COMMIT=\([0-9a-f]\{40\}\)$/\1/p' "$dockerfile")
[ -n "$libdragon_commit" ] || {
    echo "no LIBDRAGON_COMMIT in $dockerfile" >&2
    exit 1
}
command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
mkdir -p "$out"
out=$(cd "$out" && pwd)

# fetch URL COMMIT DIR [SPARSE PATTERN...]: DIR holds URL at COMMIT (kept when it already does). With patterns, a
# partial clone (blobs on demand) checked out sparsely (gitignore-style patterns), so excluded files never download.
fetch() {
    local url=$1 commit=$2 dir=$3
    shift 3
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" != "$commit" ]; then
        echo "== fetching $url $commit"
        rm -rf "$dir"
        mkdir -p "$dir"
        git -C "$dir" init -q
        local filter=()
        if [ $# -gt 0 ]; then
            git -C "$dir" config core.sparseCheckout true
            printf '%s\n' "$@" >"$dir/.git/info/sparse-checkout"
            git -C "$dir" remote add origin "$url"
            git -C "$dir" config remote.origin.promisor true
            git -C "$dir" config remote.origin.partialclonefilter blob:none
            filter=(--filter=blob:none)
            url=origin
        fi
        timeout "$tmo" git -C "$dir" fetch -q "${filter[@]}" --depth 1 "$url" "$commit"
        timeout "$tmo" git -C "$dir" checkout -q FETCH_HEAD
        test "$(git -C "$dir" rev-parse HEAD)" = "$commit"
    fi
}
fetch https://github.com/PeterLemon/N64.git "$krom_commit" "$out/krom-src" \
    '/*' '!*.N64' '!*.n64' '!*.z64' '!*.v64' '!N64_BOOTCODE.BIN' '!/BOOTCODE/' '!/Hack/' '!/N64NICCC/' '!/Sound/' \
    '!/EMU/'
fetch https://github.com/ARM9/bass.git "$bass_commit" "$out/bass-src"
fetch https://github.com/DragonMinded/libdragon.git "$libdragon_commit" "$out/libdragon-src" \
    '/LICENSE.md' '/boot/bin/ipl3_compat.z64'
# Belt and braces: no Nintendo boot code in the cache, whatever the sparse checkout did.
find "$out/krom-src" -path "$out/krom-src/.git" -prune -o \( -iname '*.n64' -o -name 'N64_BOOTCODE.BIN' \) -print0 |
    xargs -0r rm -f
rm -rf "$out/krom-src/BOOTCODE"
ipl3=$out/libdragon-src/boot/bin/ipl3_compat.z64
test "$(stat -c %s "$ipl3")" = 4096

echo "== image $image"
if ! docker image inspect "$image" >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    timeout "$tmo" docker build ${DOCKER_BUILD_ARGS:-} --build-arg JOBS="$jobs" -f "$dockerfile" -t "$image" \
        "$repo/tools/docker"
fi

# In the container: build bass, then each test with the open boot code; /work/roms gets <dir>/<name>.z64 + .png.
# nall misses <stdexcept> for GCC 13 (Ubuntu 24.04): added before building.
script='set -u
for h in /work/bass/nall/arithmetic/natural.hpp /work/bass/nall/nall.hpp; do
    [ -f "$h" ] && sed -i "1i #include <stdexcept>" "$h"
done
make -C /work/bass/bass -j"$JOBS" >/tmp/bass.log 2>&1 || { tail -n 30 /tmp/bass.log; exit 1; }
bass=$(find /work/bass/bass -maxdepth 2 -type f -name bass -perm -u+x | head -n 1)
[ -n "$bass" ] || { echo "no bass binary"; tail -n 30 /tmp/bass.log; exit 1; }
cd /work/krom
mkdir -p /work/roms
: >/work/roms/list.txt
: >/work/roms/failed.txt
grep -rl --include="*.asm" "N64_BOOTCODE.BIN" . | sort | while read -r asm; do
    d=${asm%/*}; n=${asm##*/}; n=${n%.asm}
    [ -f "$d/$n.png" ] || continue
    mkdir -p "$d/LIB"
    cp /work/ipl3.bin "$d/LIB/N64_BOOTCODE.BIN"
    o=$(sed -n "s/^ *output \"\\([^\"]*\\)\".*/\\1/p" "$asm" | head -n 1)
    [ -n "$o" ] || { echo "${asm#./} no output" >>/work/roms/failed.txt; continue; }
    rm -f "$d/$o"
    if (cd "$d" && timeout 120 "$bass" "$n.asm" >/tmp/one.log 2>&1) && [ -s "$d/$o" ]; then
        mkdir -p "/work/roms/${d#./}"
        cp "$d/$o" "/work/roms/${d#./}/$n.z64"
        cp "$d/$n.png" "/work/roms/${d#./}/$n.png"
        echo "${d#./}/$n" >>/work/roms/list.txt
    else
        echo "${asm#./} $(head -c 200 /tmp/one.log | tr "\n" " ")" >>/work/roms/failed.txt
    fi
done
echo "built $(wc -l </work/roms/list.txt), failed $(wc -l </work/roms/failed.txt)"'
cid=$(docker create --network none -e JOBS="$jobs" "$image" bash -c "$script")
trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT
tar -C "$out/krom-src" --exclude-vcs --transform 's,^\.,krom,' -cf - . | docker cp - "$cid:/work"
tar -C "$out/bass-src" --exclude-vcs --transform 's,^\.,bass,' -cf - . | docker cp - "$cid:/work"
tmp=$(mktemp -d)
dd if="$ipl3" of="$tmp/ipl3.bin" bs=64 skip=1 status=none # the 4032 bytes after the header
tar -C "$tmp" -cf - ipl3.bin | docker cp - "$cid:/work"
rm -rf "$tmp"
timeout "$tmo" docker start -a "$cid"
rm -rf "$out/roms"
docker cp "$cid:/work/roms" "$out/roms"
mv "$out/roms/list.txt" "$out/list.txt"
mv "$out/roms/failed.txt" "$out/failed.txt"
cp "$out/krom-src/LICENSE" "$out/LICENSE-krom"
printf '%s\n' "PeterLemon/N64 $krom_commit (Unlicense), assembled with ARM9/bass v14 $bass_commit (GPLv3)" \
    "boot code: DragonMinded/libdragon $libdragon_commit boot/bin/ipl3_compat.z64 (Unlicense)" >"$out/VERSION"
echo "== $out: $(wc -l <"$out/list.txt") ROMs, $(wc -l <"$out/failed.txt") failed to assemble"
