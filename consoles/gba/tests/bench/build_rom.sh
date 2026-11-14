#!/usr/bin/env bash
# Builds the synthetic benchmark ROM (XBA-151): gen_rom.py's C compiled with devkitARM in the xba-gba-homebrew image
# (tools/docker/gba-homebrew.Dockerfile, built by tools/consoles/build_homebrew.sh), no network.
#
#   consoles/gba/tests/bench/build_rom.sh OUT.gba [gen_rom.py args...]     e.g. --functions 1000 --seed 1
#
# Our own code, never committed (write it under build/). Thumb at -O2, as the homebrew corpus.
set -euo pipefail
out=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
cid=
cleanup() {
    [ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true
    rm -rf "$tmp"
}
trap cleanup EXIT
mkdir -p "$tmp/bench" "$tmp/common"
python3 "$here/gen_rom.py" "$tmp/bench" "$@"
cp "$here/../homebrew/common/"*.h "$tmp/common/"
cid=$(docker create --network none -w /tmp/work/bench xba-gba-homebrew bash -ec '
    ARCH="-mcpu=arm7tdmi -mtune=arm7tdmi -mthumb-interwork"
    CC=$DEVKITARM/bin/arm-none-eabi-gcc
    mkdir -p /tmp/obj /out
    ls *.c | xargs -P "${JOBS:-4}" -I{} sh -c "$CC $ARCH -mthumb -O2 -std=gnu11 -ffunction-sections -w \
        -I$DEVKITPRO/libtonc/include -c {} -o /tmp/obj/{}.o"
    $CC $ARCH -specs=gba.specs -L$DEVKITPRO/libtonc/lib -o /out/bench.elf /tmp/obj/*.o -ltonc
    $DEVKITARM/bin/arm-none-eabi-objcopy -O binary /out/bench.elf /out/bench.gba
    gbafix -tXBABENCH -cXBAB -m00 /out/bench.gba >/dev/null')
docker cp "$tmp/." "$cid:/tmp/work"
timeout 1800 docker start -a "$cid"
mkdir -p "$(dirname "$out")"
docker cp "$cid:/out/bench.gba" "$out"
ls -l "$out"
