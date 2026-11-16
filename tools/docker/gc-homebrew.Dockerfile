# syntax=docker/dockerfile:1
# GameCube homebrew toolchain (XBA-225): devkitPPC + libogc + elf2dol, to build our own test programs in
# consoles/gc/tests/homebrew/. Never used for commercial discs, the IPL or any other Nintendo code.
# Built and run by tools/consoles/build_homebrew.sh; by hand, from the repo root:
#   docker build -f tools/docker/gc-homebrew.Dockerfile -t xba-gc-homebrew tools/docker
#
# Base: devkitPro's official devkitPPC image, pinned by tag and digest (about 660 MB compressed, 2.6 GB on disk).
# It ships devkitPPC r51 (GCC 16.2, binutils 2.47), newlib, libogc 3.2.1 and gamecube-tools (elf2dol).
# Licences: libogc zlib-style (/opt/devkitpro/libogc/LICENSE; its threading code carries the RTEMS notice in the
# same file), newlib BSD-like, GCC / binutils / elf2dol GPL (tools only).
FROM devkitpro/devkitppc:20261016@sha256:d50f8a886f2ce1d3c22685a774afb0845379876b47d9b41a9b729f4f63a54813

ENV DEVKITPRO=/opt/devkitpro \
    DEVKITPPC=/opt/devkitpro/devkitPPC \
    PATH=/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# Fail the image build early if the pieces the corpus relies on are missing.
RUN test -f /opt/devkitpro/libogc/include/gccore.h \
    && test -f /opt/devkitpro/libogc/lib/cube/libogc.a \
    && powerpc-eabi-gcc --version | head -1 \
    && command -v elf2dol make

WORKDIR /work
