# syntax=docker/dockerfile:1
# The i686 MinGW-w64 cross compiler for our own Win32 test programs (XBA-233, epic XBA-231: Windows PC games).
# Builds tests/win32_hello (console hello worlds of our own) into 32-bit PEs that the x86 pipeline recompiles.
# Never used on game code. tools/win32/build_hello.sh builds and uses this image when no i686-w64-mingw32-gcc is
# on the PATH; by hand, from the repo root:
#   docker build -f tools/docker/win32-mingw.Dockerfile -t xba-win32-mingw tools/docker
# Licences: GCC/binutils GPLv3, the MinGW-w64 runtime and headers ZPL / public domain.
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc-mingw-w64-i686-win32 \
    && rm -rf /var/lib/apt/lists/* \
    && i686-w64-mingw32-gcc --version | head -1
WORKDIR /src
