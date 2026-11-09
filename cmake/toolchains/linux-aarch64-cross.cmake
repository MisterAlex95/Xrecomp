# Cross-compiles for Linux arm64 from an x86-64 Debian host with the arm64 multiarch packages
# (tools/docker/linux-arm64.Dockerfile). ctest runs each test binary through qemu-aarch64.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64;-L;/usr/aarch64-linux-gnu)
# Debian multiarch: the arm64 libraries are in /usr/lib/aarch64-linux-gnu, next to the host's.
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig)
