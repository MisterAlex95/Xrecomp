# Other consoles

XRecomp is adding **Game Boy Advance**, **PlayStation** and **Nintendo 64**, statically recompiled the same way as
Xbox discs. This is early work: the layout, build options and test corpus are in place, the runtimes are empty, and no
game for these consoles runs yet. Goal, order, rules (legal, Linux arm64 first), layout, pipeline and milestones are in the Plane spec:
`plane page show -p XBA "Spec · Other consoles"` (epics C0, G, P, N).

| Directory | Console | CPU |
|---|---|---|
| `gba/` | Game Boy Advance | ARM7TDMI (ARM + Thumb) |
| `ps1/` | PlayStation | MIPS R3000A |
| `n64/` | Nintendo 64 | MIPS R4300i (big-endian, 64-bit) |

Each one has:

- `runtime/`: C, CMake target `<name>_runtime`. It links only the host layer `xbr_host` (`src/host`,
  `src/platform/HOST_LAYER.md`) and never the Xbox libraries. N64 is empty for now (one version function);
  GBA and PS1 have their cores, see below.
- `tools/`: the Python pipeline (parse → disasm → function discovery → lift → C), package `<name>_tools`. Its
  tests live in the package and run with the tools suite: `python3 -m pytest tools/ consoles/`.
- `tests/`: a self-contained CMake test project. `tools/run_c_tests.sh` runs it as `console_<name>` (exit 77 =
  skipped), and the root build's ctest runs it when the console is ON.

**Title packs (XBA-147).** `pack.json` has an optional `console` field (`xbox` when absent, so every Xbox pack made
before keeps loading, still format 1) and, for `gba` / `ps1` / `n64`, a `title_key` in place of `title_id`. The key is
read from the game's content (`recomp_pack_rom_identify` in `src/platform/recomp_pack.c`, `Console.identify` in the
app): GBA game code + CRC32 of the ROM (`AXVE-1A2B3C4D`), PS1 serial from `SYSTEM.CNF` (`SLUS-00594`, or
`PSX-<CRC32 of the volume descriptor>` without a serial-shaped boot file), N64 game code + header CRC1
(`NSME-635A2BFF`, any byte order). Such a pack lives in `packs/<console>-<key>/`. The app shell's loader
(`android/app/src/main/cpp/pack_loader.c`, `xba_pack_load_rom`) checks it and calls `<console>_runtime_start`, a
stub that logs `<console> runtime ready` for now (libmain.so links the three runtimes); the GBA's plays the game, and
needs no pack (`PACK_NONE`, XBA-157); a GBA pack that Prepare built (`python3 -m tools.prepare game.gba`, XBA-158:
docs/android/prepare.md « GBA packs ») makes it run natively, checked and loaded by `runtime/gba_pack.c`
(`PACK_LOADED` / `PACK_STALE`, the game always starts), and the addresses the interpreter had to run are written at
exit for the next Prepare (`gba_runtime_opts.discovered_path`). Import filter (`Console.java`):
`.gba`; `.bin`, `.cue`, `.chd`; `.z64`, `.n64`, `.v64`; and `.iso`, which both an Xbox and a PS1 disc use, so its
content decides (Xbox volume magic `MICROSOFT*XBOX*MEDIA` first, then an ISO 9660 volume with system ID
`PLAYSTATION`). CHD is accepted by name but not read yet (no title key).

CMake options `XBR_CONSOLE_GBA`, `XBR_CONSOLE_PS1` and `XBR_CONSOLE_N64` are ON by default on Linux and macOS and OFF
on Android and Windows. A console is turned on for Android once it is ready: the GBA (XBA-157) and the PS1 (XBA-168)
are.

**GBA core (XBA-152, XBA-153).** Interpreter first (decision of 2026-11-14, XBA-142); the static lifter (below) is an
acceleration layer. `consoles/gba/runtime/gba_core.h`: an ARM7TDMI interpreter (ARM + Thumb), the memory map, LCD
timing, timers, DMA, interrupts, keypad, and either a BIOS image (LLE: the open-source Cult-of-GBA BIOS built by
`tools/consoles/build_gba_bios.sh`) or a small built-in BIOS of our own (IRQ vector and IntrWait in ARM code, the other
SWIs in C). Direct boot by default. The PPU (`gba_ppu.h`) is a pure line renderer the core calls at the end of each
visible line. The APU (`gba_audio.c`, XBA-155: PSG squares / wave / noise, Direct Sound FIFOs fed by DMA1/2 on
timer overflows, SOUNDBIAS) mixes at 131 kHz and hands out 48 kHz stereo through `gba_audio_pull`. Saves
(`gba_save.c`, XBA-156): the kind comes from the ROM's library ID string (SRAM 32 KB, flash 64 / 128 KB, EEPROM
512 B / 8 KB; none: SRAM from the first write at 0x0E000000), read and written back through `gba_save_data` /
`gba_save_load` / `gba_save_take_dirty`. CMake target `gba_core` is the machine alone (C11 + libm, no SDL),
`gba_runtime` adds the host layer; `gba_run ROM [--result] [--bios PATH] [--ppm OUT] [--wav OUT] [--save PATH]` runs
a ROM headless. Tests: `consoles/gba/tests/CMakeLists.txt` (unit tests, our homebrew HLE and LLE, jsmolka/gba-tests
CPU / memory / BIOS / save suites fetched by `tools/consoles/fetch_gba_tests.sh`).

**Playing a GBA game (XBA-157).** `gba_runtime_run` (`gba_runtime.h`) plays a ROM on the host layer: `host_window`
(RGB555 → RGBA texture, letterboxed, aspect or integer scaling, nearest or linear), every `host_pad` controller in
the Xbox layout (A → A, B → B, Y → A, X → B, shoulders and triggers → L / R, Start, Back → Select, D-pad and left
stick → D-pad), `host_audio` fed from `gba_audio_pull`, pacing at 59.73 Hz with absolute deadlines nudged by the
audio queue, saves in `<save dir>/gba-<KEY>/game.sav` (read at start, written a second after the game's last write,
at pause and at exit), pause and resume with `recomp_lifecycle`. The ROM is a path or `fd:N`. The Android app calls
it through `gba_runtime_start` with no pack (the GBA is ON for Android: [app-shell.md](../docs/android/app-shell.md)
« Game Boy Advance games »). On a desktop:

```sh
cmake -S consoles/gba/tests -B build/gba && cmake --build build/gba     # or the root build (XBR_CONSOLE_GBA ON)
build/gba/gba_runtime/gba_play game.gba          # keyboard: arrows, X = A, Z = B, A = L, S = R, Enter, Backspace
build/gba/gba_runtime/gba_play game.gba --scale integer --save-dir /tmp/saves
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/gba/gba_runtime/gba_play game.gba --frames 600 --screenshot last.ppm
xvfb-run -a build/gba/gba_runtime/gba_play gfx.gba --frames 240 --capture 200 present.ppm   # the real present path
```

**GBA static recompilation (XBA-150, XBA-151, XBA-152).** A GBA game's ARM / Thumb code is lifted to C, compiled for
the host into a title pack (`libtitle.so`), and runs natively on the same core: graphics, sound, timers, DMA, IRQs
and saves are the interpreter's code either way. Universal, no per-title configuration; whatever the static pass
misses runs in the interpreter, which records it for the next build (the discovery loop). Contract, frozen between
`gba_tools`, Prepare, the loader and the core (change together and bump `GBA_RECOMP_ABI`):

- `python3 -m gba_tools.recomp ROM OUTDIR [--discovered FILE]` (`consoles/gba/tools`, pure Python; Capstone only
  for disassembly comments) writes `OUTDIR/gba_rc_NNN.c` + `gba_rc_table.c`, nothing else; each compiles with
  `-I consoles/gba/runtime` alone. `--discovered`: one hex address per line, bit 0 = Thumb, `#` comments.
- The library exports `gba_recomp_title_table` (`gba_recomp.h`): sorted entries (address, Thumb flag, function),
  the ROM's SHA-256. The lifted code's ABI is `gba_recomp_rt.h`: the first members of `gba_machine` (`gba_rc_head`:
  registers, cycle clock, next event, wait-state tables, IRQ line, EWRAM / IWRAM pointers; static asserts in
  `gba_internal.h`) and the bus functions `gba_read8/16/32`, `gba_write8/16/32`, which the loading executable exports
  (`gba_core_exported()`: gba_play, gba_run; `--whole-archive gba_core` in the app's libmain.so).
- `gba_attach_recomp` / `gba_recomp_missed` (`gba_recomp.c`): with a table attached the CPU loop calls lifted code
  whenever the PC is in the cartridge and an entry covers it, the interpreter otherwise; the cartridge addresses
  where the interpreter had to start are the missed set. Loading a pack: `gba_pack.h` (pack.json checks, dlopen).

Lifted code is cycle-exact with the interpreter (same costs, same order, `now < next_event` checked before every
instruction, the IRQ line after every store), so a recompiled run is the interpreter's run, frame for frame.
`gba_run ROM --pack LIB --lockstep` checks it: an interpreter-only twin runs alongside, the whole state compared
after every frame; `GBA_RC_VERIFY=1` replays every lifted call in the interpreter (debugging). On a desktop:

```sh
tools/consoles/gba_recomp_build.py game.gba build/rc/game --loop 4 --gba-run build/gba/gba_runtime/gba_run --frames 1800
build/gba/gba_runtime/gba_play game.gba --pack build/rc/game/libtitle.so --missed build/rc/game/discovered.txt
build/gba/gba_runtime/gba_run game.gba --pack build/rc/game/libtitle.so --lockstep --stats --frames 600
```

Tests (`consoles/gba/tests`): `gba_rc_*` lift every homebrew and jsmolka ROM, run the discovery loop and play them in
lockstep with the interpreter (HLE and LLE BIOS); `gba_rc_fuzz` does the same with random ARM / Thumb programs under
timer IRQs (`recomp_fuzz.py`); pytest `consoles/gba/tools/gba_tools/test_recomp.py` (discovery on hand-encoded code,
the CLI, Prepare end to end). Design notes: [gba/tools/README.md](gba/tools/README.md).

Saves default to `$XDG_DATA_HOME/xrecomp/save` (else `~/.local/share/xrecomp/save`); `--bios PATH` or
`RECOMP_GBA_BIOS` uses a BIOS image of your own; every option is listed at the top of `runtime/tools/gba_play.c`. The
homebrew `pad.gba` (`consoles/gba/tests/homebrew/pad`) shows the keys held and keeps a counter in SRAM: the controls
and saves check on a real host.

**PS1 core (P-4 XBA-162, P-5 XBA-163, P-6 XBA-164, pads / cards of P-9 XBA-167).** Interpreter first, the GBA's
shape. `consoles/ps1/runtime/ps1_core.h`: an R3000A interpreter (`ps1_cpu.c`: every MIPS I instruction, branch and
load delay slots with the cancellation rules, LWL / LWR forwarding, all exceptions, COP0 with SR.IsC cache isolation,
RFE, interrupt entry; a fixed cost per instruction by fetch region, no cache model), the GTE (`ps1_gte.c`, every command,
44-bit MAC checks, FLAG, the UNR divide, register quirks), the memory map with its mirrors (`ps1_bus.c`), the interrupt
controller, DMA (7 channels, DPCR / DICR, sync modes 0 / 1 / 2, OTC; bursts stall the CPU, block and linked-list
transfers complete after their length), the three root counters with every clock source and sync mode, and the
scanline / VBlank scheduler driven by `ps1_gpu_get_timing` (`ps1_io.c`), SIO0 with the digital pad, the DualShock in
analog mode and two 128 KB memory cards (`ps1_sio.c`; `ps1_card_data` / `ps1_card_take_dirty` / `ps1_card_load` like
the GBA saves). The other chips are reached through `ps1_devices.h` only: target `ps1_core` links `ps1_gpu`, `ps1_cd`,
`ps1_spu` and `ps1_mdec`, each the real module or its placeholder `ps1_<dev>_stub.c` when the source is absent.

BIOS: **OpenBIOS** (MIT, PCSX-Redux's nugget tree), built from source by `tools/consoles/build_ps1_bios.sh` at the
commit the oracle pins, byte-identical to the oracle's (sha256 `68942dc2…`); never a Sony BIOS. Boot is LLE from the
reset vector; with a PS-X EXE the BIOS runs its init and, where it would enter its shell (0x80030000), the EXE is loaded
(header PC / GP / SP+FP / text / BSS: fast boot); with a disc and no EXE the BIOS boots `SYSTEM.CNF` itself. An HLE BIOS
(P-5 HLE) needs, for games and libpsn00b / Psy-Q: the exception handler and the IRQ / event / thread kernel (B(08h..20h)
events, B(0Eh..13h) threads, SysEnqIntRP / SysDeqIntRP, ChangeClearPad / ChangeClearRCnt), A(00h..3Fh) file and string
calls with the `cdrom:` / `bu:` devices (CD and memory card drivers over SIO0), InitPad / StartPad, FlushCache, the
kernel memory layout at 0x00000000..0x0000FFFF that games peek, and the A0 / B0 / C0 table dispatch; until then the
image is required.

`ps1_run [--bios PATH] [--exe X.exe | --disc X.cue] [--frames N] [--result] [--ppm OUT] [--wav OUT] [--tty]
[--until-tty TEXT]` runs a program headless (`--result`: our homebrew's result block at 0x801F0000; `--tty`: the
console text, BIOS putchar / puts). Tests (`consoles/ps1/tests`): `ps1_cpu` and `ps1_gte` unit tests, our homebrew
(`ps1_hb_*`, the same results as PCSX-Redux, checksums included), JaCzekanski/ps1-tests (MIT, release fetched by
`tools/consoles/fetch_ps1_tests.sh`: cpu/cop, cpu/code-in-io, gte/test-all 1150 / 1150, dma/otc-test, dma/dpcr, timers).
On a desktop:

```sh
tools/consoles/build_ps1_bios.sh && tools/consoles/build_homebrew.sh ps1 && tools/consoles/fetch_ps1_tests.sh
cmake -S consoles/ps1/tests -B build/ps1 -DPS1_CORE_ONLY=ON && cmake --build build/ps1 && (cd build/ps1 && ctest)
build/ps1/ps1_runtime/ps1_run --exe build/homebrew/ps1/gfx.exe --result --ppm gfx.ppm
```

**PS1 static recompilation (P-2 XBA-160, P-3 XBA-161, P-4 XBA-162).** The GBA's model on RAM-loaded code: a PS1
program's MIPS code is lifted to C, compiled for the host into a title pack (`libtitle.so`), and runs natively on the
same core; the BIOS, the devices, DMA, timers and interrupts are the interpreter's code either way. Universal, no
per-title configuration; what the static pass misses runs in the interpreter, which records it, bytes included, for
the next build (the discovery loop). Contract (`ps1_recomp.h`, frozen between `ps1_tools`, Prepare, the loader and
the core; change together and bump `PS1_RECOMP_ABI`):

- `python3 -m ps1_tools.recomp INPUT OUTDIR [--discovered FILE]` (`consoles/ps1/tools`, pure Python, our own MIPS
  decoder) writes `OUTDIR/ps1_rc_NNN.c` + `ps1_rc_table.c`; each compiles with `-I consoles/ps1/runtime` alone. INPUT
  is a disc image (`.cue` / `.bin` / `.iso`: the EXE `SYSTEM.CNF` boots, read from ISO 9660) or a PS-X EXE.
- The library exports `ps1_recomp_title_table`: entries (address, length, **content hash**, function), the EXE's
  SHA-256 and entry point. PS1 code runs from RAM (the EXE, then overlays read over each other), so an entry runs only
  while RAM holds its words: the core keeps a write counter per 1 KB RAM page (CPU and lifted stores, DMA, the EXE
  load), re-hashes an entry only when a page it covers changed, and picks, among entries sharing an address
  (overlays), the one that matches. The lifted code's ABI is `ps1_recomp_rt.h`: the first members of `ps1_machine`
  (`ps1_rc_head`: registers and delay-slot state, COP0, clock and next event, RAM / scratchpad / page counters;
  static asserts in `ps1_recomp.c`) and the core functions `ps1_rc_lw` / `_read` / `_write` / `_swlr` / `_gte_cmd` /
  `_gte_read` / `_gte_write`, which the loading executable exports (`ps1_core_exported()`: ps1_run).
- `ps1_attach_recomp` / `ps1_recomp_missed` / `ps1_recomp_write_missed` (`ps1_recomp.c`, called from the
  `ps1_cpu.c` run loop): main RAM above the BIOS's 64 KB, once the CPU reached the EXE's entry, runs lifted code
  when an entry matches, the interpreter otherwise; the discovered file gets `ADDR HASH HEXBYTES` per miss (a 2 KB
  window of the code as it was), so overlays are lifted on the next round from their own bytes. An address seen
  with 8 contents is code generated at run time: left to the interpreter.

Lifted code is cycle-exact with the interpreter (same fetch and load costs at the same points, the scheduler test
before every instruction, the IRQ line after IO accesses; load delay slots committed early only when the next
instruction cannot tell, else left in flight to the interpreter; branch delay slots inline; exceptions, COP0 and a
branch in a delay slot handed to the interpreter; calls, returns and indirect jumps through the dispatcher, so the C
stack stays bounded). `ps1_run INPUT --pack LIB --lockstep` checks it: an interpreter-only twin alongside, the whole
state (CPU, cycles, scheduler, IRQ / DMA / timer registers, GTE, RAM, scratchpad, VRAM) compared after every frame
(`--lockstep-cycles N`: every N cycles). On a desktop:

```sh
tools/consoles/ps1_recomp_build.py game.cue build/rc/game --loop 4 --ps1-run build/ps1/ps1_runtime/ps1_run \
    --frames 3000 --run-args "--bios build/ps1-bios/openbios.bin"
build/ps1/ps1_runtime/ps1_run game.cue --pack build/rc/game/libtitle.so --lockstep --stats --frames 600
```

Tests (`consoles/ps1/tests`, ctest `ps1_rc_*`): every homebrew (the overlay one from its EXE and from its disc:
both overlays are recorded in round 1, lifted in round 2, and B replacing A at 0x80100000 never runs A's native
code) and ps1-tests (cpu/cop, cpu/code-in-io, gte/test-all, dma/otc-test, dma/dpcr, timers) through the discovery
loop then in lockstep; `ps1_rc_fuzz`: random MIPS code under a timer IRQ every 37-2000 cycles; pytest
`consoles/ps1/tools/ps1_tools/test_recomp.py`. All converge in 2-3 rounds at 99.9 % native instructions, except
gte/test-all (81.7 %: it writes each GTE command into a stub before running it). Lifted C costs about 1.3-2 s of
compiler per 1000 instructions at -O1 on a desktop x86 core for compiled homebrew, up to ~4 s for dense code
(100 k random instructions of the fuzzer); function bodies are cut at 1000 instructions because compile time grows
faster than linearly with them (`--sparse-labels`: ~10 % less). Design notes:
[ps1/tools/README.md](ps1/tools/README.md).

**PS1 GPU (P-7, XBA-165).** `consoles/ps1/runtime/ps1_gpu.c`, the GPU half of `ps1_devices.h` (CMake target
`ps1_gpu`, plain C11, no host dependency): GP0 / GP1, GPUSTAT / GPUREAD, CPU / VRAM / DMA transfers and a software
rasterizer with the hardware's rules (top-left fill, 1023 x 511 culling, quads as (v0 v1 v2) + (v1 v2 v3), 4 / 8 / 15
bpp textures with CLUT, page and window, the four semi-transparency modes, dithering, mask bit), span loops specialised
per primitive kind; `ps1_gpu_display` crops the display ranges and outputs 15- or 24-bit as XRGB. Commands run when
their last word arrives, so the GPU is never busy. Tests: `consoles/ps1/tests/test_gpu.c` (`PS1_CORE_ONLY=ON` builds
them without SDL2); `ps1_gpu_bench` is the micro benchmark. The gfx.exe homebrew frame matches PCSX-Redux pixel for
pixel.

**PS1 disc, CD-ROM, SPU, MDEC (P-1 XBA-159, P-8 XBA-166, P-9 XBA-167).** The other devices of `ps1_devices.h`, plain
C11 without host dependency (CMake targets `ps1_cd`, `ps1_spu`, `ps1_mdec`). `ps1_disc.c` reads raw sectors and
tracks only (no ISO 9660): `.cue` (several `.bin` files, INDEX 00 / 01, PREGAP, relative paths, names matched ignoring
case), a single `.bin`, `.iso` (2048-byte sectors in a synthesized Mode 2 Form 1 sector), `.chd` through the vendored
libchdr (`third_party/libchdr`, v0.3.0, option `XBR_PS1_CHD`, ON by default), and `fd:N` for single-file images.
`ps1_cdrom.c` is the controller of psx-spx: banked registers, parameter / response / data FIFOs, INT1..INT5 queued
until acknowledged, first / second response times from psx-spx's measurements, seeks approximated by distance, every
command up to ReadTOC (Test 20h answers vC2), ReadN / ReadS with 800h / 924h sectors and DMA3, XA-ADPCM (filter,
37800 / 18900 Hz, psx-spx zigzag interpolation to 44.1 kHz) and CD-DA with the volume matrix into a 44.1 kHz stream. GetID answers a data
disc as licensed (`SCEx` from the licence text at LBA 4, `SCEA` without one, as a modchip would), so a BIOS boots our
homebrew. `ps1_spu.c`: 24 ADPCM voices with Gaussian interpolation, exact ADSR and sweep envelopes, noise, pitch
modulation, ENDX, reverb (psx-spx formula with its FIR), capture buffers, the SPU IRQ, FIFO and DMA4 transfers, CD
input. `ps1_mdec.c`: RLE, IDCT with the scale table, 4 / 8 / 15 / 24-bit output, DMA0 / DMA1. Tests:
`consoles/ps1/tests/test_{disc,cdrom,spu,mdec}.c` on images and register streams they generate (a compressed CHD
from `chdman` too when it is installed). The limits are listed at the top of each source.

**Playing a PS1 game (XBA-168).** `ps1_runtime_run` (`consoles/ps1/runtime/ps1_runtime.h`) plays a disc or an EXE
on the host layer, the GBA's way: a disc (`.cue` with its tracks, `.bin`, `.iso`, `.chd`, or `fd:N` for a
single-file image) boots through OpenBIOS from `SYSTEM.CNF`, an EXE (recognised by its `PS-X EXE` magic) is
fast-booted. The picture is the GPU's display area (256 / 320 / 368 / 512 / 640 x 240 / 480, 15- or 24-bit) shown at
4:3 whatever its width (`host_window_set_aspect`), letterboxed, linear by default (`RECOMP_PS1_FILTER=nearest`,
`RECOMP_PS1_SCALE=integer`: whole multiples of the height). Pads, Xbox layout: A → Cross, B → Circle, X → Square,
Y → Triangle, LB / RB → L1 / R1, LT / RT → L2 / R2, Start, Back → Select, stick clicks → L3 / R3, D-pad; port 1 is a
digital pad (the left stick drives the D-pad too) or, with `RECOMP_PS1_PAD=analog` or Back + L3 + R3 while playing, a
DualShock in analog mode (both sticks); every host pad drives port 1, or with `RECOMP_PS1_PLAYERS=2` host pads 1 and
2 drive ports 1 and 2. Sound: `ps1_audio_pull` (44.1 kHz) into `host_audio`, which SDL converts to the device's rate.
Pacing: the frame's length is the CPU cycles the core ran to the next VBlank (NTSC 59.94 / 59.29, PAL 50 Hz, as the
GPU's mode says), absolute deadlines nudged ±2 % by the audio queue. Memory cards: `<save dir>/ps1-<KEY>/card1.mcr`
and `card2.mcr`, 128 KB raw images as other emulators write them (.mcr / .mcd), read before the first frame, written
a second after the game's last write to the card (five at most after its first), at pause and at exit, only when
changed. BIOS: `--bios`, `RECOMP_PS1_BIOS`, the app's asset `ps1-bios/openbios.bin`, else `build/ps1-bios/openbios.bin`
from the working directory or an ancestor of the program. A title pack (`--pack LIB`, XBA-168: `python3 -m
tools.prepare game.cue` builds one, [prepare.md « PS1 packs »](../docs/android/prepare.md#ps1-packs-xba-168)) runs the
lifted code when its `pack.json` and table say it is this disc's (the SHA-256 of its boot EXE) and the game plays
interpreted otherwise; `--missed FILE` keeps what it did not cover for the next Prepare (`consoles/ps1/runtime/ps1_pack.h`). The Android app calls it through `ps1_runtime_start` ([app-shell.md](../docs/android/app-shell.md)
« PlayStation games »); the APK carries OpenBIOS (the `ps1-bios` stage of `tools/docker/android.Dockerfile`, sha256
checked; `LICENSES/MIT-OpenBIOS.txt`). On a desktop:

```sh
cmake -S consoles/ps1/tests -B build/ps1 && cmake --build build/ps1          # or the root build (XBR_CONSOLE_PS1 ON)
build/ps1/ps1_runtime/ps1_play game.cue      # keyboard: arrows, X = Cross, C = Circle, Z = Square, S = Triangle,
                                             # Q / E = L1 / R1, 1 / 3 = L2 / R2, Enter = Start, Backspace = Select
build/ps1/ps1_runtime/ps1_play build/homebrew/ps1/gfx.exe --scale integer --save-dir /tmp/saves
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/ps1/ps1_runtime/ps1_play gfx.cue --frames 600 --screenshot last.ppm
xvfb-run -a build/ps1/ps1_runtime/ps1_play gfx.exe --frames 240 --capture 200 present.ppm   # the real present path
```

The title key comes from the disc as the app reads it (`recomp_pack_rom_identify`: `SLUS-00594`, or `PSX-<CRC32>`);
an EXE gets `EXE-<CRC32>`. Every option is listed at the top of `runtime/tools/ps1_play.c`. Test: `ps1_runtime`
(`consoles/ps1/tests/test_main.c`: the helpers, then gfx.exe fast-booted, through `fd:N`, a memory card read back and
left alone, gfx.cue booted by the BIOS to its 320 x 240 picture, the errors).

**Legal**: no ROM, disc image, BIOS or game data in this repository, its CI or its artifacts. Tests use homebrew we
build ourselves (devkitARM / libtonc, PSn00bSDK, libdragon) and dumps Alex made from his own media, kept outside every
repository.
