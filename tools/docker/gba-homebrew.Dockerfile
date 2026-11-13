# syntax=docker/dockerfile:1
# GBA homebrew toolchain (XBA-149): devkitARM + libtonc + libgba, to build our own test programs in
# consoles/gba/tests/homebrew/. Never used for commercial ROMs or a BIOS.
# Built and run by tools/consoles/build_homebrew.sh; by hand, from the repo root:
#   docker build -f tools/docker/gba-homebrew.Dockerfile -t xba-gba-homebrew tools/docker
#
# Base: devkitPro's official image from Docker Hub, pinned by tag and digest (devkitARM GCC 16.1.0,
# libtonc, libgba, gbafix, grit). We pull it from devkitPro's official channel and never push or
# redistribute it. Licences: GCC/binutils GPLv3, newlib BSD-like, libtonc MIT, libgba/libgba-maxmod
# per /opt/devkitpro/libgba/*license.txt (see consoles/gba/tests/homebrew/README.md).
# To bump: pick a tag on hub.docker.com/r/devkitpro/devkitarm/tags, update tag + digest together.
FROM devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5

ENV DEVKITPRO=/opt/devkitpro \
    DEVKITARM=/opt/devkitpro/devkitARM \
    PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# Fail the image build early if the pieces the corpus relies on are missing.
RUN test -f /opt/devkitpro/libtonc/include/tonc.h \
    && test -f /opt/devkitpro/libgba/include/gba.h \
    && arm-none-eabi-gcc --version | head -1 \
    && command -v gbafix make

WORKDIR /work
