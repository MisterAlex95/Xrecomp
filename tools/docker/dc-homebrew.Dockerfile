# syntax=docker/dockerfile:1
# Dreamcast homebrew toolchain (XBA-227): KallistiOS 2.1 with its sh-elf GCC 14.2, to build our own test programs in
# consoles/dc/tests/homebrew/. Never used for commercial discs, a BIOS or a flash dump.
# Built and run by tools/consoles/build_homebrew.sh; by hand, from the repo root:
#   docker build -f tools/docker/dc-homebrew.Dockerfile -t xba-dc-homebrew tools/docker
#
# Base: KallistiOS's own toolchain image (Docker Hub kallistios/dc-kos-toolchain, built by the KallistiOS project:
# sh-elf + arm-eabi GCC, newlib, KOS and kos-ports, prebuilt libkallisti), pinned by tag and digest. About 430 MB
# compressed, 1.55 GB on disk; amd64 and arm64. Licences: KallistiOS the KOS License, "new" BSD
# (/opt/toolchains/dc/kos/doc/LICENSE.md), newlib BSD-like, GCC / binutils GPLv3 (tools only), libgcc with the GCC
# runtime exception.
FROM kallistios/dc-kos-toolchain:14.2-stable@sha256:759066b3bc0d162d7e1fec4f20107446f5df255f57a8c6b80d10b44723107b56

# The image's entrypoint sources environ.sh.master; `docker create ... bash -ec` bypasses it, so the build
# commands source it themselves. Fail the image build early if the pieces the corpus relies on are missing.
RUN bash -c '. /opt/toolchains/dc/kos/environ.sh.master \
    && test -f "$KOS_BASE/lib/dreamcast/libkallisti.a" \
    && test -f "$KOS_BASE/kernel/arch/dreamcast/include/dc/minifont.h" \
    && "$KOS_CC" --version | head -1 \
    && command -v kos-cc make'

WORKDIR /work
