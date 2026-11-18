# syntax=docker/dockerfile:1
# Xbox 360 test-title toolchain (XBA-270): devkitPPC, to build our own test programs in consoles/x360/tests/homebrew/
# (-mcpu=970 -mpowerpc64 -maltivec: the 64-bit PowerPC and VMX instructions Xenon has). No Xbox 360 SDK, no
# Microsoft code: the programs declare the kernel exports they call themselves (kernel.h) and are packed into a
# synthetic XEX2 at test time (elf2xex.py). Built and run by tools/consoles/build_homebrew.sh x360.
#
# Base: devkitPro's official devkitPPC image, the GameCube corpus's (same tag and digest). Licences: GCC / binutils
# GPL (tools only), libgcc with the runtime library exception.
FROM devkitpro/devkitppc:20261016@sha256:d50f8a886f2ce1d3c22685a774afb0845379876b47d9b41a9b729f4f63a54813

ENV DEVKITPRO=/opt/devkitpro \
    DEVKITPPC=/opt/devkitpro/devkitPPC \
    PATH=/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

RUN powerpc-eabi-gcc --version | head -1 && command -v make
