# syntax=docker/dockerfile:1
# N64 homebrew toolchain (XBA-149): libdragon's mips64-elf GCC toolchain + libdragon built from a
# pinned commit, to build our own test programs in consoles/n64/tests/homebrew/ as .z64 ROMs.
# Never used for commercial ROMs. x86_64 host (the toolchain .deb matches the host arch).
# Built and run by tools/consoles/build_homebrew.sh; by hand, from the repo root:
#   docker build -f tools/docker/n64-homebrew.Dockerfile -t xba-n64-homebrew tools/docker
#
# Toolchain: libdragon only publishes it as a rolling "toolchain-continuous-prerelease" (GCC 16.2.0,
# binutils 2.45, newlib 4.4.0, built 2026-09-15 from trunk). We pin the .deb by sha256: when upstream
# replaces it the build fails on the checksum, on purpose. Then either take the new one (update
# N64_TOOLCHAIN_SHA256 and LIBDRAGON_COMMIT together) or point N64_TOOLCHAIN_URL at our own copy
# (GPL binaries, mirroring is allowed). libdragon is pinned by commit (the commit id is a content hash).
# Licences: libdragon Unlicense (public domain), GCC/binutils GPLv3, newlib BSD-like
# (see consoles/n64/tests/homebrew/README.md).
FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5

ARG N64_TOOLCHAIN_URL=https://github.com/DragonMinded/libdragon/releases/download/toolchain-continuous-prerelease/gcc-toolchain-mips64-x86_64.deb
ARG N64_TOOLCHAIN_SHA256=fac8e6572493a66468b7d45df41b042f90b1a630ec96aa218bd5fb1f320a0a4d
ARG LIBDRAGON_COMMIT=e356bf3f56f7afbf7e5246329562f145965cfdfc
ARG JOBS=4

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl git make gcc g++ libc6-dev python3 xxd \
    && rm -rf /var/lib/apt/lists/*

ENV N64_INST=/opt/libdragon
ENV PATH=${N64_INST}/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

RUN curl -fsSL -o /tmp/n64-toolchain.deb "${N64_TOOLCHAIN_URL}" \
    && echo "${N64_TOOLCHAIN_SHA256}  /tmp/n64-toolchain.deb" | sha256sum -c - \
    && dpkg -i /tmp/n64-toolchain.deb \
    && rm /tmp/n64-toolchain.deb \
    && mips64-elf-gcc --version | head -1

# libdragon library + host tools (n64tool, n64sym, mksprite, ...), installed into $N64_INST.
RUN git init -q /tmp/libdragon \
    && cd /tmp/libdragon \
    && git fetch -q --depth 1 https://github.com/DragonMinded/libdragon.git "${LIBDRAGON_COMMIT}" \
    && git checkout -q FETCH_HEAD \
    && test "$(git rev-parse HEAD)" = "${LIBDRAGON_COMMIT}" \
    && make -j"${JOBS}" install-mk \
    && make -j"${JOBS}" libdragon tools \
    && make -j"${JOBS}" install tools-install \
    && cd / && rm -rf /tmp/libdragon \
    && test -f "${N64_INST}/include/n64.mk" && command -v n64tool

WORKDIR /work
