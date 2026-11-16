# syntax=docker/dockerfile:1
# PS2 homebrew toolchain (XBA-226): the ps2dev project's official image (mips64r5900el-ps2-elf-gcc 15.2, binutils,
# newlib, PS2SDK with its EE crt0, linkfile and libkernel), to build our own test programs in
# consoles/ps2/tests/homebrew/. Never used for commercial games, disc images or a BIOS.
# Built and run by tools/consoles/build_homebrew.sh; by hand, from the repo root:
#   docker build -f tools/docker/ps2-homebrew.Dockerfile -t xba-ps2-homebrew tools/docker
#
# Base: ps2dev/ps2dev v2.0.0 (Alpine, about 1 GB unpacked, 281 MB download), pinned by digest. It has no make and no
# bash, which the corpus build uses: added from Alpine's repository (a few MB).
# Licences: PS2SDK AFL-2.0 (linked: crt0, libkernel, libcglue), newlib BSD-like (linked), GCC / binutils GPLv3
# (tools only). See consoles/ps2/tests/homebrew/README.md.
FROM ps2dev/ps2dev:v2.0.0@sha256:8e6dc4564598ecce94d078d3111b8c4803f88faa94a6f183ac60240a0ae010a1

ENV PS2DEV=/usr/local/ps2dev \
    PS2SDK=/usr/local/ps2dev/ps2sdk \
    PATH=/usr/local/ps2dev/bin:/usr/local/ps2dev/ee/bin:/usr/local/ps2dev/iop/bin:/usr/local/ps2dev/ps2sdk/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

RUN apk add --no-cache make bash

# Fail the image build early if the pieces the corpus relies on are missing.
RUN test -f /usr/local/ps2dev/ps2sdk/ee/startup/linkfile \
    && test -f /usr/local/ps2dev/ps2sdk/ee/include/sio.h \
    && test -f /usr/local/ps2dev/ps2sdk/ee/lib/libkernel.a \
    && mips64r5900el-ps2-elf-gcc --version | head -1 \
    && command -v make bash

WORKDIR /work
