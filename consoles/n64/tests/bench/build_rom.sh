#!/usr/bin/env bash
# Builds the N64 run-time benchmark ROM (XBA-277): main.c with libdragon in the xba-n64-homebrew image
# (tools/docker/n64-homebrew.Dockerfile, built by tools/consoles/build_homebrew.sh), no network.
#
#   consoles/n64/tests/bench/build_rom.sh OUT.z64
#
# Our own code, never committed (write it under build/). Then, e.g.:
#   tools/consoles/n64_recomp_build.py OUT.z64 WORK --loop 4 --n64-run build/.../n64_run
#   n64_run OUT.z64 --frames 60 --pack WORK/libtitle.so --stats        (frames per second of lifted code)
set -euo pipefail
out=$1
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
cid=
cleanup() {
    [ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true
    rm -rf "$tmp"
}
trap cleanup EXIT
mkdir -p "$tmp/bench/common"
cp "$here/main.c" "$tmp/bench/"
cp "$here/../homebrew/common/xba_result.h" "$tmp/bench/common/"
cat >"$tmp/bench/Makefile" <<'EOF'
BUILD_DIR = /tmp/build
SOURCE_DIR = .
N64_ROM_ELFCOMPRESS := 0
include $(N64_INST)/include/n64.mk
N64_CFLAGS += -Werror -Icommon
all: bench.z64
$(BUILD_DIR)/bench.elf: $(BUILD_DIR)/main.o
bench.z64: N64_ROM_TITLE = "XBA bench"
EOF
cid=$(docker create --network none -w /tmp/work/bench xba-n64-homebrew bash -ec 'make -j2 && mkdir -p /out && cp bench.z64 /out/')
docker cp "$tmp/." "$cid:/tmp/work"
timeout 1800 docker start -a "$cid"
mkdir -p "$(dirname "$out")"
docker cp "$cid:/out/bench.z64" "$out"
ls -l "$out"
