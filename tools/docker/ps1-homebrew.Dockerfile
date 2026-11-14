# syntax=docker/dockerfile:1
# PS1 homebrew toolchain (XBA-149): PSn00bSDK (with its bundled mipsel-none-elf GCC 12.3.0, mkpsxiso
# and dumpsxiso) to build our own test programs in consoles/ps1/tests/homebrew/ as PS-X EXEs and
# BIN/CUE disc images. Never used for commercial discs or a BIOS. x86_64 host only (the SDK's
# release binaries are x86_64).
# Built and run by tools/consoles/build_homebrew.sh; by hand, from the repo root:
#   docker build -f tools/docker/ps1-homebrew.Dockerfile -t xba-ps1-homebrew tools/docker
#
# Licences: PSn00bSDK MPL-2.0, mkpsxiso/dumpsxiso GPL-2.0-or-later, bundled GCC GPLv3
# (see consoles/ps1/tests/homebrew/README.md).
# To bump: change PSN00BSDK_VERSION and PSN00BSDK_SHA256 together (sha256sum of the release zip).
FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5

ARG PSN00BSDK_VERSION=0.24
ARG PSN00BSDK_SHA256=5ada7c9478d795b22bde96ee23d6869290c14a740bcde3d57c01c342a26ded35

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl unzip cmake make python3 \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL -o /tmp/psn00bsdk.zip \
        "https://github.com/Lameguy64/PSn00bSDK/releases/download/v${PSN00BSDK_VERSION}/PSn00bSDK-${PSN00BSDK_VERSION}-Linux.zip" \
    && echo "${PSN00BSDK_SHA256}  /tmp/psn00bsdk.zip" | sha256sum -c - \
    && unzip -q /tmp/psn00bsdk.zip -d /opt \
    && mv "/opt/PSn00bSDK-${PSN00BSDK_VERSION}-Linux" /opt/psn00bsdk \
    && rm -rf /tmp/psn00bsdk.zip /opt/psn00bsdk/share/psn00bsdk/examples

ENV PSN00BSDK_LIBS=/opt/psn00bsdk/lib/libpsn00b \
    PATH=/opt/psn00bsdk/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

RUN mipsel-none-elf-gcc --version | head -1 && command -v mkpsxiso dumpsxiso elf2x ninja

WORKDIR /work
