# syntax=docker/dockerfile:1
# NDS homebrew toolchain (XBA-223): devkitARM + libnds + calico + ndstool, to build our own test programs in
# consoles/nds/tests/homebrew/. Never used for commercial ROMs, firmware or a BIOS.
# Built and run by tools/consoles/build_homebrew.sh; by hand, from the repo root:
#   docker build -f tools/docker/nds-homebrew.Dockerfile -t xba-nds-homebrew tools/docker
#
# Base: the same devkitPro image as the GBA corpus (tools/docker/gba-homebrew.Dockerfile), pinned by the same tag
# and digest, so the two images share every layer (no extra download or disk). devkitARM ships libnds, calico (its
# ARM9 / ARM7 runtime and the prebuilt ARM7 programs) and ndstool. Licences: libnds zlib
# (/opt/devkitpro/libnds/libnds_license.txt), calico as its package declares (Zope), ndstool GPLv3 (a tool only),
# newlib BSD-like.
# Bump together with gba-homebrew.Dockerfile.
FROM devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5

ENV DEVKITPRO=/opt/devkitpro \
    DEVKITARM=/opt/devkitpro/devkitARM \
    PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# Fail the image build early if the pieces the corpus relies on are missing.
RUN test -f /opt/devkitpro/libnds/include/nds.h \
    && test -f /opt/devkitpro/libnds/lib/libnds9.a \
    && test -f /opt/devkitpro/calico/lib/libcalico_ds9.a \
    && test -f /opt/devkitpro/calico/bin/ds7_maine.elf \
    && arm-none-eabi-gcc --version | head -1 \
    && command -v ndstool make

WORKDIR /work
