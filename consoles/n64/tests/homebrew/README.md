# N64 homebrew corpus (XBA-149)

Test programs we write and build ourselves, so the N64 pipeline and runtime can be tested headless without a
commercial ROM. Only our own code lives here; third-party test ROMs wait for the licence review (XBA-148).

| Program | What it exercises | Output |
|---|---|---|
| [`hello`](hello/README.md) | boot (.data/.bss), VI init, software text | `hello.z64` |
| [`cpu`](cpu/README.md) | 56 VR4300 sequences: 64-bit integer ops, branch-likely, unaligned BE loads/stores, FPU | `cpu.z64` |
| [`timer_irq`](timer_irq/README.md) | VI interrupt, COUNT/COMPARE timer interrupt (libdragon timers), COUNT rate | `timer_irq.z64` |
| [`gfx`](gfx/README.md) | RDP via rdpq: fill rectangles, copy-mode and scaled texture blits, gouraud triangle | `gfx.z64` |

## Build

```sh
tools/consoles/build_homebrew.sh n64 [OUTDIR]     # default OUTDIR: build/homebrew -> build/homebrew/n64/
```

The script builds `tools/docker/n64-homebrew.Dockerfile` and runs `make` (libdragon's `n64.mk`) on a copy of this
directory inside it (no network, `-j4`). Outputs (`.z64`, `.elf`, `SHA256SUMS`) are never committed.

**ROM layout**: these are libdragon ROMs, not libultra ones. The `.z64` (big-endian, magic `80371240`) holds
libdragon's open-source IPL3 (no Nintendo boot code) followed by the program as an ELF, which IPL3 loads. The
Makefile sets `N64_ROM_ELFCOMPRESS=0` so the ELF is stored uncompressed and a parser can read the code from the ROM.
The N64 parser therefore needs a "libdragon ELF-in-ROM" path next to the classic flat `0x80000400` boot segment;
the `.elf` next to each ROM is the same program with symbols, for debugging.

## Result protocol (all programs)

Each program writes a 512-byte result block at **physical RDRAM `0x00300000`** (big-endian), defined in
[`common/xba_result.h`](common/xba_result.h), through KSEG1 (`0xA0300000`, uncached, so no data-cache line holds the
verdict back), then spins in a halt loop (a jump to itself; libdragon interrupts keep running):

| Offset | Field | Meaning |
|---|---|---|
| 0x00 | `magic` | `0x58424152` ("XBAR") once `main` started |
| 0x04 | `status` | 0 while running, `0x50415353` ("PASS") or `0x4641494C` ("FAIL") at the end |
| 0x08 | `test_id` | fourcc (`HELO`, `CPU `, `TIRQ`, `GFX `) |
| 0x0C | `detail` | 0 on pass; on fail 1 + index of the first failing check |
| 0x10 | `checksum` | program-specific (`cpu`: FNV-1a of the table) |
| 0x14 | `count` | number of table entries written |
| 0x18 | `frames` | VI interrupts counted by the program (0 if it does not count) |
| 0x1C | `stage` | progress marker (`gfx`: capture point) |
| 0x20 | `table[120]` | per-check values (32-bit words), listed in each program's README |

A headless runner boots the ROM, runs until `status` is non-zero (poll at each VI, timeout e.g. 600 frames) and
reports `status`, `detail` and the table. `cpu` and `timer_irq` end on a green screen (pass) or red screen (fail).
The block sits at 3 MiB: code, data and the libdragon heap (frame buffers included) stay below, the stack is at the
top of RDRAM.

## Validation status

All four programs report PASS in **ares** (commit `0b928930`, N64 core, paraLLEl-RDP on Mesa's lavapipe; ares HLEs
the PIF boot, so no Nintendo boot ROM is involved) (2026-11-14):

```sh
tools/consoles/build_homebrew.sh n64 && tools/consoles/run_homebrew.sh n64
```

ares has no headless mode: `run_homebrew.sh` builds `tools/docker/n64-oracle.Dockerfile` (about 5 min cold,
1.7 GB), runs ares under Xvfb with its null video driver and its GDB server on, and
`tools/consoles/oracle/gdb_poll.py` reads the block through the GDB stub while the game runs (`m` packets, the target
is never halted). ares (ISC) is an **external oracle only**: run in its own container, never copied into or linked
with our code, the image never published. Expected values come from the VR4300 user manual and were computed with
Python; the first oracle run matched them all (`cpu`: 116 table words). It caught one bug of ours: `gfx` asked for
`FILTERS_DISABLED` at 320 wide, which libdragon refuses with an assertion (fixed: `FILTERS_RESAMPLE`).

## Toolchain and licences

| Component | Version | Licence |
|---|---|---|
| Base image | `ubuntu:24.04@sha256:2198e4c6…ecaf5` | Ubuntu (tools only) |
| libdragon (linked into the ROMs, IPL3 inside every ROM, `n64tool`) | commit `e356bf3f56f7afbf7e5246329562f145965cfdfc` (trunk, 2026-09-14) | Unlicense (public domain) |
| `mips64-elf` GCC toolchain | libdragon's `gcc-toolchain-mips64-x86_64.deb`, GCC 16.2.0 / binutils 2.45 / newlib 4.4.0, sha256 `fac8e657…20a0a4d` | GPLv3 (tools only; libgcc has the runtime exception), newlib BSD-style |

libdragon publishes its toolchain only as a rolling `toolchain-continuous-prerelease` asset. The Dockerfile pins its
sha256, so when upstream replaces it the image build fails on the checksum, on purpose: update
`N64_TOOLCHAIN_SHA256` and `LIBDRAGON_COMMIT` together, or point `N64_TOOLCHAIN_URL` at our own mirror of the
pinned `.deb` (GPL binaries, mirroring is allowed). Mirroring it on our Gitea is recommended before CI depends on it.
