# PS1 homebrew corpus (XBA-149)

Test programs we write and build ourselves, so the PS1 pipeline and runtime can be tested headless without a
commercial disc or a BIOS dump. Only our own code lives here; third-party test programs wait for the licence review
(XBA-148).

| Program | What it exercises | Outputs |
|---|---|---|
| [`hello`](hello/README.md) | crt0 (.data/.bss), GPU init, debug font | `hello.exe`, `hello.bin/.cue` |
| [`cpu`](cpu/README.md) | 51 R3000A sequences: delay slots, mult/div (incl. divide by zero), LWL/LWR/SWL/SWR | `cpu.exe`, `cpu.bin/.cue` |
| [`timer_irq`](timer_irq/README.md) | VBlank callback, root counters 1 (HBlank) and 2 (sysclk/8) IRQs | `timer_irq.exe`, `.bin/.cue` |
| [`gfx`](gfx/README.md) | TILE, POLY_F3, POLY_G4, POLY_FT4 (16-bit texture), SPRT (4-bit CLUT), VRAM readback | `gfx.exe`, `gfx.bin/.cue` |
| [`overlay`](overlay/README.md) | code loaded from the disc into RAM and called, then replaced at the same address | `overlay.exe`, `overlay.bin/.cue` |

Every program is a PS-X EXE **and** a BIN/CUE disc (one MODE2/2352 data track, ISO 9660, `SYSTEM.CNF` booting the
EXE), built by `mkpsxiso`, so disc parsing (ISO 9660, SYSTEM.CNF, EXE header) is tested on every program. Disc file
names are 8.3: `HELLO.EXE`, `CPU.EXE`, `TIMERIRQ.EXE`, `GFX.EXE`, `OVERLAY.EXE` (+ `OVL_A.BIN`, `OVL_B.BIN`).
The discs carry **no licence data** (the system area is zero-filled): they are for emulators and our recompiler,
not for booting a retail console.

## Build

```sh
tools/consoles/build_homebrew.sh ps1 [OUTDIR]     # default OUTDIR: build/homebrew -> build/homebrew/ps1/
```

The script builds `tools/docker/ps1-homebrew.Dockerfile` and configures this directory as a **standalone** CMake
project with PSn00bSDK's toolchain file (it is not part of the repo's CMake tree), no network, `-j4`. Outputs
(`.exe`, `.elf`, `.bin`, `.cue`, `SHA256SUMS`) are never committed. The `.bin` images embed the build date
(mkpsxiso stamps the ISO 9660 records with the current time), so their hash changes between
builds; the EXEs, ELFs, GBA and N64 ROMs are byte-for-byte reproducible.

## Result protocol (all programs)

Each program writes a 512-byte result block at **`0x801F0000`** (KSEG0; physical `0x001F0000`, little-endian),
defined in [`common/xba_result.h`](common/xba_result.h), then spins in a halt loop (a jump to itself):

| Offset | Field | Meaning |
|---|---|---|
| 0x00 | `magic` | `0x58424152` ("XBAR") once `main` started |
| 0x04 | `status` | 0 while running, `0x50415353` ("PASS") or `0x4641494C` ("FAIL") at the end |
| 0x08 | `test_id` | fourcc (`HELO`, `CPU `, `TIRQ`, `GFX `, `OVL `) |
| 0x0C | `detail` | 0 on pass; on fail 1 + index of the first failing check |
| 0x10 | `checksum` | program-specific (`cpu`: FNV-1a of the table) |
| 0x14 | `count` | number of table entries written |
| 0x18 | `frames` | VBlanks counted by the program (0 if it does not count) |
| 0x1C | `stage` | progress marker (`gfx`: capture point) |
| 0x20 | `table[120]` | per-check values, listed in each program's README |

A headless runner boots the EXE (or the disc), runs until `status` is non-zero (poll at each VBlank, timeout e.g.
600 frames) and reports `status`, `detail` and the table. `cpu`, `timer_irq` and `overlay` end on a green screen
(pass) or red screen (fail). The block sits 64 KiB below the top of RAM: the stack starts at `0x801FFFF0` and the
programs never `malloc`. Overlays load at `0x80100000`, far above the EXEs (`0x80010000` + < 64 KiB).

## Validation status

All five programs report PASS in **PCSX-Redux** (commit `b3eb3253`, interpreter CPU) with **OpenBIOS** (its
open-source BIOS, built from source; no Sony BIOS), both as the side-loaded EXE and as the disc booted through
`SYSTEM.CNF` (2026-11-14):

```sh
tools/consoles/build_homebrew.sh ps1 && tools/consoles/run_homebrew.sh ps1
```

`run_homebrew.sh` builds `tools/docker/ps1-oracle.Dockerfile` (about 16 min cold at JOBS=4, 2.9 GB) and runs
PCSX-Redux `-no-ui`; `tools/consoles/oracle/ps1_watch.lua` reads the block at each VSync and the verdict is decoded by
`oracle/decode_block.py`. PCSX-Redux (GPL-2.0-or-later) and OpenBIOS (MIT) are **external oracles only**: run in
their own container, never copied into or linked with our code, the image never published. Expected values come
from the R3000A behaviour documented in the LSI/IDT manuals and nocash's psx-spx; the first oracle run caught one
wrong expectation (`gfx` sampled the gouraud quad on its diagonal, fixed), the rest matched as written.

## Toolchain and licences

| Component | Version | Licence |
|---|---|---|
| Base image | `ubuntu:24.04@sha256:2198e4c6…ecaf5` | Ubuntu (tools only) |
| PSn00bSDK (libpsn00b linked into the EXEs, CMake scripts, `elf2x`) | v0.24, `PSn00bSDK-0.24-Linux.zip` sha256 `5ada7c94…ded35` | MPL-2.0 |
| `mkpsxiso` / `dumpsxiso` (bundled with the SDK) | 2.03 | GPL-2.0-or-later (tools only) |
| `mipsel-none-elf` GCC (bundled with the SDK) | 12.3.0 | GPLv3 (tools only; libgcc has the runtime exception) |

No BIOS is used or needed: the runtime HLEs the BIOS calls libpsn00b makes (A/B/C tables: interrupts, events,
`FlushCache`, CD).
