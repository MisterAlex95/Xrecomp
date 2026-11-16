# NDS homebrew corpus (XBA-223)

Our own test programs for the DS spike, so it can be tested headless without a commercial cartridge, a BIOS or a
firmware dump.

| Program | What it exercises | Output |
|---|---|---|
| `hello` | calico's boot (MOD9 stub, load list, PXI handshake with the ARM7, threads), libnds's console on engine A (top screen), an ARMv5TE check, VBlank IRQs | `hello.nds` |

`hello`'s top screen after 120 frames is `consoles/nds/tests/hello_top.png`: `ARMv5TE: ok` means each of CLZ, QADD /
QSUB / QDADD / QDSUB (and the Q flag), SMULBB / SMULTT / SMULBT / SMLABB / SMULWB / SMLAWT / SMLALBB, STRD / LDRD and
an ARM to Thumb call gave the value the ARM ARM gives; otherwise `ARMv5TE: FAIL` and a mask of the failed checks
(bit n = check n in `hello/main.c`).

## Build

```sh
tools/consoles/build_homebrew.sh nds [OUTDIR]     # default OUTDIR: build/homebrew -> build/homebrew/nds/
```

The image `tools/docker/nds-homebrew.Dockerfile` is the GBA corpus's devkitARM image (same tag and digest); `make`
runs inside it without network. Outputs (`.nds`, the ARM9 `.elf`, `SHA256SUMS`) are never committed.

## Toolchain and licences

| Component | Licence |
|---|---|
| devkitARM GCC / binutils (`devkitpro/devkitarm:20260610`, pinned by digest) | GPLv3 (tools only) |
| newlib (linked) | BSD-style licences |
| libnds 2.0.2 (linked) | zlib (`/opt/devkitpro/libnds/libnds_license.txt`) |
| calico 1.2.0 (linked into the ARM9; its prebuilt `ds7_maine.elf` is the ARM7 binary) | as its package declares it (`dkp-pacman -Qi calico`: Zope) |
| ndstool | GPLv3 (a tool only) |

The expected picture holds libnds's default console font (zlib).
