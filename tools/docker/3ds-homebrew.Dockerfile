# syntax=docker/dockerfile:1
# 3DS homebrew toolchain (XBA-228): devkitARM + libctru + 3dsxtool, to build our own test programs in
# consoles/3ds/tests/homebrew/. Never used for commercial titles, system files or keys.
# Built and run by tools/consoles/build_homebrew.sh; by hand, from the repo root:
#   docker build -f tools/docker/3ds-homebrew.Dockerfile -t xba-3ds-homebrew tools/docker
#
# Base: the same devkitPro image as the GBA and DS corpora (gba-homebrew.Dockerfile, nds-homebrew.Dockerfile), pinned
# by the same tag and digest, so the three images share every layer (no extra download or disk). devkitARM ships the
# 3ds-dev group: libctru 2.7.0 and 3dstools (3dsxtool). Licences: libctru zlib, newlib BSD-like, 3dsxtool GPLv3 (a
# tool only).
# Bump together with gba-homebrew.Dockerfile.
FROM devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5

ENV DEVKITPRO=/opt/devkitpro \
    DEVKITARM=/opt/devkitpro/devkitARM \
    PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# Fail the image build early if the pieces the corpus relies on are missing.
RUN test -f /opt/devkitpro/libctru/include/3ds.h \
    && test -f /opt/devkitpro/libctru/lib/libctru.a \
    && test -f /opt/devkitpro/devkitARM/arm-none-eabi/lib/3dsx.specs \
    && arm-none-eabi-gcc --version | head -1 \
    && command -v 3dsxtool make

WORKDIR /work
