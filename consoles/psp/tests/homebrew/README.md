# PSP homebrew corpus (XBA-224)

Our own test programs for the PSP spike, so it can be tested headless without a UMD, a firmware dump or any
commercial module.

| Program | What it exercises | Output |
|---|---|---|
| `hello` | PSPSDK's crt0 (module_start, the `user_main` thread, libcglue: newlib heap, LwMutexes, semaphores), the debug screen on the 480x272 32-bit framebuffer, an Allegrex check, `sceCtrl`, 30 VBlanks, `sceKernelExitGame` | `hello.prx`, `hello.pbp` (EBOOT.PBP), `hello.elf` |

`hello`'s framebuffer when it exits is `consoles/psp/tests/hello.png`: `Allegrex: ok` means each of EXT, INS, SEB, SEH,
WSBH, WSBW, BITREV, CLZ, CLO, MAX, MIN, MOVZ / MOVN, MADD / MSUBU, ROTR, ROTRV, BEQL / BNEL (slot skipped when not
taken) and a few FPU operations (MUL.S, DIV.S, ADD.S, TRUNC.W.S, C.LT.S, SQRT) gave the value the MIPS32 / Allegrex
definitions give; otherwise `Allegrex: FAIL` and a mask of the failed checks (bit n = check n in `hello/main.c`).

## Build

```sh
tools/consoles/build_homebrew.sh psp [OUTDIR]     # default OUTDIR: build/homebrew -> build/homebrew/psp/
```

The image `tools/docker/psp-homebrew.Dockerfile` is pspdev's official `pspdev/pspdev` (Alpine, about 790 MB unpacked,
pinned by digest); `make` runs inside it without network, each program through PSPSDK's own `build.mak`
(`prog.mak`: `BUILD_PRX=1`, `EXTRA_TARGETS = EBOOT.PBP`, `-O2 -G0`). Outputs (`.prx`, `.pbp`, `.elf`, `SHA256SUMS`) are
never committed.

## Toolchain and licences

| Component | Licence |
|---|---|
| psp-gcc 15.2 / binutils (`pspdev/pspdev`, pinned by digest) | GPLv3 (tools only) |
| PSPSDK (linked: crt0, libcglue, libpspdebug and its font, the NID stub libraries; `build.mak`, `psp-prxgen`, `pack-pbp`) | BSD (`/usr/local/pspdev/psp/share/licenses/pspsdk`) |
| newlib (linked) | BSD-style licences (`.../licenses/newlib`) |
| pthread-embedded (linked: libcglue's start-up initialises it) | LGPL-2.1 (`.../licenses/pthread-embedded`): fine for test binaries that are built in CI and never distributed, which is all these are |

The expected picture holds PSPSDK's debug screen font (BSD).
