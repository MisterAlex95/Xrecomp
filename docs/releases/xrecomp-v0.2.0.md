# XRecomp 0.2.0

XRecomp is no longer Xbox only: **Game Boy Advance, PlayStation and Nintendo 64** games are statically recompiled and
prepared on the device the same way, with an interpreter behind the native code for whatever the recompiler has not
found yet. **Experimental**: the three new consoles pass their homebrew and test suites but have not been tried on
many commercial games yet. Tell us what yours do.

## What is new

- **Three new consoles**, the Xbox model for each: Prepare lifts the game's code to C and compiles it on the device
  (no PC, a few minutes per game); the game always starts, prepared or not; code the recompiler missed runs in the
  interpreter, is recorded, and the next « Prepare again » makes it native.
  - **Game Boy Advance** (`.gba`): ARM / Thumb recompiler, picture, sound, saves (SRAM, Flash, EEPROM; `.sav` files
    compatible with other emulators), our own BIOS (no BIOS dump needed).
  - **PlayStation** (`.cue` / `.bin`, `.iso`, `.chd`, `.m3u`): MIPS recompiler that follows code loaded from the disc
    at run time (overlays), the open-source OpenBIOS (MIT) built in, so no Sony BIOS is needed; DualShock with
    analog and rumble, memory cards (`.mcr`), XA and CD audio, « Change disc » in the in-game menu for multi-disc
    games.
  - **Nintendo 64** (`.z64` / `.v64` / `.n64`): MIPS III recompiler that needs only the ROM (no symbol file),
    low-level RSP (every microcode), the RDP on the GPU through Vulkan (paraLLEl-RDP, MIT) with a software renderer as
    fallback, controller / rumble pak, saves (`.eep` / `.sra` / `.fla` / `.mpk`, the files mupen64plus and ares use).
- **Library folders**: Options › Library folders takes several folders (one per console if you like); every game
  gets a card with its console's badge.
- **Controls for the new consoles**: remap per console or per game (a game's Home › Controls), and an on-screen
  touch pad with each console's layout.
- **Faster Prepare** for the new consoles: a typical GBA or PS1 game prepares in about 2–3 minutes on a recent
  phone (estimated, see below).

## What works

- Xbox: as in 0.1.0 (`profiles/titles/*/` says what each game does).
- GBA, PS1, N64: our own homebrew test programs and public test suites (jsmolka gba-tests, JaCzekanski ps1-tests,
  n64-systemtest) pass, recompiled and interpreted, with identical results. Commercial games: untested by us so far.
- Measured on a desktop; **not yet measured on a phone**: Prepare times, and N64 frame rate (the Vulkan RDP is the
  default on Android; `RECOMP_N64_RDP=software` in recomp.cfg switches to the software one to compare).

## Known problems

- N64: no cache emulation (games do not rely on it); EEPROM answers as 16K unless the ROM header says otherwise.
- PS1: `.m3u` files are used to find a game's other discs, not listed in the library themselves.
- The touch layouts cannot be edited yet; the N64 touch pad has no D-pad.

## Before you install

- **Bring your own games.** XRecomp contains no game and no BIOS dump. Use only games you dumped yourself.
- Updates 0.1.0 and `xrecomp-latest` in place (same signing key). Prepared Xbox games stay prepared.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG` ([CONTRIBUTING.md](../../CONTRIBUTING.md)).
  Never attach game files.
