# syntax=docker/dockerfile:1
# PSP homebrew toolchain (XBA-224): the pspdev project's official image (psp-gcc 15.2, binutils, newlib, PSPSDK,
# psp-prxgen, pack-pbp), to build our own test programs in consoles/psp/tests/homebrew/. Never used for commercial
# games, UMD dumps or firmware.
# Built and run by tools/consoles/build_homebrew.sh; by hand, from the repo root:
#   docker build -f tools/docker/psp-homebrew.Dockerfile -t xba-psp-homebrew tools/docker
#
# Base: pspdev/pspdev (Alpine, about 790 MB unpacked, 217 MB download), pinned by digest: `latest` moves with every
# pspdev build, the digest changes nothing here until it is bumped on purpose.
# Licences: PSPSDK BSD (linked: crt0, libcglue, libpspdebug, the NID stubs), newlib BSD-like (linked),
# pthread-embedded LGPL-2.1 (linked by libcglue; our test binaries are never distributed), GCC / binutils GPLv3
# (tools only). See consoles/psp/tests/homebrew/README.md.
FROM pspdev/pspdev:latest@sha256:ee385df3aa783189d5a033af5eb1d4a9cdd93718fb8bf652ec7bb48e54038a8b

ENV PSPDEV=/usr/local/pspdev \
    PATH=/usr/local/pspdev/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# Fail the image build early if the pieces the corpus relies on are missing.
RUN test -f /usr/local/pspdev/psp/sdk/lib/build.mak \
    && test -f /usr/local/pspdev/psp/sdk/include/pspdebug.h \
    && psp-gcc --version | head -1 \
    && command -v psp-prxgen pack-pbp psp-fixup-imports make bash

WORKDIR /work
