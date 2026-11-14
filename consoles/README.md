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
  `src/platform/HOST_LAYER.md`) and never the Xbox libraries. PS1 and N64 are empty for now (one version function);
  GBA has its core, see below.
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
on Android and Windows. A console is turned on for Android once it is ready: the GBA is (XBA-157).

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

**Legal**: no ROM, disc image, BIOS or game data in this repository, its CI or its artifacts. Tests use homebrew we
build ourselves (devkitARM / libtonc, PSn00bSDK, libdragon) and dumps Alex made from his own media, kept outside every
repository.
