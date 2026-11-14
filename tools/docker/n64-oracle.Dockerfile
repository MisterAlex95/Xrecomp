# N64 test oracle (XBA-149): ares, built from a pinned commit with only its N64 core. ares has no headless mode, so
# tools/consoles/run_homebrew.sh runs it under Xvfb with its null video/audio drivers and reads our result block
# through ares's GDB server (tools/consoles/oracle/gdb_poll.py). The RDP runs on paraLLEl-RDP over Mesa's lavapipe
# (CPU Vulkan), as on a desktop without a GPU. ares HLEs the PIF boot, so no Nintendo boot ROM is needed: our ROMs
# carry libdragon's open-source IPL3. An external oracle only: nothing of ares is copied into or linked with this
# repository's code, and the image is never published.
#
# Licences: ares ISC (paraLLEl-RDP MIT), SDL3 zlib, Mesa MIT, Xvfb MIT/X11; the prebuilt ares-deps archive its CMake fetches
# (librashader MPL-2.0, slang-shaders, pinned by sha256 in ares' deps.json).
# To bump: change ARES_COMMIT and re-run tools/consoles/run_homebrew.sh n64.
#   docker build -f tools/docker/n64-oracle.Dockerfile -t xba-n64-oracle tools/docker
FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5

ARG ARES_COMMIT=0b928930af4cc59ed99e942996f0d3c815379388
ARG SDL3_COMMIT=d9d5536704d585616d4db3c8ba3c4ff6fc2757e1
ARG JOBS=4

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates git cmake ninja-build g++ pkg-config xz-utils \
        libgtk-3-dev libgl-dev libx11-dev libxrandr-dev \
        xvfb xauth mesa-vulkan-drivers libvulkan1 libgl1-mesa-dri python3 \
    && rm -rf /var/lib/apt/lists/*

# SDL3 (ares' audio/input layer needs its headers; Ubuntu 24.04 has no package): release-3.4.8, pinned by commit,
# minimal (no tests); in the container only its dummy audio driver is ever used.
RUN git init -q /sdl \
    && cd /sdl \
    && git fetch -q --depth 1 https://github.com/libsdl-org/SDL.git "${SDL3_COMMIT}" \
    && git checkout -q FETCH_HEAD \
    && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib \
    && cmake --build build -j "${JOBS}" \
    && cmake --install build \
    && ldconfig \
    && cd / && rm -rf /sdl

RUN git init -q /src \
    && cd /src \
    && git fetch -q --depth 1 https://github.com/ares-emulator/ares.git "${ARES_COMMIT}" \
    && git checkout -q FETCH_HEAD \
    && test "$(git rev-parse HEAD)" = "${ARES_COMMIT}" \
    && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DARES_CORES=n64 -DENABLE_IPO=OFF \
        -DENABLE_CCACHE=OFF -DARES_BUILD_LOCAL=OFF -DARES_BUILD_OPTIONAL_TARGETS=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/ares \
    && cmake --build build -j "${JOBS}" \
    && cmake --install build \
    && mkdir -p /opt/ares/licenses && cp LICENSE /opt/ares/licenses/ares-LICENSE \
    && echo "ares ${ARES_COMMIT}" > /opt/ares/VERSION \
    && cd / && rm -rf /src

ENV PATH=/opt/ares/bin:${PATH}
RUN command -v ares && cat /opt/ares/VERSION

WORKDIR /run
