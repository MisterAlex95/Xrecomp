# syntax=docker/dockerfile:1
# Builds the toolkit and runs every upstream test (ctest + pytest) in a clean Debian container.
# This is the fork's Linux baseline (XBA-13): an Android change must keep it green.
# From the repo root:
#   docker build -f tools/docker/linux.Dockerfile --target test .
# Same packages as tools/linux/install_deps.sh (Debian branch), plus the Python side of CONTRIBUTING.md.
# The test stage runs ctest and pytest at build time: a red test fails the build.
FROM debian:trixie-slim AS deps
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake build-essential clang pkg-config ninja-build \
        libsdl2-dev libgl1-mesa-dev libepoxy-dev libssl-dev \
        libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
        python3 python3-venv \
    && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/venv && /opt/venv/bin/pip install --quiet --no-cache-dir capstone pytest pefile numpy
ENV PATH=/opt/venv/bin:$PATH

FROM deps AS test
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
WORKDIR /src
COPY . .
# Headless: SDL picks its dummy drivers, the tests that need a real display or audio device skip (exit 77).
ENV SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
# Everything runs in a tmpfs on /tmp: some tests write multi-GB sparse partition images there (kernel_fd_limit),
# which would fill the host's disk once frozen into an image layer. Only the reports stay in the image.
# 1. the toolkit itself, and the two tests the root build lists
# 2. every self-contained C test project under tests/ (report in /reports/c-tests.md)
# 3. the Python side (lifter unit tests)
RUN --mount=type=tmpfs,target=/tmp \
    mkdir -p /reports \
    && cmake -S . -B /tmp/build/root -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXBOXRECOMP_BUILD_TESTS=ON \
    && cmake --build /tmp/build/root -j"$(nproc)" \
    && ctest --test-dir /tmp/build/root --output-on-failure --timeout 300 \
    && tools/run_c_tests.sh /tmp/build/c-tests /reports/c-tests.md \
    && python3 -m pytest -q -p no:cacheprovider tools/ | tee /reports/pytest.txt
