# syntax=docker/dockerfile:1
# Linux arm64 baseline (XBA-14) without an arm64 host: cross-compiles the toolkit and every C test project for
# aarch64 (Debian multiarch) and runs the tests under qemu-aarch64 (user mode, no binfmt needed on the host).
#   docker build -f tools/docker/linux-arm64.Dockerfile --target test .
# qemu-user is not a real arm64 host: the trapped-MMIO tests (SIGSEGV on PROT_NONE pages, mmio_decode_a64) and the
# timing-sensitive ones can differ. A pass here is a good sign, a failure has to be confirmed on real hardware.
# The Python side does not depend on the host CPU: it is covered by linux.Dockerfile.
FROM debian:trixie-slim AS deps
RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake ninja-build pkg-config crossbuild-essential-arm64 qemu-user file \
        libsdl2-dev:arm64 libgl1-mesa-dev:arm64 libepoxy-dev:arm64 libssl-dev:arm64 \
        libavcodec-dev:arm64 libavformat-dev:arm64 libavutil-dev:arm64 libswscale-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*

FROM deps AS test
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
# The expected-failures checker (tools/ci/check_expected.py). Its own layer, so the deps layer above stays cached.
RUN apt-get update && apt-get install -y --no-install-recommends python3 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
ARG JOBS=4
# RECOMP_GUEST_WINDOW=1: the memory layout Android uses by default (XBA-21); without it the contiguous window at
# base + 2 GB lands on a taken range under qemu-user, as it would in an app process.
ENV SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy JOBS=${JOBS} RECOMP_GUEST_WINDOW=1 \
    XBR_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchains/linux-aarch64-cross.cmake" \
    XBR_JUNIT_DIR=/reports/junit
# /work removed at the end of the RUN, as in linux.Dockerfile (multi-GB sparse images from kernel_fd_limit).
# A build break fails at once. The tests run to the end whatever fails (under qemu a failure is a lead, not a
# verdict), the report is printed, then tools/ci/check_expected.py fails the build on any failure that is not in
# tools/ci/arm64-expected.txt, and on any line there that no longer fails (XBA-53).
RUN mkdir -p /work && trap 'rm -rf /work; find /tmp -mindepth 1 -delete' EXIT \
    && export TMPDIR=/work TMP=/work TEMP=/work \
    && mkdir -p /reports/junit \
    && cmake -S . -B /work/build/root -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo $XBR_CMAKE_ARGS \
    && cmake --build /work/build/root -j"$JOBS" 2>&1 | tee /reports/build-root.log \
    && (ctest --test-dir /work/build/root --output-on-failure --timeout 600 --output-junit /reports/junit/root.xml || true) \
    && (tools/run_c_tests.sh /work/build/c-tests /reports/c-tests.md || true) \
    && (file /work/build/c-tests/mmx_helpers/mmx_helpers_test* || true) | tee /reports/arch.txt \
    && awk -F'|' 'NR > 2 && ($3 ~ /FAIL/ || $4 ~ /FAIL/ || $7 + 0 > 0) { gsub(/ /, "", $2); print $2 }' /reports/c-tests.md \
         | while read -r p; do echo "=== $p (log tail)"; tail -n 60 "/work/build/c-tests/$p.log"; done \
    && echo "=== arm64 C test report" && cat /reports/c-tests.md \
    && echo "=== Against tools/ci/arm64-expected.txt" \
    && python3 tools/ci/check_expected.py ctest --expected tools/ci/arm64-expected.txt \
         --report /reports/c-tests.md --junit /reports/junit
