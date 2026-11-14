# syntax=docker/dockerfile:1
# PS1 test oracle (XBA-149): PCSX-Redux, built from a pinned commit, with OpenBIOS (its open-source BIOS
# replacement, built from source in the same tree: no Sony BIOS dump anywhere). tools/consoles/run_homebrew.sh
# runs our homebrew corpus in it headless (-no-ui, a Lua script polls the result block at each VSync) and reads
# the verdict. An external oracle only: nothing of PCSX-Redux is copied into or linked with this repository's
# code, and the image is never published.
#
# Licences: PCSX-Redux GPL-2.0-or-later, OpenBIOS (pcsx-redux/nugget) MIT, the build image's MIPS GCC GPLv3.
# The base is PCSX-Redux's own CI build image (Ubuntu 24.04 + its deps, SDL3 and the mipsel-none-elf toolchain it
# builds OpenBIOS with), pinned by digest. To bump: change PCSX_REDUX_COMMIT (and the base digest if their CI moved
# to a newer image) and re-run tools/consoles/run_homebrew.sh ps1.
#   docker build -f tools/docker/ps1-oracle.Dockerfile -t xba-ps1-oracle tools/docker
FROM ghcr.io/pcsx-redux/pcsx-redux-build:noble@sha256:20fece3d1e3b3c3e0a42bce9b986fb659b12bd441352df55abd862c381fb48f2

ARG PCSX_REDUX_COMMIT=b3eb32535cac5c8dab7c966d2ae87f7abad59d8c
ARG JOBS=4

# One layer: the source tree (about 2 GB with the submodules) is gone by the end of it.
RUN git init -q /src \
    && cd /src \
    && git fetch -q --depth 1 https://github.com/grumpycoders/pcsx-redux.git "${PCSX_REDUX_COMMIT}" \
    && git checkout -q FETCH_HEAD \
    && test "$(git rev-parse HEAD)" = "${PCSX_REDUX_COMMIT}" \
    && (git submodule update --init --recursive --depth 1 -j 4 \
        || CI= ./.github/scripts/update-submodules.sh) \
    && make -j"${JOBS}" all \
    && make -C src/mips/openbios -j"${JOBS}" all \
    && make install DESTDIR=/opt/pcsx-redux \
    && cp src/mips/openbios/openbios.bin /opt/pcsx-redux/share/pcsx-redux/resources/ \
    && mkdir -p /opt/pcsx-redux/licenses \
    && cp LICENSE /opt/pcsx-redux/licenses/pcsx-redux-LICENSE \
    && cp src/mips/LICENSE /opt/pcsx-redux/licenses/nugget-openbios-LICENSE \
    && echo "pcsx-redux ${PCSX_REDUX_COMMIT}, nugget $(git -C src/mips rev-parse HEAD)" > /opt/pcsx-redux/VERSION \
    && cd / && rm -rf /src

ENV PATH=/opt/pcsx-redux/bin:${PATH}
RUN test -s /opt/pcsx-redux/share/pcsx-redux/resources/openbios.bin && cat /opt/pcsx-redux/VERSION

WORKDIR /run
