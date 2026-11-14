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
stub that logs `<console> runtime ready` for now (libmain.so links the three runtimes). Import filter (`Console.java`):
`.gba`; `.bin`, `.cue`, `.chd`; `.z64`, `.n64`, `.v64`; and `.iso`, which both an Xbox and a PS1 disc use, so its
content decides (Xbox volume magic `MICROSOFT*XBOX*MEDIA` first, then an ISO 9660 volume with system ID
`PLAYSTATION`). CHD is accepted by name but not read yet (no title key).

CMake options `XBR_CONSOLE_GBA`, `XBR_CONSOLE_PS1` and `XBR_CONSOLE_N64` are ON by default on Linux and macOS and OFF
on Android and Windows. A console is turned on for Android once it is ready.

**GBA core (XBA-152, XBA-153).** Interpreter first (decision of 2026-11-14, XBA-142); the static lifter comes later as an
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

**Legal**: no ROM, disc image, BIOS or game data in this repository, its CI or its artifacts. Tests use homebrew we
build ourselves (devkitARM / libtonc, PSn00bSDK, libdragon) and dumps Alex made from his own media, kept outside every
repository.
