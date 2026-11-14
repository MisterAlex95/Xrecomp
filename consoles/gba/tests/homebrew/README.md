# GBA homebrew corpus (XBA-149)

Test programs we write and build ourselves, so the GBA pipeline and runtime can be tested headless without a
commercial ROM or a BIOS dump. Only our own code lives here; third-party test ROMs wait for the licence review
(XBA-148).

| Program | What it exercises | Output |
|---|---|---|
| [`hello`](hello/README.md) | crt0 (.data copy, .bss clear), tile text in mode 0 | `hello.gba` |
| [`cpu`](cpu/README.md) | 45 ARM + 25 Thumb sequences, ARM<->Thumb interworking, BIOS Div/Sqrt, libgcc | `cpu.gba` |
| [`timer_irq`](timer_irq/README.md) | VBlank / HBlank / timer IRQs, cascade timer, `VBlankIntrWait` | `timer_irq.gba` |
| [`gfx`](gfx/README.md) | mode 0 tiles + sprites, then mode 3 bitmap; two capture points | `gfx.gba` |
| [`iwram_copy`](iwram_copy/README.md) | code run from IWRAM/EWRAM: crt0-copied, copied at run time, patched in place | `iwram_copy.gba` |

## Build

```sh
tools/consoles/build_homebrew.sh gba [OUTDIR]     # default OUTDIR: build/homebrew -> build/homebrew/gba/
```

The script builds the toolchain image `tools/docker/gba-homebrew.Dockerfile` and runs `make` on this directory inside
it (no network, `-j4`). Outputs (`.gba`, `.elf`, `SHA256SUMS`) are never committed. All C is Thumb except the
`.s` files (which pick their state) and any `*.arm.c`.

## Result protocol (all programs)

Each program writes a 512-byte result block at **EWRAM `0x0203FE00`** (little-endian), defined in
[`common/xba_result.h`](common/xba_result.h), then spins in a halt loop (a branch to itself):

| Offset | Field | Meaning |
|---|---|---|
| 0x00 | `magic` | `0x58424152` ("XBAR") once `main` started |
| 0x04 | `status` | 0 while running, `0x50415353` ("PASS") or `0x4641494C` ("FAIL") at the end |
| 0x08 | `test_id` | fourcc of the program (`HELO`, `CPU `, `TIRQ`, `GFX `, `IWRM`) |
| 0x0C | `detail` | 0 on pass; on fail 1 + index of the first failing check |
| 0x10 | `checksum` | program-specific (`cpu`: FNV-1a of the table; `gfx`: FNV-1a of the mode 3 frame) |
| 0x14 | `count` | number of table entries written |
| 0x18 | `frames` | VBlanks counted by the program (0 if it does not count) |
| 0x1C | `stage` | progress marker (`gfx`: capture points) |
| 0x20 | `table[120]` | per-check values, listed in each program's README |

A headless runner loads the `.gba`, runs until `status` is non-zero (poll at each VBlank, give up after a timeout,
e.g. 600 frames), and reports `status`, `detail` and the table. The screen also shows the verdict where a program
has no picture of its own (green backdrop = pass, red = fail).

## Validation status

All five programs report PASS in mGBA 0.10.1 (Debian bookworm `mgba-sdl`, its built-in HLE BIOS, no BIOS file),
read through mGBA's GDB stub with a watchpoint on `status` (2026-11-13). Expected values are therefore checked
against an accurate emulator, not only against the ARM ARM.

## Toolchain and licences

| Component | Version | Licence |
|---|---|---|
| Base image `devkitpro/devkitarm` | tag `20260610`, digest `sha256:116afba8…0bbf5` (pinned in the Dockerfile) | devkitPro's official image, pulled from Docker Hub, never redistributed by us |
| devkitARM GCC / binutils | GCC 16.1.0 | GPLv3 (tools only; our ROMs are not covered) |
| newlib (libc linked into the ROMs) | devkitARM's | BSD-style licences |
| devkitARM `gba_crt0` / `gba_cart.ld`, `gbafix` | devkitARM's | devkitPro tools licence (`/opt/devkitpro/licenses/gba-tools`) |
| libtonc (linked) | devkitPro's `libtonc` | MIT |
| libgba (in the image, not linked) | devkitPro's | LGPL with static-linking exception |

No BIOS dump is used or needed: the core's built-in BIOS handles the SWIs these programs call (`Div` 0x06, `Sqrt`
0x08, `VBlankIntrWait` 0x05), and they also pass through the open-source Cult-of-GBA BIOS (`gba_run --bios`,
`consoles/gba/tests`: `gba_hb_*` and `gba_hb_lle_*`).
