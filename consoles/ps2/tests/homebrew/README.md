# PS2 homebrew corpus (XBA-226)

Our own test programs for the PS2 spike, so it can be tested headless without a disc, a BIOS dump or any
commercial code.

| Program | What it exercises | Output |
|---|---|---|
| `hello` | PS2SDK's crt0 and start-up (SetupThread / SetupHeap, the kernel patches PS2SDK installs through SetSyscall, `InitThread`'s thread and semaphore, newlib, libcglue), 64-bit integer code, an R5900 check, 33 MMI operations through LQ / SQ, the non-IEEE FPU, all printed to the EE's SIO; returns from `main` (the Exit syscall) | `hello.elf` |

`hello`'s SIO output is `consoles/ps2/tests/hello.txt`, one value per line:

- 64-bit integer arithmetic as GCC compiles it for the R5900 (`ld` / `sd`, `dsll32`, libgcc's 64-bit divide);
- the R5900's own instructions, written out in asm: three-operand MULT, MADD, MULT1 / DIV1 / MFLO1 / MFHI1 (the second
  pipeline), PLZCW, BEQL with a skipped slot, MOVZ / MOVN;
- 128-bit MMI: each line loads two quadwords with LQ, runs one MMI instruction (PADDW, PSUBSW, PEXTLW, PPACH, PINTH,
  PCPYLD, PNOR, PABSW, PEXEW, PCPYH, PSLLH ...), stores the result with SQ, and prints its four words (high to low).
  GCC 15 keeps a `u128` in a pair of 64-bit registers, never in one 128-bit GPR, so the operands never go through C
  variables; PMFHL.LW and PMTLO / PMFLO check the 128-bit HI / LO;
- the FPU's R5900 behaviour: an overflow clamps to 0x7FFFFFFF, 0x7F800000 is 2^128 (not infinity), a denormal reads as
  zero, 1 / 3 rounds toward zero (0x3EAAAAAA, where IEEE gives 0x3EAAAAAB), MULA.S / MADD.S through ACC, MAX.S /
  MIN.S.

The expected values come from the R5900 / TX79 manuals' definitions, as the interpreter and the lifter implement them
(`consoles/ps2/runtime/ps2_alu.h`); the MMI lines were also checked against an independent Python model of the
manual. They are not checked on hardware. The FPU lines avoid the cases where the hardware's adder (limited guard
bits) is known to differ from an exact computation truncated toward zero.

## Build

```sh
tools/consoles/build_homebrew.sh ps2 [OUTDIR]     # default OUTDIR: build/homebrew -> build/homebrew/ps2/
```

The image `tools/docker/ps2-homebrew.Dockerfile` is ps2dev's official `ps2dev/ps2dev:v2.0.0` (Alpine, about 1 GB
unpacked, 281 MB download, pinned by digest) plus Alpine's `make` and `bash`; `make` runs inside it without network,
with PS2SDK's own flags and link file (`-D_EE -G0 -O2`, `ee/startup/linkfile`). Outputs (`.elf`, `SHA256SUMS`) are
never committed.

## Toolchain and licences

| Component | Licence |
|---|---|
| mips64r5900el-ps2-elf-gcc 15.2 / binutils (`ps2dev/ps2dev`, pinned by digest) | GPLv3 (tools only) |
| PS2SDK (linked: crt0, libkernel with its kernel patches, libcglue; the link file) | AFL-2.0 (`$PS2SDK/LICENSE`) |
| newlib (linked) | BSD-style licences |
| pthread-embedded (linked by libcglue's start-up) | LGPL-2.1: fine for test binaries that are built in CI and never distributed, which is all these are |
