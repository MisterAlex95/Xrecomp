# syntax=docker/dockerfile:1
# A Windows build check without Windows: the runtime libraries cross-compiled with MinGW-w64
# (cmake/mingw-w64-x86_64.cmake). It compiles the Windows-only paths (the kernel's CreateProcess hand-over to another
# image and quick reboot, kernel_reboot.c, XBA-82) but runs nothing, and MSVC stays the reference environment.
# From the repo root:
#   docker build -f tools/docker/windows-mingw.Dockerfile .
# A compile error fails the build. Not part of CI (yet).
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake ninja-build gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /work/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
        -DXBOXRECOMP_BUILD_TESTS=OFF \
    && cmake --build /work/build \
    && ls /work/build/src/*/*.a \
    && rm -rf /work
