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
WORKDIR /src
COPY . .
ENV SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    XBR_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchains/linux-aarch64-cross.cmake"
# tmpfs on /tmp, as in linux.Dockerfile (multi-GB sparse images from kernel_fd_limit). The C test run does not fail
# the build here: under qemu a failure is a lead, not a verdict; the report says what failed.
RUN --mount=type=tmpfs,target=/tmp \
    mkdir -p /reports \
    && cmake -S . -B /tmp/build/root -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo $XBR_CMAKE_ARGS \
    && cmake --build /tmp/build/root -j"$(nproc)" 2>&1 | tee /reports/build-root.log \
    && (ctest --test-dir /tmp/build/root --output-on-failure --timeout 600 || true) \
    && (tools/run_c_tests.sh /tmp/build/c-tests /reports/c-tests.md || true) \
    && file /tmp/build/c-tests/mmx_helpers/mmx_helpers_test* | tee /reports/arch.txt
