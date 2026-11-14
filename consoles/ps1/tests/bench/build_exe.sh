#!/usr/bin/env bash
# Builds the synthetic benchmark program (XBA-161): gen_exe.py's C compiled with PSn00bSDK (GCC, -O2, as the homebrew
# corpus) in the xba-ps1-homebrew image (tools/docker/ps1-homebrew.Dockerfile, built by
# tools/consoles/build_homebrew.sh), no network.
#
#   consoles/ps1/tests/bench/build_exe.sh OUT.exe [gen_exe.py args...]     e.g. --functions 1000 --seed 1
#
# Our own code, never committed (write it under build/).
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
python3 "$here/gen_exe.py" "$tmp/bench" "$@"
cp "$here/../homebrew/common/"*.h "$tmp/common/"
cat >"$tmp/bench/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.21)
project(xba-ps1-bench LANGUAGES C ASM)
file(GLOB srcs ${PROJECT_SOURCE_DIR}/*.c)
psn00bsdk_add_executable(bench NOGPREL ${srcs})
target_compile_options(bench PRIVATE -w -fno-strict-aliasing)
EOF
cid=$(docker create --network none -w /tmp/work/bench xba-ps1-homebrew bash -ec '
    cmake -S /tmp/work/bench -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$PSN00BSDK_LIBS/cmake/sdk.cmake" >/dev/null
    cmake --build /tmp/build -j "${JOBS:-4}" >/dev/null
    mkdir -p /out && cp /tmp/build/bench.exe /out/')
docker cp "$tmp/." "$cid:/tmp/work"
timeout 1800 docker start -a "$cid"
mkdir -p "$(dirname "$out")"
docker cp "$cid:/out/bench.exe" "$out"
ls -l "$out"
