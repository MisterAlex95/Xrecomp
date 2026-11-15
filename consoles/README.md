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
  `src/platform/HOST_LAYER.md`) and never the Xbox libraries. GBA, PS1 and N64 have their cores and play games on
  the host layer, see below.
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
(`android/app/src/main/cpp/pack_loader.c`, `xba_pack_load_rom`) checks it and calls `<console>_runtime_start`
(libmain.so links the three runtimes, which all play games: PS1 XBA-168 and N64 XBA-174 below); the GBA's plays the game, and
needs no pack (`PACK_NONE`, XBA-157); a GBA pack that Prepare built (`python3 -m tools.prepare game.gba`, XBA-158:
docs/android/prepare.md « GBA packs ») makes it run natively, checked and loaded by `runtime/gba_pack.c`
(`PACK_LOADED` / `PACK_STALE`, the game always starts), and the addresses the interpreter had to run are written at
exit for the next Prepare (`gba_runtime_opts.discovered_path`). Import filter (`Console.java`):
`.gba`; `.bin`, `.cue`, `.chd`; `.z64`, `.n64`, `.v64`; and `.iso`, which both an Xbox and a PS1 disc use, so its
content decides (Xbox volume magic `MICROSOFT*XBOX*MEDIA` first, then an ISO 9660 volume with system ID
`PLAYSTATION`). CHD is accepted by name but not read yet (no title key).

CMake options `XBR_CONSOLE_GBA`, `XBR_CONSOLE_PS1` and `XBR_CONSOLE_N64` are ON by default on Linux and macOS and OFF
on Android and Windows. A console is turned on for Android once it is ready: the GBA (XBA-157), the PS1 (XBA-168) and
the N64 (XBA-174) are.

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
(RGB555 → RGBA texture, letterboxed, aspect, integer or stretch scaling, nearest by default or linear: the display
options shared by every console, [environment-variables.md](../docs/runtime/environment-variables.md#consoles-gba-ps1-n64)), every `host_pad` controller in
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
  for disassembly comments) writes `OUTDIR/gba_rc_*.c` + `gba_rc_table.c`, nothing else; each compiles with
  `-I consoles/gba/runtime` alone. `--discovered`: one hex address per line, bit 0 = Thumb, `#` comments.
- The library exports `gba_recomp_title_table` (`gba_recomp.h`): sorted entries (address, Thumb flag, function),
  the ROM's SHA-256. The lifted code's ABI is `gba_recomp_rt.h`: the first members of `gba_machine` (`gba_rc_head`:
  registers, cycle clock, next event, wait-state tables, IRQ line, EWRAM / IWRAM pointers; static asserts in
  `gba_internal.h`) and the bus functions `gba_read8/16/32`, `gba_write8/16/32`, which the loading executable exports
  (`gba_core_exported()`: gba_play, gba_run; `--whole-archive gba_core` in the app's libmain.so).
- `gba_attach_recomp` / `gba_recomp_missed` (`gba_recomp.c`): with a table attached the CPU loop calls lifted code
  whenever the PC is in the cartridge and an entry covers it, the interpreter otherwise; the cartridge addresses
  where the interpreter had to start are the missed set. Loading a pack: `gba_pack.h` (pack.json checks, dlopen).

Lifted code is cycle-exact with the interpreter (same costs, same order, it stops before the same instruction when a
scheduler event or an IRQ is due: one test per block segment, and the interpreter runs the rest of a block an event
falls in), so a recompiled run is the interpreter's run, frame for frame.
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

The ROM tests skip (exit 77) when their ROMs are not built, which is the case in the linux CI job (no Docker inside
it, so no corpus). The homebrew corpus runs in the **homebrew job** instead (XBA-180), after the corpus build, through
`tools/consoles/run_gba_packs.sh` in the linux job's deps image: ctest `gba_hb_*` and `gba_rc_hb_*`, then, with the
inputs from `tools/consoles/gba_test_inputs.sh` (XBA-193: the Cult-of-GBA BIOS built from source and jsmolka/gba-tests,
both MIT, never committed or uploaded, cached in the host's Docker as the data image `xba-gba-inputs:<hash of the
pins>`), `gba_hb_lle_*`, `gba_jsmolka_*` and their `gba_rc_*` (30 tests, about 11 s; one that skips fails the job), then `tools/consoles/gba_corpus_packs.py`, the user's chain on each program:
`python3 -m tools.prepare <prog>.gba` (a desktop pack), `gba_run --pack libtitle.so --lockstep --result` until the
verdict, and a screenshot 30 frames later (`--ppm`, kept as PNG in the run's artefact `gba-corpus-screens`). A program
fails on a failing result block, a lockstep difference, or a screenshot whose sha256 is not the one in
`consoles/gba/tests/corpus_screens.sha256`. The GitHub mirror runs the same (`.github/workflows/linux.yml`,
gba-corpus). Locally, and to record new screenshots after an intended change (check the PNGs first):

```sh
tools/consoles/build_homebrew.sh gba build/homebrew && tools/consoles/gba_test_inputs.sh build/gba-inputs
tools/consoles/run_gba_packs.sh build/homebrew build/gba-packs build/gba-inputs
tools/consoles/gba_corpus_packs.py build/homebrew/gba build/gba-packs --gba-run build/gba/gba_runtime/gba_run --update
```

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
scanline / VBlank scheduler driven by `ps1_gpu_get_timing` (`ps1_io.c`), SIO0 with the digital pad, the DualShock
(digital or analog mode; config mode: 0x43 enter / exit, 0x44 mode + lock, 0x45 status, 0x46 / 0x47 / 0x4C tables,
0x4D rumble mapping, whose motors `ps1_pad_rumble` reports; `ps1_pad_analog_button`) and two 128 KB memory cards (`ps1_sio.c`; `ps1_card_data` / `ps1_card_take_dirty` / `ps1_card_load` like
the GBA saves). The other chips are reached through `ps1_devices.h` only: target `ps1_core` links `ps1_gpu`, `ps1_cd`,
`ps1_spu` and `ps1_mdec`, each the real module or its placeholder `ps1_<dev>_stub.c` when the source is absent.

BIOS: **OpenBIOS** (MIT, PCSX-Redux's nugget tree), built from source by `tools/consoles/build_ps1_bios.sh` at the
commit the oracle pins, byte-identical to the oracle's (sha256 `68942dc2…`); never a Sony BIOS. Boot is LLE from the
reset vector; with a PS-X EXE the BIOS runs its init and, where it would enter its shell (0x80030000), the EXE is loaded
(header PC / GP / SP+FP / text / BSS: fast boot); with a disc and no EXE the BIOS boots `SYSTEM.CNF` itself. Or no
image at all: the **HLE BIOS** (P-5 HLE, `runtime/ps1_hle.c`; [ps1/HLE_BIOS.md](ps1/HLE_BIOS.md): how it works and the
190 implemented calls, each with a check). `ps1_create(NULL, ...)`, `ps1_run --bios hle`, `RECOMP_PS1_BIOS=hle`, and
the default of `ps1_run` / `ps1_play` / the app when no image is found. Kernel RAM laid out as the retail BIOS /
OpenBIOS lay it out (vectors, table of tables, A0 / B0 / C0 tables, TCBs, EvCBs), each table entry a trap stub the
interpreter hands to C (packs reach it the same way: kernel RAM is never lifted), the exception handler and its
SysEnqIntRP chains in MIPS like OpenBIOS's (syscalls, root counter / IRQ events, custom exit), events, threads, pads,
`cdrom:` straight from ISO 9660 and `bu00:` / `bu10:` on the card images; the EXE (or the disc's `SYSTEM.CNF` boot
file) starts at once. A call it does not implement returns 0 and is reported once with its number.

`ps1_run [--bios PATH|hle] [--exe X.exe | --disc X.cue] [--frames N] [--result] [--ppm OUT] [--wav OUT] [--tty]
[--until-tty TEXT] [--card FILE]` runs a program headless (`--result`: our homebrew's result block at 0x801F0000;
`--tty`: the console text, BIOS putchar / puts; `--card`: memory card 1 kept in a 128 KB .mcr file, loaded when
there, written back at exit when the game wrote it). Tests (`consoles/ps1/tests`): `ps1_cpu` and `ps1_gte` unit
tests, our homebrew (`ps1_hb_*`, the same results as PCSX-Redux, checksums included; `mcard` saves through the BIOS
memory card functions: `ps1_mcard` runs it on a blank card, again on the card it saved (the save found and read
back, the card unchanged) and on an unformatted one (formatted by the BIOS first), and checks the card image:
checksums, directory entry, data), JaCzekanski/ps1-tests (MIT, release fetched by
`tools/consoles/fetch_ps1_tests.sh`: cpu/cop, cpu/code-in-io, gte/test-all 1150 / 1150, dma/otc-test, dma/dpcr, timers),
and the same under the HLE BIOS (`ps1_hle` unit tests, one per implemented call; `ps1_hle_*`: the homebrew with the
same result block as under OpenBIOS, its discs booted by the HLE BIOS, mcard, ps1-tests, packs in lockstep).
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
`consoles/ps1/tools/ps1_tools/test_recomp.py`. All converge in 2-3 rounds at 99.1-99.9 % native instructions, except
gte/test-all (81.3 %: it writes each GTE command into a stub before running it). Lifted C is cut into blocks with one
scheduler check per segment, RAM accesses inline and anything else handed to the interpreter (XBA-161): about 0.6 s
of NDK clang (aarch64, -O1) per 1000 instructions on the synthetic game-sized program of `ps1/tests/bench` (was 3.5-4
s with a label and a check per instruction; docs/android/prepare.md « PS1 packs »); function bodies are cut at 1000
instructions because compile time grows faster than linearly with them. Design notes:
[ps1/tools/README.md](ps1/tools/README.md).

**PS1 GPU (P-7, XBA-165).** `consoles/ps1/runtime/ps1_gpu.c`, the GPU half of `ps1_devices.h` (CMake target
`ps1_gpu`, plain C11, no host dependency): GP0 / GP1, GPUSTAT / GPUREAD, CPU / VRAM / DMA transfers and a software
rasterizer with the hardware's rules (top-left fill, 1023 x 511 culling, quads as (v0 v1 v2) + (v1 v2 v3), 4 / 8 / 15
bpp textures with CLUT, page and window, the four semi-transparency modes, dithering, mask bit), span loops specialised
per primitive kind; `ps1_gpu_display` crops the display ranges and outputs 15- or 24-bit as XRGB. Commands run when
their last word arrives, so the GPU is never busy. Tests: `consoles/ps1/tests/test_gpu.c` (`PS1_CORE_ONLY=ON` builds
them without SDL2); `ps1_gpu_bench` is the micro benchmark. The gfx.exe homebrew frame matches PCSX-Redux pixel for
pixel.

**PS1 internal resolution (XBA-184).** `ps1_gpu_set_upscale(gpu, 2 | 4)` (`RECOMP_UPSCALE`, `RECOMP_PS1_UPSCALE`,
`ps1_play --upscale`; Android: Settings › Graphics › Internal resolution, a game's own in its Options), Beetle PSX's
software way: the native 1024 x 512 VRAM stays the truth (CPU / DMA reads, GPUREAD, texture and CLUT fetches,
VRAM-to-VRAM copies see exactly what they see at 1x), and a shadow VRAM of 1024N x 512N is drawn as well:
polygons and rectangles rasterized again with their vertices scaled by N (same rules and span loops, drawing area
scaled, textures sampled from the native VRAM; no PGXP, vertices stay on the native grid), lines N times finer and N
pixels thick, fills and CPU writes as N x N blocks, VRAM copies copying the shadow's blocks (an off-screen picture
copied to the display keeps its detail), the mask bit per copy. `ps1_gpu_display` reads the shadow: the picture is
N times wider and higher (up to 2560 x 2048 at 4x, which `host_window` uploads as it is; 24-bit MDEC pictures are
native pixels repeated); given a buffer too small for it (the save state thumbnail at 4x), the native picture. At 1x
there is no shadow and none of this runs: the output is byte-identical to before (the golden hashes did not move).
The shadow is derived, never saved: a state is the native GPU only (its size unchanged, so 0.3.0 states load), and
loading one rebuilds the shadow from the VRAM as N x N blocks, the detail coming back with the next frame drawn.
Cost: the shadow pass is N^2 times the native one, so a primitive covering enough shadow pixels has its rows shared
(interleaved) between the emulation thread and up to three workers (`ps1_gpu_set_threads`, `RECOMP_PS1_GPU_THREADS`;
by default 3 from 8 cores); each pixel is computed as by one thread, so the picture does not depend on the count.
`ps1_gpu_bench [seconds] [factor] [workers]`, x86-64 (Ryzen AI Max+ 395 at 3 GHz), the busy 2000-primitive frame
(about 0.5 Mpixels native, the PS1's fill-rate ceiling): 1x 3.8 ms; 2x 17.8 ms alone, 12.6 ms with 3 workers; 4x
54 ms alone, 22 ms with 3 workers; a big textured triangle's shadow pass scales 3.3 times with 3 workers. Real games
draw a fraction of that frame. Tests (`test_gpu.c`): the native VRAM and GPUREAD at 2x / 4x equal 1x's, golden hashes
of the shadow (gfx.exe replica, busy scene), flat rectangles / fills / sprites (flipped too) / straight lines exactly
the native pixels as blocks, the display, the state round trip, the same shadow with 0 or 3 workers, a 4x fuzz;
`test_main.c`: gfx.exe through the runtime at 1x / 2x / 4x (screenshot sizes and golden CRCs).

**PS1 disc, CD-ROM, SPU, MDEC (P-1 XBA-159, P-8 XBA-166, P-9 XBA-167).** The other devices of `ps1_devices.h`, plain
C11 without host dependency (CMake targets `ps1_cd`, `ps1_spu`, `ps1_mdec`). `ps1_disc.c` reads raw sectors and
tracks only (no ISO 9660): `.cue` (several `.bin` files, INDEX 00 / 01, PREGAP, relative paths, names matched ignoring
case), a single `.bin`, `.iso` (2048-byte sectors in a synthesized Mode 2 Form 1 sector), `.chd` through the vendored
libchdr (`third_party/libchdr`, master 47d7941 after v0.3.0, with the crafted-CHD bounds fixes; option `XBR_PS1_CHD`,
ON by default), and `fd:N` for single-file images. `ps1_identify.c` (target `ps1_identify`) makes this reader
`recomp_pack_rom_identify`'s disc hook (`recomp_pack_set_disc_hook`), so a `.chd` gets its title key (SYSTEM.CNF's
serial) where both are linked: libmain, the launcher's `libxba_import.so` (`Ps1Native`, the library scan) and
`ps1_play`; `recomp_pack.c` stays libc-only and leaves a `.chd` unidentified without the hook.
`ps1_cdrom.c` is the controller of psx-spx: banked registers, parameter / response / data FIFOs, INT1..INT5 queued
until acknowledged, first / second response times from psx-spx's measurements and the ps1-tests cdrom/timing hardware
log (seeks: a settling time plus distance, fitted to it), every command up to ReadTOC and GetQ (Test 20h answers vC2),
ReadN / ReadS with 800h / 924h sectors and DMA3, XA-ADPCM (filter, 37800 / 18900 Hz, psx-spx zigzag interpolation to
44.1 kHz) and CD-DA with the volume matrix into a 44.1 kHz stream (at 2x when Setmode says so: double pitch), report
mode with peak levels, sub-channel Q for GetlocP and the reports (pregaps: index 00 of the next track; the lead-out
AAh), the lid (`ps1_cdrom_set_lid`: INT5 01h 08h, stat bit 4 latched until a Getstat; `ps1_cdrom_set_disc` swaps
discs, XBA-166). GetID answers a data
disc as licensed (`SCEx` from the licence text at LBA 4, `SCEA` without one, as a modchip would), so a BIOS boots our
homebrew. `ps1_spu.c`: 24 ADPCM voices with Gaussian interpolation, exact ADSR and sweep envelopes, noise, pitch
modulation, ENDX, reverb (psx-spx formula with its FIR, the hardware's IIR form with the vIIR = -8000h quirk), capture
buffers, the SPU IRQ, FIFO and DMA4 transfers, CD input; KON / KOFF latched to the next sample, a voice starts four
samples after its key on, and the main volume scales the whole mix (voices, CD, reverb). `ps1_mdec.c`: RLE, IDCT with the scale table, 4 / 8 / 15 / 24-bit output, DMA0 / DMA1. Tests:
`consoles/ps1/tests/test_{disc,cdrom,spu,mdec}.c` on images and register streams they generate (a compressed CHD
from `chdman` too when it is installed); JaCzekanski's cdrom/getloc (passes), cdrom/timing (within the hardware log's
ranges) and cdrom/disc-swap on a 74:10 sparse synthetic disc (`run_jcz_cd.sh`); `ps1_hb_chd_overlay_*` boots CHDs
written by `ps1_tools.chd` through libchdr. The limits are listed at the top of each source (the CD-ROM's: its
sector buffers, the head after a Pause, audio sub-channels other than Q).

**Playing a PS1 game (XBA-168).** `ps1_runtime_run` (`consoles/ps1/runtime/ps1_runtime.h`) plays a disc or an EXE
on the host layer, the GBA's way: a disc (`.cue` with its tracks, `.bin`, `.iso`, `.chd`, or `fd:N` for a
single-file image) boots through OpenBIOS from `SYSTEM.CNF`, an EXE (recognised by its `PS-X EXE` magic) is
fast-booted. A disc boots fast too (`ps1_set_fast_boot`, on by default, `RECOMP_PS1_FASTBOOT=0` / `--no-fast-boot`
turn it off): OpenBIOS's shell (its own logo animation, about five seconds; there is no Sony logo) returns at once and
the BIOS boots the disc itself, SYSTEM.CNF's settings included, so nothing is skipped but the animation: our homebrew
discs reach their result in 85-145 frames instead of 390-420. A `.m3u` playlist (multi-disc games, one image per line,
`ps1_runtime_m3u`) plays its first disc; `ps1_runtime_change_disc(path)` (any thread: the app's in-game menu « Change
disc », F6 on the desktop for the playlist's next disc, `ps1_play --swap FRAME:DISC`) opens the lid, and a second later
closes it on the new disc (XBA-166). The picture is the GPU's display area (256 / 320 / 368 / 512 / 640 x 240 / 480, 15- or 24-bit) shown at
4:3 whatever its width (`host_window_set_aspect`), letterboxed, linear by default (`RECOMP_PS1_FILTER=nearest`,
`RECOMP_PS1_SCALE=integer`: whole multiples of the height; or the shared `RECOMP_FILTER` / `RECOMP_SCALE`). Pads, Xbox layout: A → Cross, B → Circle, X → Square,
Y → Triangle, LB / RB → L1 / R1, LT / RT → L2 / R2, Start, Back → Select, stick clicks → L3 / R3, D-pad; port 1 is a
DualShock plugged in in digital mode (the left stick drives the D-pad too) or, with `RECOMP_PS1_PAD=analog`, in
analog mode (both sticks); the game may switch it itself (config mode) and lock it, Back + L3 + R3 while playing is its
ANALOG button, and its rumble motors drive the host pad's (`host_pad_rumble`); every host pad drives port 1, or with `RECOMP_PS1_PLAYERS=2` host pads 1 and
2 drive ports 1 and 2. Sound: `ps1_audio_pull` (44.1 kHz) into `host_audio`, which SDL converts to the device's rate.
Pacing: the frame's length is the CPU cycles the core ran to the next VBlank (NTSC 59.94 / 59.29, PAL 50 Hz, as the
GPU's mode says), absolute deadlines nudged ±2 % by the audio queue. Memory cards: `<save dir>/ps1-<KEY>/card1.mcr`
and `card2.mcr`, 128 KB raw images as other emulators write them (.mcr / .mcd), read before the first frame, written
a second after the game's last write to the card (five at most after its first), at pause and at exit, only when
changed (the `ps1_runtime` test saves with the mcard homebrew: written by the debounce, then reloaded and found by
the game). BIOS: `--bios`, `RECOMP_PS1_BIOS`, the app's asset `ps1-bios/openbios.bin`, else `build/ps1-bios/openbios.bin`
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
(`consoles/ps1/tests/test_main.c`: the helpers and `.m3u` parsing, then gfx.exe fast-booted, through `fd:N`, a memory
card read back and left alone, gfx.cue booted by the BIOS to its 320 x 240 picture, the errors, a save written and
reloaded, a playlist's disc swap, fast boot on and off); `ps1_hb_fastboot_*` boot every homebrew disc fast.

**N64 core (N-1 XBA-169, N-3 XBA-171, N-4 XBA-172).** Interpreter first, the PS1's shape. `consoles/n64/runtime/n64_core.h`:
a VR4300 interpreter (`n64_cpu.c`: every MIPS III integer instruction, 64-bit ops, LL / SC / LLD / SCD, LWL..SDR,
branch-likely, traps; 32- and 64-bit kernel / supervisor / user address spaces; COP0 with COUNT / COMPARE, every
exception and its EPC / BD / BadVAddr / Context / XContext / EntryHi, the 32-entry TLB with page masks and ASIDs,
refill / XTLB refill / invalid / modified; a fixed cost per instruction by fetch region, 1.5 cycles from cached RDRAM;
no cache model: CACHE does nothing), COP1 (`n64_fpu.c`, on the host FPU: rounding modes, FCSR flags / causes /
enables, the unimplemented operation for denormals, quiet NaNs and out-of-range conversions, FS flushing, the FR = 0 /
1 register views and the half-mode operand rules), the bus (`n64_bus.c`: RDRAM 8 MB big-endian, MI, PI DMA with the
PI's block / misalignment rules and its write latch, RI / RDRAM registers reporting configured memory, SI, the RCP bus
rules for 8 / 16 / 64-bit accesses, MI repeat mode, the IS-Viewer debug buffer), VI and AI (`n64_io.c`: line timing
from H_SYNC / V_SYNC, the interrupt at V_INTR; double-buffered AI DMA at the DAC rate, the DAC's pulls switching
buffers without a gap, resampled to 48 kHz stereo by a 32-tap Kaiser-windowed sinc: SNR above 80 dB for tones up to
14 kHz from a 32 kHz DAC, no click at buffer boundaries, gaps or DAC rate changes), the VI's picture path
(`n64_vi.c`, below), and the PIF and saves (`n64_pif.c`). The RSP and the RDP are reached only through
`n64_devices.h`: target `n64_core` links `n64_rsp` and `n64_rdp`, each the real module or its stub
(`n64_rsp_stub.c`: SP registers and DMA, halts at once; `n64_rdp_stub.c`: walks the commands, SYNC_FULL interrupt).

Boot: the PIF is HLE, never a PIF ROM: the CIC is recognised from the ROM's IPL3 (CRC32 tables for 6101, 6102 / 7101,
6103, 6105, 6106, 7102; libdragon's open IPL3 by its banner; else a 6102), the header and IPL3 go to DMEM, PI domain 1
timing comes from the header, s3..s7 / sp / ra are what IPL3 expects (seed, TV type from the region, cold reset), the
RDRAM is reported configured (RI_SELECT, osMemSize at 0x80000318) so IPL3 takes its warm path, and the CPU starts at
0xA4000040 in the ROM's own IPL3. IMEM holds IPL2's first eight words, as the PIF leaves them (the 6105 / 7105 IPL3
decrypts its RSP program with them). That program spins on SP_SEMAPHORE while the CPU computes the boot checksum,
counting down in RSP $5, and games check what it leaves (Ocarina of Time's rspboot: $4, $6, $11 and $5 within 0.2 % of
the console's count; when the check fails, the game corrupts RDRAM at random and crashes). Our cost model cannot meet
that count, so for this IPL3 only the RSP is not run while it waits for the semaphore, and $5 gets the console's value
once its program ends (`n64_pif.c` header: sources and values). Joybus: controllers (info, buttons + stick), the controller pak (32 KB, data CRC;
a fresh one is formatted like a new .mpk: ID block and copies, index table, empty notes) and the rumble pak, EEPROM
4K / 16K, the RTC (status, control and time blocks on the host's local clock plus the offset the game sets). The CIC
challenge (PIF command bit 1, used by 6105 games) is acknowledged without a response: nothing of ours needs it. Saves: SRAM 32 KB, FlashRAM 128 KB (MX29L1100 ID, erase / program / read
through the PI), EEPROM, the four paks (`n64_save_data` / `n64_save_take_dirty` / `n64_save_load` per kind). The
cartridge's kind is never a per-game table: forced by `n64_options`, else the ED64 / libdragon header byte (`ED` at
0x3C, type in 0x3F), else the first access (a write to the flash command register makes it FlashRAM, any other
domain 2 access SRAM, an EEPROM command EEPROM, 16K once a block above 63 is used). libultra checks EEPROM block
numbers against the type the status command reports, so unless the kind is fixed the status answers like a 16K part
(4K games accept it) and the image stays 512 B until a block above 63 is used. ROMs: z64 / v64 / n64 byte orders
normalised, up to 64 MB, header parsed (`n64_rom_parse`).

`n64_run ROM [--frames N] [--result] [--ppm OUT] [--wav OUT] [--tty] [--pad MASK] [--pal|--ntsc] [--save PATH] [--hash]
[--info] [--regs]` runs a ROM headless (`--result`: our homebrew's block at physical 0x00300000; `--tty`: the IS-Viewer
text). Tests (`consoles/n64/tests`): `n64_cpu` and `n64_io` unit tests, our homebrew (`n64_hb_*`: hello, cpu,
timer_irq; rsp with `n64_rsp.c`, gfx with both modules; the same results as ares), and lemmy-64/n64-systemtest (MIT,
built from source by `tools/consoles/build_n64_systemtest.sh`; `n64_systemtest` passes when every failing test is a
known gap of `systemtest_known.txt`: the caches, reverse-endian user mode, one PI end-of-page quirk). The caches
are left out on purpose: those tests check stale lines (a cached read not seeing an uncached write until an
invalidate, write-back timing), which needs a tag lookup on every cached access in the interpreter and the same in
lifted code (the lockstep must match), for a behaviour games only ever avoid; reverse-endian user mode is used by no
game (only by Unix-style kernels). About 180 frames/s
(190 MIPS) on the x86 dev host for cpu.z64 and gfx.z64. On a desktop:

```sh
tools/consoles/build_homebrew.sh n64 && tools/consoles/build_n64_systemtest.sh
cmake -S consoles/n64/tests -B build/n64 -DN64_CORE_ONLY=ON && cmake --build build/n64 && (cd build/n64 && ctest)
build/n64/n64_runtime/n64_run build/homebrew/n64/gfx.z64 --result --ppm gfx.ppm
build/n64/n64_runtime/n64_run build/n64-systemtest/n64-systemtest.z64 --tty --frames 900
```

**The VI's picture (`n64_vi.c`, `n64_display`).** What a television showed, not a crop of the framebuffer: a fixed
canvas of 640 VI pixels from H = 108 (NTSC) / 128 (PAL) and 240 / 288 lines from half-line 34 / 44 (480 / 576
interlaced), the active area placed where H / V_VIDEO put it and black around it (a narrow or short picture keeps
its aspect and place); 320 wide when X_SCALE is 0.5 or less (a 320 x 240 game comes out at its own pixels). The
VI's horizontal guard band is modelled (as paraLLEl-RDP's VI and angrylion do): the first 8 and last 7 VI pixels of
the active area are black unless that edge is clamped to the scan-out, so a standard mode shows canvas columns
4..316 of 320 (OoT draws its fades one column short; a console never shows that column). Per
pixel: X / Y_SCALE steps and their start offsets, bilinear resampling (AA modes 0..2) or replication (mode 3), the AA
filter on partially covered pixels (coverage from the RDP's hidden RDRAM bits, `n64_rdp_hidden_bits`; without them
the pixel's low bit), the divot filter, the dither filter (16-bit), gamma and gamma dither. Interlaced modes are
woven: each field goes to its rows of the canvas (the lower field by the framebuffer position of its first line), so
a 480-line game is shown whole, without combing or bobbing. About 0.4 ms per 320 x 240 picture on the x86 dev host
(1.2 ms with AA on every pixel, the worst case).

**Playing an N64 game (XBA-174).** `n64_runtime_run` (`consoles/n64/runtime/n64_runtime.h`) plays a cartridge on the
host layer, the PS1's way: a ROM file in any byte order (`.z64`, `.v64`, `.n64`, or `fd:N`), no BIOS (the PIF is HLE),
a title pack optional (`--pack LIB`, XBA-174: `python3 -m tools.prepare game.z64` builds one, [prepare.md « N64
packs »](../docs/android/prepare.md#n64-packs-xba-174); it runs the lifted code when its `pack.json` and table say it is
this cartridge's, the SHA-256 of the ROM in big-endian order, and the game plays interpreted otherwise; `--missed FILE`
keeps what it did not cover for the next Prepare, `consoles/n64/runtime/n64_pack.h`). The picture is the VI's
canvas (320 or 640 x 240, 480 interlaced, PAL 288 / 576 lines; `n64_vi.c` above) shown at 4:3 whatever its size,
letterboxed, less the VI's black guard band (8 VI pixels cut from each side, 4 columns of 320, the rest still filling
4:3 as a television's overscan hid them; `RECOMP_N64_CROP_EDGES=0` or `n64_play --edges` shows them), linear by default (`RECOMP_N64_FILTER=nearest`, `RECOMP_N64_SCALE=integer`; or the shared `RECOMP_FILTER` /
`RECOMP_SCALE`). Controller, Xbox layout: left
stick → stick (radial dead zone 8 %, `RECOMP_N64_DEADZONE` in percent; ±80 on an axis at full travel,
`RECOMP_N64_STICK_RANGE`; the diagonals go to about ±68, the real stick's octagonal gate), A → A, B and X → B, right
stick → C buttons (past half its travel), Y → C-up, LB → L, RB → R, LT or RT → Z, Start, D-pad; keyboard on a desktop
(arrows = stick, X = A, C = B, Z = Z, Enter = Start, Q / E = L / R, I / K / J / L = C buttons, T / G / F / H = D-pad).
Every host pad drives controller 1, or with `RECOMP_N64_PLAYERS=2..4` host pad N drives controller N. The accessory
(`RECOMP_N64_PAK`): a controller pak by default, `rumble` a rumble pak whose motor drives the host pad's rumble, `none`.
Sound: `n64_audio_pull` (48 kHz) into `host_audio`. Pacing: the frame's length is the CPU cycles the core ran to the
next VI interrupt (NTSC about 60 Hz, PAL 50 Hz, one field per interrupt when interlaced), absolute deadlines nudged ±2 %
by the audio queue; a line every 10 s gives the frame rate reached (`n64: N fps over the last 10 s`, with the frames
skipped). Frame skip (`RECOMP_N64_FRAMESKIP`, `n64_play --frameskip`): `auto` (default) skips the picture of a frame
that starts more than half a frame late or while the audio queue is about to run dry, at most 3 in a row; `N` shows
one frame in N + 1; `0` never. A skipped frame still runs whole (CPU, RSP, the RDP drawing into RDRAM, which games
read back) and makes its audio: only `n64_display` and the present are skipped. The RDP's
scanline-band threads (`n64_rdp_set_threads`): `RECOMP_N64_RDP_THREADS`, else 2 on Android, else the core's
(`XBR_N64_RDP_THREADS`, else 1). Saves: `<save dir>/n64-<KEY>/game.eep` (EEPROM 4K 512 B / 16K 2 KB), `game.sra` (SRAM
32 KB), `game.fla` (FlashRAM 128 KB), `pak1.mpk`..`pak4.mpk` (controller paks, 32 KB): raw images, the files
mupen64plus and ares use (rename to copy one in). A cartridge save on disk is read before the first frame and also
fixes the save type (an `.eep`'s size picks 4K or 16K unless the header says; a 2 KB `.eep` for a 4K game gives its
first 512 bytes); written a second after the game's last write (five at most after its first), at pause and at exit,
only when changed. `RECOMP_N64_TTY=1` prints the IS-Viewer text. The Android app calls it through `n64_runtime_start`
([app-shell.md](../docs/android/app-shell.md) « Nintendo 64 games »). On a desktop:

```sh
cmake -S consoles/n64/tests -B build/n64 && cmake --build build/n64          # or the root build (XBR_CONSOLE_N64 ON)
build/n64/n64_runtime/n64_play game.z64
build/n64/n64_runtime/n64_play build/homebrew/n64/gfx.z64 --scale integer --pak rumble --rdp-threads 2
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/n64/n64_runtime/n64_play gfx.z64 --frames 300 --screenshot last.ppm
xvfb-run -a build/n64/n64_runtime/n64_play gfx.z64 --frames 200 --capture 180 present.ppm   # the real present path
```

The title key comes from the ROM as the app reads it (`recomp_pack_rom_identify`: `NSME-635A2BFF`; the same formula
when it cannot, `n64_runtime_fallback_key`). Every option is listed at the top of `runtime/tools/n64_play.c`. Test:
`n64_runtime` (`consoles/n64/tests/test_main.c`: the helpers, then gfx.z64's picture, through `fd:N` with two RDP
threads and a rumble pak, byte-swapped as `.v64`, an EEPROM and a controller pak read back and left alone, the app
shell's entry, the errors). Diagnostics in the log (stdout, logcat on Android; no effect on the game): the first
guest fault exception of a session (`n64: guest fault at frame N: TLB miss (store) (Cause …), EPC …, BadVAddr …`:
TLB, address and bus errors, reserved instruction, coprocessor unusable other than COP1), and `n64: guest stuck at
frame N (…)` with pc, Status, Cause, EPC, BadVAddr and the DPC registers, once per episode, when for 5 s nothing moves
(no new VI origin, SI / PI DMA or audio buffer: an exception storm, a deadlock) or the picture stays after a fault
(a game's own crash screen); the 10 s frame-rate line also gives the pc. gfx.z64 runs at about 60 fps paced on the x86 dev host with one RDP thread.

**N64 RSP (N-5, XBA-173).** `consoles/n64/runtime/n64_rsp.c`, the RSP half of `n64_devices.h` (CMake target
`n64_rsp`, plain C11, no host dependency): LLE, so every microcode runs as on the hardware (no per-game HLE). Scalar
unit (MIPS subset, delay slots, wrapping DMEM accesses, BREAK / intr-break), COP0 = SP registers (instant DMA with
length / count / skip, status set / clear pairs, semaphore) and the DPC registers of the attached RDP (an END write
runs the RDP at once, so XBUS lists in DMEM are consumed before the microcode reuses them), vector unit (every load /
store with its wraparound rules, the 16 element selectors, every computational op, the reciprocal / inverse square
root ROMs generated from their formulas). Accumulator as bits 47..16 (int32) + 15..0 (uint16): 8-lane 32-bit loops
the compiler vectorises, identical results everywhere. One cycle per instruction. Tests
(`consoles/n64/tests`): `test_rsp.c` (hand-checked vectors, and 93 differential groups of random microcode over every
vector funct, load, store, DMA, status and scalar op whose checksums must equal those the ares oracle computed from the
homebrew ROM `homebrew/rsp`), `test_rsp_rdp.c` (microcode driving the real RDP over XBUS and from RDRAM);
`n64_rsp_bench` (by hand; `-n COUNT` runs a fixed instruction count and prints a DMEM hash): ~115 M RSP
instructions/s on a vector-dense loop on the x86 dev host (was ~97).

*NEON (arm64, XBA-173).* On arm64 the hot vector ops are NEON intrinsics (`vu_op_neon`: the multiply-accumulate family
VMULF..VMADH, VADD / VSUB / VADDC / VSUBC, VLT / VEQ / VNE / VGE, VCL / VCH, VMRG, the logic ops, the divide moves),
every element selector is one table lookup (`tbl`), and the aligned LQV / SQV / LDV / SDV are a load + `rev16`; the
remaining ops run the portable loops (`-DN64_RSP_NO_NEON` forces them everywhere). Portable gains for every host:
vector computational ops dispatched before the scalar decode, selectors 2..7 without the table gather, fixed-count
lane loads / stores. Bit-identical: the 93 ares groups and the RSP + RDP tests pass on x86 and on arm64 (qemu-user),
and a random-microcode differential (vector ops, loads, stores, flags) hashes the same on x86, arm64 NEON and arm64
portable. Measured with callgrind (aarch64, gcc 14 -O3, under qemu-user in the `xba-arm64-deps` image) on
`n64_rsp_bench -n`: 105.7 -> 68.4 arm64 instructions per RSP instruction (-35 %; 92.8 with the portable gains alone);
x86-64 (SSE2): 153.5 -> 136.5.

**N64 RDP: software and Vulkan (N-5, XBA-173).** `n64_rdp` (the RDP half of `n64_devices.h` + `n64_rdp.h`) is
`n64_rdp_select.c`, which picks a back end at `n64_rdp_create` (`n64_rdp_backend.h`). The software RDP is `n64_rdp.c`
(target `n64_rdp_sw`, its public names renamed `n64_rdp_sw_*` by the forced `n64_rdp_sw_names.h`). The Vulkan RDP is
`n64_rdp_vk.cpp` (target `n64_rdp_vk`), the same DPC front end over paraLLEl-RDP (Themaister, MIT, vendored in
[third_party/parallel-rdp](../third_party/parallel-rdp/README.md)): an LLE RDP on Vulkan compute, conformance-tested
bit for bit against angrylion.
- **Building it.** CMake option `XBR_N64_RDP_VULKAN`: ON for Android, OFF elsewhere (`-DXBR_N64_RDP_VULKAN=ON`; it
  needs C++17). Vulkan is dlopened by a private volk (symbols renamed `prdp_*`), so a device without Vulkan still runs.
- **Choosing it.** `n64_rdp_set_backend`, else `RECOMP_N64_RDP` = `vulkan` | `software` | `auto` (the default). `auto`
  means Vulkan on Android when a device with Vulkan 1.1 and 8/16-bit storage is there, software elsewhere. Anything
  that stops the Vulkan RDP from starting falls back to software (logged). The start line names the back end
  (`n64: ... RDP vulkan, threads 1`), and the Vulkan one also logs `n64: Vulkan RDP on <GPU> (paraLLEl-RDP)`.
- **Coherence.** paraLLEl-RDP gets the core's RDRAM without imported host memory (its "incoherent" path). It uploads
  the pages a render pass reads and writes back, byte-masked, the bytes the GPU wrote, swapping each word between the
  N64 byte order and the GPU's host words (our patch). On SYNC_FULL the CPU waits for the GPU and the write-back,
  then the DP interrupt is raised. `n64_rdp_flush` (called before the VI reads the frame buffer) waits for commands
  queued since then. The colour / Z rows drawn are reported through `bus.wrote` after the wait, as `n64_rdp.c` does.
  `n64_rdp_set_threads` is a no-op there (1).
- **Shaders the driver cannot compile.** At start, `n64_rdp_vk.cpp` draws a few commands into a corner of RDRAM
  (saved and restored): fills of the colour and Z images and a textured rectangle with a Z test. Every pipeline must
  compile, and the pixels must come out right. If not, it tries again without subgroup operations (when the default
  shaders used them), then with paraLLEl-RDP's ubershader. The ubershader is skipped on Qualcomm's proprietary
  driver (Adreno 740 on the AYN Thor): it passes the check there but Ocarina of Time comes out black, without a
  Vulkan error; `RECOMP_N64_RDP_UBERSHADER=1` tries it anyway. When all three fail, the software RDP takes over. The
  start line names what was kept and the driver (`n64: Vulkan RDP on <GPU> (paraLLEl-RDP, default shaders; driver
  <driverName> <driverInfo>, system)`). Our Granite patch
  never compiles a failed pipeline twice, and logs the program and its spec constants once
  ([third_party/parallel-rdp](../third_party/parallel-rdp/README.md)). If a pipeline still fails during the game
  (another frame buffer format, for example), the failure is logged and `n64_rdp_select.c` switches to the software
  RDP when the RDP is idle. The RDP state is lost then, and so is the frame being drawn. `RECOMP_N64_RDP_CHECK=0`
  skips the check. `PARALLEL_RDP_TEST_FAIL_PROGRAM=depth_blend` (or `rasterizer`, `tile_binning_combined`, ...)
  fakes a failing driver, to test the fallbacks on lavapipe.
- **Internal resolution (XBA-185).** `RECOMP_N64_UPSCALE` (else the shared `RECOMP_UPSCALE`) = `1` | `2` | `4` |
  `8`, default 1, read at `n64_rdp_create` (`n64_rdp_set_upscale` overrides). The Vulkan RDP then draws each frame
  buffer pass twice: at 1x into RDRAM as before, so whatever the game reads back is unchanged, and at 2x / 4x / 8x
  into GPU copies of RDRAM (paraLLEl-RDP's upscaling, only for 16 / 32-bit frame buffers at least 256 pixels wide).
  Its own VI scans the upscaled one out (`n64_rdp_scanout`) and `n64_display` returns that picture, read back to the
  CPU: 640 VI pixels x the scale wide, 240 / 288 lines (480 / 576 interlaced, upscaled bob instead of our VI's weave)
  x the scale high, 1280 x 480 at 2x. The runtime shows it at 4:3 and crops the guard band as at 1x
  (`RECOMP_N64_CROP_EDGES`); `--screenshot` and `n64_run --ppm` write it at that size. Costs and limits:
  - the upscaled RDRAM (8 MB x the factor squared) is bound as one storage buffer, so it must fit the device's
    `maxStorageBufferRange` (128 MB on lavapipe and many mobile drivers: 4x at most), and the buffers and the VI's
    images must fit in half the largest device-local heap. The factor is lowered with a log line otherwise
    (`upscale 8x needs a 512 MB storage buffer, ... allows 128 MB`, `upscale lowered from 8x to 4x`);
  - the scanout is halved (filtered, super-sampled) until it is at most 2560 pixels wide, so 8x is read back at 4x;
    `RECOMP_N64_UPSCALE_DOWNSCALE=0..3` sets the halvings;
  - texture rectangles are drawn at 1x by default (2D and HUD art made for 1:1 texels shows seams otherwise):
    `RECOMP_N64_UPSCALE_NATIVE_TEXRECT=0` upscales them; `RECOMP_N64_UPSCALE_NATIVE_LOD=1` computes texture LOD as at
    1x; `RECOMP_N64_UPSCALE_SSREADBACK=1` makes the RDRAM copy the upscaled picture averaged down (not bit-exact);
  - the start-up check runs upscaled: its frame buffer is 256 pixels wide, so the upscaled passes run, and its VI
    scanout must come out at the expected size and colour. A shader try that fails upscaled is made again at 1x
    (`default shaders fail upscaled (2x) on <GPU>, trying 1x`). The start line ends `upscale 2x, scanout 2x`;
  - the software RDP always draws at 1x: `n64: upscale 2x needs the Vulkan RDP, the software RDP draws at 1x`, also
    when the Vulkan RDP cannot start (Qualcomm's own driver on the Thor) or fails during the game.
- **Custom Vulkan driver (Android).** The player can supply a driver, Mesa Turnip for the Adreno, which compiles
  paraLLEl-RDP's shaders where Qualcomm's driver does not. `n64_vk_driver.c` reads the selection the app put in the
  environment (`RECOMP_VK_DRIVER_DIR`, `RECOMP_VK_DRIVER_LIB`, `RECOMP_VK_DRIVER_ID`, `RECOMP_VK_HOOK_DIR`;
  `RECOMP_VK_DRIVER=system` forces the system driver: [environment variables](../docs/runtime/environment-variables.md#custom-vulkan-driver-n64-rdp)),
  dlopens `libxba_vkdriver.so` (libadrenotools, [third_party/adrenotools](../third_party/adrenotools/README.md)) and
  hands its `vkGetInstanceProcAddr` to Granite's `init_loader`. The same shader tries run on that driver. If it cannot
  be opened, makes no device, or fails every try, the system's libvulkan is loaded again and the tries run there:
  custom driver, then system driver, then the software RDP, each step logged. When adrenotools could not load the
  custom library it silently loads Qualcomm's driver; the `driverID` tells, and the log says `custom driver <id> was
  not loaded`. A driver that crashes the process instead: `vk_drivers/trying` holds its id while it is opened and
  checked, and the next start skips it. The start line ends `custom <id>` or `system`. Off Android the open always
  fails (no `libxba_vkdriver.so`), the same path as a driver that cannot be opened. In the app the player picks
  the driver in Settings › Graphics › Vulkan driver (downloaded from GitHub releases or imported as a zip; none
  ships with the app): [app-shell.md « Vulkan drivers »](../docs/android/app-shell.md#vulkan-drivers).

Tests (`consoles/n64/tests`, built with `-DXBR_N64_RDP_VULKAN=ON`; without a GPU, Mesa's lavapipe with
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`):
- `n64_rdp_vulkan` runs `test_rdp.c` on the Vulkan RDP with its own golden hashes (busy scene, gfx frame, mode
  matrix). Seven feature cases are reported, not failed (`vk_known` in `test_rdp.c`), where the two RDPs differ on
  purpose: fill rectangles against the scissor edge, flipped and TLUT / 8-bit copy mode, a 4-bit texture-image load
  (crashes a real RDP; paraLLEl-RDP drops it), the YUV convert, the non-forced blender on fully covered pixels. Six of
  the seven are places where paraLLEl-RDP is the angrylion-accurate one. The seventh is the chroma key, which
  paraLLEl-RDP does not implement and `n64_rdp.c` approximates.
- `n64_rdp_compare` (`n64_rdp_test --compare`) draws the same lists on both and reports the differences per 16-bit
  unit. The gfx.z64 frame differs in 322 of 76,800 pixels, all on the triangle's edges: the software RDP draws
  edge pixels angrylion leaves out. The busy scene differs in 25 % of its pixels, 72 % of those off by one step of a
  5-bit channel (the software RDP's interpolation and filter rounding). The 64 random mode-matrix lists (every mode,
  many invalid on purpose) differ in 0.1 to 11 % of their colour units (one is identical).
- `n64_rdp_vulkan_bad_driver`: `n64_rdp_vulkan` with a custom driver that cannot be opened; the log says why and the
  system driver passes the same tests. `n64_vk_driver` (every build, no Vulkan): the `RECOMP_VK_*` variables, the
  trial marker and `last_start`.
- `n64_hb_gfx` passes with `RECOMP_N64_RDP=vulkan` too.
- Upscaling (XBA-185): `n64_rdp` / `n64_rdp_vulkan` test the option parsing (`RECOMP_N64_UPSCALE` over
  `RECOMP_UPSCALE`) and a 2x scanout of a 320 x 240 frame buffer (1280 x 480, the colours in place; the software RDP
  refuses it). `n64_hb_gfx_upscale` (`run_upscale.sh`) runs gfx.z64 at 1x and 2x: the 2x picture is 1280 x 480 and
  matches the 1x one sampled at its pixel centres (`upscale_compare.py`).
- lavapipe's speed means nothing for a phone.

**N64 static recompilation (N-1 XBA-169, N-2 XBA-170, N-3 XBA-171).** The PS1's model on the VR4300: an N64 game's
code is lifted to C, compiled for the host into a title pack (`libtitle.so`), and runs natively on the same core; IPL3,
the RSP, the RDP, VI / AI / PI / SI, the PIF and interrupts are the interpreter's code either way. Universal: the ROM
alone, no symbol or ELF file (N64Recomp needs a decompilation's ELF), no per-title configuration; what the static
pass misses runs in the interpreter, which records it, bytes included, for the next build (the discovery loop).
Contract (`n64_recomp.h`, frozen between `n64_tools`, Prepare, the loader and the core; change together and bump
`N64_RECOMP_ABI`):

- `python3 -m n64_tools.recomp ROM OUTDIR [--discovered FILE]` (`consoles/n64/tools`, pure Python, our own MIPS III
  decoder) writes `OUTDIR/n64_rc_NNN.c` + `n64_rc_table.c`; each compiles with `-I consoles/n64/runtime` alone. The
  code comes from the boot (the classic boot megabyte at the header's address, or libdragon's ELF in the ROM read
  from its program headers as its IPL3 does), PI DMA copies with constant arguments, and the recorded code; functions
  from a recursive walk (calls, tail calls, jump tables, code pointers, the exception vectors) and a linear sweep of
  the text. Design notes and measurements: [n64/tools/README.md](n64/tools/README.md).
- The library exports `n64_recomp_title_table`: entries (32-bit virtual address, length, **content hash**, function),
  the ROM's SHA-256 and boot PC. N64 code runs from RDRAM (IPL3's copy, PI DMA, overlays, TLB-mapped segments), so an
  entry runs only while memory holds its words: the core keeps a write counter per 4 KB RDRAM page (CPU and lifted
  stores, PI / SI / SP DMA), re-hashes an entry only when a page it covers changed, and picks, among entries sharing
  an address, the one that matches. TLB-mapped entries stay within one 4 KB page: the PC is translated with the
  current TLB / ASID / mode, the bytes are checked at the physical page, the answer is cached until a TLB write or an
  EntryHi write. The lifted code's ABI is `n64_recomp_rt.h`: the first members of `n64_machine` (`n64_rc_head`: time,
  stop, instruction count, RDRAM and its page counters, MI mode, then `n64_rc_cpu`: GPRs, HI / LO, PC / next PC /
  delay slot, COP0, FPRs, FCSR; static asserts in `n64_recomp.c`) and the core functions `n64_rc_read` / `_write` /
  `_read_partial` / `_write_masked` / `_cop1` / `_ctc1`, which the loading executable exports
  (`n64_core_exported()`: n64_run; the app's libmain.so must link `n64_core` whole-archive the same way).
- `n64_attach_recomp` / `n64_recomp_missed` / `n64_recomp_write_missed` (`n64_recomp.c`, called from `n64_cpu_run`):
  RDRAM code in kernel mode, once the CPU reached the boot PC, runs lifted code when an entry matches, the interpreter
  otherwise; the discovered file gets `ADDR HASH HEXBYTES` per miss (a 2 KB window, never past a mapped page). An
  address seen with 8 contents is code generated at run time: left to the interpreter.

Lifted code is tick-exact with the interpreter, in the GBA / PS1 recompilers' shape (compile time on the phone):
functions cut into blocks (entry, jump targets, return points, every 12 instructions), only block starts resumable
(inside one, the interpreter runs to its end: `rc_hint`), one scheduler / interrupt check per block (the interpreter
finishes the block when an event lands inside), checked again after every call into the core; the same fetch cost by
segment, the same multiply / divide / FPU / uncached / bus costs at the same points, the instruction count (COP0
Random derives from it), so COUNT = COMPARE and RCP interrupts land in front of the same instruction; cached RDRAM
accesses (and KSEG1 stores) inline, known constant addresses off it (the drivers' RCP registers) through the core,
pointer accesses off it handed to the interpreter with the rest of their block; only the guest registers a function
uses in locals; files cut by a hash of function identity (stable names: a discovery round recompiles about one file
per new function); delay slots inline, branch-likely skipping its slot; COP0 (but CACHE), SYSCALL / BREAK, LL / SC, a branch in a delay slot and any instruction whose exception
condition holds (overflow, trap, faulting access, COP1 unusable) handed to the interpreter with the state in front of
it (exact EPC / BD / BadVAddr); COP1 arithmetic and CTC1 through `n64_fpu.c`, which raises FPE / unimplemented
itself; calls and indirect jumps through the dispatcher (bounded C stack). `n64_run ROM --pack LIB --lockstep` checks
it: an interpreter-only twin alongside, the whole state (GPRs, FPRs, COP0 with COUNT, the TLB, time, instruction
count, scheduler, MI, RDRAM, RSP memory) and the picture compared after every frame. On a desktop:

```sh
tools/consoles/n64_recomp_build.py game.z64 build/rc/game --loop 4 --n64-run build/n64/n64_runtime/n64_run --frames 1800
build/n64/n64_runtime/n64_run game.z64 --pack build/rc/game/libtitle.so --lockstep --stats --frames 600
build/n64/n64_runtime/n64_run game.z64 --pack build/rc/game/libtitle.so --missed build/rc/game/discovered.txt
```

Tests (`consoles/n64/tests`, ctest `n64_rc_*`): every homebrew through the discovery loop then in lockstep, with
the discovery measured against its ELF (`recomp_rom.sh`: at least 99 % of the function symbols found, never an
input); n64-systemtest under `--pack` (the interpreter's pass / known-gap set) and in lockstep
(`recomp_systemtest.sh`); `n64_rc_fuzz`: random MIPS III code (64-bit ops, branch-likely, unaligned accesses, COP1
in both FR modes, overflows / traps / FPE exceptions) under a COUNT = COMPARE interrupt every 40-600 COUNT ticks
(`recomp_fuzz.py`); pytest `consoles/n64/tools/n64_tools/test_recomp.py`. Measured on the x86 dev host (loaded,
12 cores): the homebrew converge in 1-2 rounds, 98.4-99.5 % native instructions once IPL3 is past; n64-systemtest
in 3 rounds at 97.2 % native (rounds 2 and 3 recompiled 5 and 2 of its 237 files), its 3721 tests giving the
interpreter's result. Recompiled vs interpreted: 1075 vs 146 frames/s on n64-systemtest (CPU-bound), 3700-4600 vs
180 on timer_irq / gfx (their idle loop after the verdict), 1190-1215 vs 190 on rsp (RSP-bound); the interpreter
alone is within noise of what it was before the page write tracking. Lifted C costs 0.8-0.9 s of compiler per 1000
instructions with gcc -O1 -fno-tree-slp-vectorize on that host (homebrew: 28-38 k instructions in ~4-5 s with 6 jobs;
n64-systemtest: 407 k instructions in ~77 s with 6 jobs, 1.1 s / 1k).

**GBA / PS1 interpreter speed (XBA-191).** Profiled with callgrind (instruction counts, portable enough to steer arm64
work; qemu timings mean nothing) on the homebrew corpus, the jsmolka / JaCzekanski suites and the synthetic game-sized
programs of `gba/tests/bench` and `ps1/tests/bench` (`build_rom.sh` / `build_exe.sh`, CPU-bound), each run with
`--run-hash` so a change that alters a picture, a sample or a cycle shows. What changed, all byte-identical (run
hashes, lockstep, save-state round trips, a differential fuzz of the PPU against its previous version):
- Both interpreters' step functions, and on the GBA both decoders and the data-processing class, are inlined into
  the run loop (a call per instruction and its callee-saved registers were a fifth of the PS1 interpreter's cost);
  the opcode fetch from the regions code runs from (GBA ROM / IWRAM / BIOS with the `fetch16` / `fetch32` cost tables
  lifted code already uses; PS1 main RAM) is inline, the rest goes through the bus as before.
- PS1 byte / halfword loads and every store to main RAM skip the bus call (same decode, page write counter bumped).
- With a pack, code no pack holds (the BIOS and its interrupt handlers, GBA RAM code) runs in the inlined
  interpreter loop (`gba_cpu_step_outside_rom`, `ps1_cpu_step_unliftable`) instead of a call per instruction from
  the recompiler's loop; on the PS1 bench that code is still most of the frame.
- GBA PPU: layers composed back to front a layer at a time instead of a front-to-back search per pixel; 4bpp text rows
  mirrored once, empty rows skipped; unscaled bitmap rows walked directly. PS1 SPU: no interpolation for an off,
  silent voice.

x86-64 Ryzen AI Max+ 395, gcc -O3, before -> after, instructions per frame: GBA bench 20.3 M -> 13.5 M, gfx.gba 7.7 M
-> 4.9 M, jsmolka arm 5.7 M -> 2.6 M, bench with its pack 9.6 M -> 6.3 M; PS1 bench 45.4 M -> 36.5 M, gfx.exe 53.5 M
-> 42.6 M, bench with its pack 33.9 M -> 26.9 M. Wall clock per 600 frames: GBA 1.3-1.6x faster, PS1 1.06-1.16x
(that wide core hides most of the PS1 interpreter's removed loads and stores; the Cortex-A715 / A710 cores of the
Thor should see more of it). LTO of the cores gains 0-4 % there and is not enabled; the Android build already
compiles them -O3 -mtune=cortex-a715. What is left on the PS1 is the interpreter itself (~80 instructions per guest
instruction: the per-step delay-slot and load-delay bookkeeping through memory), which a register-cached loop or a
predecoded block cache would address.

**Frame generation (XBA-186).** In a build with `XBOXRECOMP_LSFG` (Android `-PxbaLsfg=1`), under `RECOMP_LSFG=1`, the
host window feeds the LSFG pacer for every console: only distinct pictures reach the generator (a 30 fps game
presents each one twice at 60 Hz), the generated frames take the repeats' slots, and the runtimes' frame sleep goes
through `host_window_sleep_until`, which draws the frames due meanwhile. A game already at 60 distinct pictures is shown
as it comes; fast-forward bypasses it, a state load and a pause start a fresh history; pictures wider than 1280 are
generated at a reduced size. Latency: half the game's frame period plus the generation cost (16.7 ms + c at 30 fps).
Details: [Frame generation › Console window](../docs/technical/framegen.md#console-window).

**Save states and fast-forward (XBA-182, XBA-183).** GBA, PS1 and N64 (the Xbox has none: its state is the whole
guest memory and GPU). Three layers:

- The cores: `gba_state_save` / `gba_state_load` (`gba_core.h`, `gba_state.c`) and `ps1_state_save` / `ps1_state_load`
  (`ps1_core.h`, `ps1_state.c`) put the whole machine in a blob: a header (magic `XRSTATE`, the console, its format
  version `GBA_STATE_VERSION` / `PS1_STATE_VERSION`, an FNV-1a 64 hash of the game: the ROM; the EXE given, else the
  disc in the drive (its size and volume descriptor); and of the BIOS: 0 for the built-in / HLE one) and sections
  (id, size, bytes). GBA: the `gba_machine` whole (CPU and banks, cycle clock and scheduler, IO, timers, DMA, PPU
  registers, VRAM / OAM / palette, EWRAM / IWRAM, the APU and its FIFOs, the save chip and its contents). PS1: the
  machine (CPU, COP0, delay slots, GTE, scratchpad, interrupt controller, DMA, timers, the video scheduler, SIO with
  the pads and both memory cards), main RAM, the GPU (VRAM and its state), the CD-ROM controller (registers, FIFOs,
  commands and responses in flight, the drive's position, sector buffers, XA / CD-DA), the SPU (state and its 512 KB),
  the MDEC (its output not read yet), the HLE BIOS's state, the recompiled code's boot flag. Each device has its
  `ps1_<dev>_state_size / _save / _load` (`ps1_devices.h`; the placeholders keep nothing). A state from another game,
  another BIOS, another format version or another build (a section whose size is not this build's) is refused and
  the machine is left as it was. Host pointers are never in a state, and neither is anything a pack adds: a state is
  the machine only, so it loads the same whether the game runs interpreted or recompiled, either way round. On load
  the GBA's lifted-code hand-offs in flight are reset (its dispatch is a function of the ROM, which never changes);
  the PS1 bumps every RAM page's write counter and flushes its dispatch cache, so each lifted function is hashed
  against RAM again before it runs. Take and load states between frames. Loading clears the save chip's / cards'
  dirty flags: what the state brought back reaches `game.sav` / `card1.mcr` only once the game writes its save again.
  N64 (XBA-183): `n64_state_save` / `n64_state_load` (`n64_core.h`, `n64_state.c`), `N64_STATE_VERSION`, the ROM's
  hash and RDRAM's size (4 or 8 MB) in the header. Sections: the `n64_machine` (the VR4300's GPRs, HI / LO, PC and
  delay slot state, LL bit, COP0 with COUNT / COMPARE and the 32 TLB entries, COP1's registers, FCSR and the FR mode;
  the tick clock and the event scheduler; MI, VI, AI with its two-buffer DMA queue and the resampler's phase and
  history, PI with its latch, SI and PIF RAM, the controllers with their paks, RI; EEPROM, SRAM and the four
  controller paks, the RTC and the FlashRAM's command state), RDRAM (all-zero 4 KB pages left out: 1.5 MB for our
  homebrew, up to 8 MB for a game), the RSP (`n64_rsp_state_*`, `n64_devices.h`: IMEM / DMEM, the scalar registers,
  the vector unit's registers, accumulator, flags and divider, PC, the SP registers and semaphore: the RSP is LLE, so
  there is no HLE state), the RDP (`n64_rdp_state_*`: the DPC registers and the command being collected, then the back
  end's own: the software RDP's modes, combiner, colours, tiles, TMEM, noise seed and hidden RDRAM bits, drawn out
  first whatever its thread count; the Vulkan RDP is synchronised first, so RDRAM holds every pixel, and keeps the
  last of each state command it was given, replayed into paraLLEl-RDP on load, TMEM not kept: games load textures
  again before they use them), FlashRAM's 128 KB when the cartridge has it, the IS-Viewer buffer, the recompiled
  code's boot flag. A state of one RDP back end loads on the other with its DPC registers only (the drawing state
  starts as after a reset: the game sets it again in its next display list). The upscaled copy of RDRAM (XBA-185) is
  derived and never saved: paraLLEl-RDP takes the 1x pixels where RDRAM changed, so what the game draws again is
  upscaled again, a picture it never redraws shows at 1x until then. On load every RDRAM page's write counter is
  bumped and the dispatch cache flushed, as on the PS1; the VI's picture path (`n64_vi.c`) starts afresh (an
  interlaced game's first field after a load is shown alone). The save files are never written by a state: the
  runtime writes the cartridge and paks once the game does.
- The shared interface, `src/platform/recomp_states.h` (in `xbr_host_core`): a runtime attaches itself while it
  plays (console, state folder, a save and a load callback that run under its frame lock), the in-game menu and the
  desktop keys call `recomp_states_save / _load / _info (slot)`. Slots: `<save dir>/<console>-<KEY>/state1.st` ..
  `state4.st`, beside the saves: a header (magic `XRSLOT`, version, console, key, the time saved, the frames run), a
  thumbnail of the last frame (RGB565, at most 160 x 120, at the console's display aspect), then the core's blob;
  written through `.tmp` + rename. About 0.6 MB for the GBA, 4.1 MB for the PS1, 1 to 10 MB for the N64. No save
  dir, no states.
- The automatic slot, `auto.st` (slot `RECOMP_STATE_AUTO` = 0), for « resume where I left off »: with
  `RECOMP_STATE_RESUME=1` the runtime loads it before its first frame (`recomp_states_resume`; an empty slot starts
  the game as usual, a refused one too) and saves it when the player closes the window. On Android the app sets the
  variable and the in-game menu saves the slot when the player leaves (Quit, Back to launcher, the background).
- Quick save / load: on a pad, **Back + right shoulder** saves and **Back + left shoulder** loads the quick slot
  (the last numbered slot saved or loaded this run; at the start, the most recently saved one, else slot 1). Back and
  the shoulders then reach the game as nothing. `recomp_states_last_event` tells the UI what the last quick save /
  load / resume / quick slot change did (the Android menu shows « Saved to slot N », « Quick slot: N »).
- Fast-forward (`recomp_ff_*`): a toggle (the menu's « Fast-forward », or a hotkey) and a hold (Tab on a desktop; on a
  pad, Back + right trigger held, whose two controls then reach the game as nothing); the speed is `RECOMP_FF_SPEED` (2..16
  times the console's rate, default 4) or `max` (as many frames as fit in one display frame, silent). The runtime
  runs `speed` frames per shown frame and averages their audio down by the same factor (higher pitched, never a gap:
  the audio queue keeps its length, so pacing stays audio-driven).

- Hotkeys (XBA-190, `src/platform/recomp_hotkeys.h`, in `xbr_host_core`): the chords above are the defaults of a
  map the player can change. Actions: `ff_hold`, `ff_toggle`, `qsave`, `qload`, `slot_next` / `slot_prev` (the quick
  slot + / - 1, around), `menu` (the Android shell's, `MenuTriggers`), `save1..4` / `load1..4`; each takes a pad chord
  (`RECOMP_HOTKEYS`: a modifier then a button, a button alone, or none) and a desktop key (`RECOMP_HOTKEYS_KEYS`:
  `[ctrl+][alt+][shift+]key`), parsed once in `recomp_hotkeys_get`
  ([environment-variables.md](../docs/runtime/environment-variables.md)). Each frame a runtime hands every physical
  pad's sources (`console_pad_sources`, triggers past half) to `recomp_hotkeys_pad_update` (what is held, what went
  down, the sources to hide from the game) and the SDL keyboard to `recomp_hotkeys_keys_update`, runs what went down
  with `recomp_hotkeys_run` and holds fast-forward while `ff_hold` is held. A two-button chord fires once when both
  are down (either order); a button-alone chord waits while a two-button chord holds its button; the menu chord is
  never fired or swallowed by a runtime (the Android shell opens the menu and the game keeps the buttons). The
  Android app edits them per console and per game ([app-shell.md « Hotkeys »](../docs/android/app-shell.md#hotkeys)).
  No screenshot hotkey: the menu's Screenshot is the shell's (the desktop players have `--screenshot`).

Desktop: `gba_play` / `ps1_play` / `n64_play` by default F1-F4 save slots 1-4, Shift+F1-F4 load them, Tab held
fast-forwards (`RECOMP_HOTKEYS_KEYS` remaps them), the pad chords as above (`RECOMP_HOTKEYS`). Headless, `gba_run` / `ps1_run` / `n64_run --state-save N:FILE` writes a state after frame N,
`--state-load FILE` starts from one (exit 5 when the core refuses it), `--run-hash` prints one hash of every frame's
picture and audio after the save point. Tests: `tests/recomp_states` (slot files, refusals, thumbnails, the attach /
save / load path, the automatic and quick slots, the chords, the events, fast-forward speed and audio; the hotkeys:
both text forms and their round trip, remapped pad chords and keys frame by frame, what they run); the round trip
on every homebrew (`gba_state_hb_*`, `ps1_state_hb_*`, `ps1_state_hle_hb_*`: save after 120 / 150 frames, hash the
next 180 / 150, then the same from the state in a new machine; a run from power-on must differ;
`consoles/{gba,ps1}/tests/state_*.sh`), the same with each program's pack (`gba_rc_state_hb_*`, `ps1_rc_state_hb_*`:
the state of a recompiled run loads in the interpreter and the other way round, same hash, and a recompiled run from a
state stays in lockstep), a disc read in progress (`ps1_state_hb_disc_overlay`), refusals (`gba_state_wrong_game`,
`ps1_state_wrong_game`, `ps1_state_wrong_bios`); the runtimes' tests save and load through `recomp_states.h` while the
game runs on its own thread (`gba_runtime`, `ps1_runtime`). N64: `n64_io` (a machine with something in every part
saved, loaded into a fresh one and compared, the same run after; refusals: version, magic, truncated, another ROM, a 4
MB machine), `n64_state_hb_*` (every homebrew ROM, `consoles/n64/tests/state_rom.sh`; also loaded with 2 and 4 RDP
threads), `n64_rc_state_hb_*` (with each ROM's pack), `n64_state_vulkan_gfx` (the Vulkan RDP's round trip, states
crossing back ends, upscaled 2x: the same machine after the load), `n64_state_wrong_game`, and `n64_runtime` (slots,
the quick slot and fast-forward while gfx.z64 runs, the automatic slot resumed, another game's refused). The Android
in-game menu: [app-shell.md « In-game menu »](../docs/android/app-shell.md#in-game-menu).

`n64_runtime.c` attaches as `"n64"` with `<save dir>/n64-<KEY>`, runs its frames through the same fast-forward loop
(no frame skip while fast-forwarding), reads the hotkeys from every host pad, and
the in-game menu shows the rows for an N64 game (`Console.hasSaveStates`).

**Rewind (XBA-194).** Hold the rewind hotkey (pad: **Back + left trigger**; desktop: **R**, not Backspace, the GBA's
Select; `rewind` in `RECOMP_HOTKEYS` / `_KEYS`) and the game goes back in time, one capture per shown frame, silent;
let go and it plays on from there. `src/platform/recomp_rewind.h` (in `xbr_host_core`):

- The ring: the runtime captures its machine (the save-state blob above; the GBA adds the frame's picture, which its
  state does not hold; the PS1 and N64 pictures come back from VRAM / RDRAM) every `RECOMP_REWIND_FRAMES` frames.
  The newest capture is held whole; each older one is the XOR with the capture after it, coded as runs of zero bytes
  skipped and literal bytes kept (no dependency, a few dozen lines). Stepping back decodes one delta onto the newest,
  so a step costs the same at any depth, and the oldest capture goes for free when the memory cap
  (`RECOMP_REWIND_MB`, default 64; it counts the deltas, the newest capture and the coding buffer) is reached: these
  backward deltas need no keyframe. Captures of different sizes (the N64 leaves zero RDRAM pages out) code the same
  way.
- The runtimes (`recomp_rewinder`): a capture after a console frame when one is due, at most one per shown frame (so
  fast-forward never captures more than once per display frame); while the hotkey is held, no console frame runs,
  the newest capture is loaded (the same post-load path as a slot: queued audio dropped, the save chip / cards / paks
  the state brought back reach storage only once the game writes them) and dropped unless it is the last; the frame
  generation history starts again on each step (`recomp_states_note_load`). The cost is logged at the end of a run
  (`<console>: rewind: N captures, x ms each ...`).
- Defaults: GBA on, a capture every 2 frames; PS1 on, every 3; N64 **off** (every 4 when `RECOMP_REWIND=1`). Capture
  cost measured on our homebrew (x86-64 host under load, `--rewind`): GBA 0.06 ms (639 KB raw, about 5 KB a capture
  held); PS1 0.5 ms (4 MB raw, about 40 KB a capture: 64 MB keep minutes of a static scene, tens of seconds of a busy
  one); N64 1.4 ms on the homebrew, 2.0 ms average with a full 8 MB RDRAM and up to 10 ms peaks, before any Vulkan RDP
  drain (each capture synchronises the GPU): off by default, a game can turn it on (its settings). The N64 software
  RDP's hidden-bit packing in `n64_rdp_state_save_sw` went from 6 ms to under 1 ms for this (the format is unchanged).

Headless: `gba_run` / `ps1_run` / `n64_run --rewind C:R [--rewind-mb M]` (`recomp_rewind_check`): `--frames` frames
with pseudo-random pads, a capture every C frames through the runtimes' `recomp_rewinder`, R steps back; the picture
must be the one recorded after that capture's frame and every frame after it (picture and audio) the first run's;
prints `REWIND OK` with the capture cost (exit 6 on a difference). Tests: `tests/recomp_rewind` (the codec with sizes
that change and corrupt deltas, the ring's round trips, the memory cap and the oldest dropped, the settings, the
capture schedule under fast-forward, the steps, the check on a fake machine, a load that is not exact caught),
`gba_rewind_hb_*`, `gba_rewind_cap` (a 2 MB cap), `ps1_rewind_hb_*` (with a disc read, `overlay`), `ps1_rewind_cap`,
`n64_rewind_hb_*`; the hotkey's default and text form in `tests/recomp_states`.

**Legal**: no ROM, disc image, BIOS or game data in this repository, its CI or its artifacts. Tests use homebrew we
build ourselves (devkitARM / libtonc, PSn00bSDK, libdragon) and dumps Alex made from his own media, kept outside every
repository.
