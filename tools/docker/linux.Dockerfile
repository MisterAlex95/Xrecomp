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
        python3 python3-venv ccache \
    && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/venv && /opt/venv/bin/pip install --quiet --no-cache-dir capstone pytest pefile numpy
ENV PATH=/opt/venv/bin:$PATH

FROM deps AS test
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
# git for tools/test_sync_upstream.py (fixture repositories). Its own layer, so the deps layer above stays cached.
RUN apt-get update && apt-get install -y --no-install-recommends git && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# Headless: SDL picks its dummy drivers, the tests that need a real display or audio device skip (exit 77).
ARG JOBS=4
ENV SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy JOBS=${JOBS} \
    XBA_E2E=1
# XBA_E2E=1: tools/compat/test_e2e.py builds a synthetic title against this tree and boots it through the compat
# runner (a few seconds): the profile layers, the template and the runtime linked together, run headless.
# Builds and temp files go to /work (and some tests write to /tmp directly), both emptied at the end of the same RUN:
# some tests write multi-GB sparse partition
# images to the temp dir (kernel_fd_limit), which would fill the disk once frozen into an image layer. Only the
# reports stay in the image. Not a tmpfs: on the CI host (shared with Gitea and Plane) a RAM tmpfs of several GB per
# job made it thrash on 2026-11-09. JOBS caps the parallel compiles for the same reason.
# 1. the toolkit itself, and the two tests the root build lists
# 2. every self-contained C test project under tests/ (report in /reports/c-tests.md)
# 3. the Python side (lifter unit tests, and the console pipelines' under consoles/, XBA-146)
# PART splits that for parallel CI jobs (.github/workflows/linux.yml): all (default, everything, as Gitea runs it),
# python (1 and 3), or c-K/N (the K-th of N slices of 2, tools/run_c_tests.sh XBR_SHARD).
# ccache (tools/run_c_tests.sh): the projects that rebuild the runtime share its objects within a run, and the cache
# mount keeps them between runs on the same host (BuildKit, capped at CCACHE_MAXSIZE; a fresh GitHub runner starts
# empty). The env makes every CMake configure here use it, the root build and pytest's builds included.
ENV CCACHE_DIR=/ccache CCACHE_MAXSIZE=2G CCACHE_BASEDIR=/ CCACHE_NOHASHDIR=1 \
    CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
ARG PART=all
RUN --mount=type=cache,id=xba-ccache-linux,target=/ccache mkdir -p /work && trap 'rm -rf /work; find /tmp -mindepth 1 -delete' EXIT \
    && case "$PART" in all|python|c-[1-9]*/[1-9]*) ;; *) echo "PART: all, python or c-K/N, not $PART"; exit 2 ;; esac \
    && export TMPDIR=/work TMP=/work TEMP=/work \
    && mkdir -p /reports \
    && case "$PART" in all|python) \
         cmake -S . -B /work/build/root -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXBOXRECOMP_BUILD_TESTS=ON \
         && cmake --build /work/build/root -j"$JOBS" \
         && (ulimit -c 0; ctest --test-dir /work/build/root --output-on-failure --timeout 300) ;; esac \
    && case "$PART" in all) tools/run_c_tests.sh /work/build/c-tests /reports/c-tests.md ;; \
         c-*) XBR_SHARD=${PART#c-} tools/run_c_tests.sh /work/build/c-tests /reports/c-tests.md ;; esac \
    && case "$PART" in all|python) python3 -m pytest -q -p no:cacheprovider tools/ consoles/ | tee /reports/pytest.txt ;; esac \
    && ccache -s | tee /reports/ccache.txt
