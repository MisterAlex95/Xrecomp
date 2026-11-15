# XRecomp 0.5.0

Every setting per game, rewind and cheats on the consoles, and much faster interpreters. GBA, PS1 and N64 games get
closer to the hardware (measured by the open test suites now run in CI), and Prepare redoes far less work when the
self-healing loop finds new code.

## What is new

- **Settings per game, for everything** (Xbox and consoles): any value in Settings can be « As in Settings » or the
  game's own, in one « Game settings » screen reachable from the game's Home and the in-game menu, with « Reset to
  Settings ». Xbox controls can be mapped per game too. The older per-game choices (language, 16:9, scale, filter,
  internal resolution, mapping, N64 pak) are moved over on first use.
- **Rewind** (GBA, PS1, N64): hold **Select + L2** (keyboard: R) to go back in time. On by default for GBA and PS1,
  off by default for N64 (turn it on per game); memory cap in Settings › General.
- **Save states and fast-forward on N64**, as on GBA and PS1: four slots, « Where you left off », quick save / load
  on the pad.
- **Configurable hotkeys** (Controls › Hotkeys): fast-forward, quick save / load, next / previous slot, slots 1–4,
  the game menu; per console or per game, with a conflict warning. Desktop keys too (`RECOMP_HOTKEYS_KEYS`).
- **Cheats** (GBA, PS1, N64): GameShark / Action Replay / CodeBreaker codes, toggled from the in-game menu « Cheats »
  page, applied without a restart. Cheats are never stored in save states.
- **Picture shaders**: sharp bilinear, scanlines, CRT, LCD grid (GBA / PS1 / N64) for the consoles and the Xbox
  (Settings › Graphics, and per game).
- **Library**: sort (name, last played, console, most played), console tabs (L1 / R1), search, favourites (X), a
  « Recently played » row, and play time that counts only time on screen, for every console.
- **Touch layout editor**: move, resize and hide on-screen controls, per console or per game (Settings › Controls,
  or the in-game menu).
- **Frame generation in every build**: the published APKs and desktop packages now include it (bring your own
  Lossless.dll, as before).
- **Xbox movies**: a portable XMV decoder (WMV2 video, Xbox ADPCM sound) is bound at Prepare when the title's movie
  player is recognised, with no per-title code. A movie it can't decode exactly is left to the title's own player,
  and a watchdog takes over when that player stalls. `RECOMP_XMV=native` turns it off.

## Faster

- **Interpreters**: PS1 1.7–2.2×, N64 2–2.5×, GBA 1.3–1.9× on interpreter-bound code, byte-identical to the
  previous ones (checked side by side over every test program and thousands of random ones).
- **Lifted Xbox code**: indirect calls about 39 % faster, memory fills 80 % faster, SSE-heavy code about 30 % faster.
- **Prepare**: the lift is about 20 % faster; a re-Prepare after the self-healing loop finds a new function now
  recompiles a few files instead of the whole title; more compile jobs on phones with enough memory.

## Closer to the hardware

- **GBA**: the mGBA test suite goes from 4245 to 6998 of 7005 (cartridge prefetch buffer, timers and interrupts,
  open bus, BIOS timing and math, DMA, serial port, long multiply carry), plus jsmolka's tests and the Cult-of-GBA
  BIOS in CI.
- **N64**: n64-systemtest 3716 of 3721 (3726 of 3731 with every group), libdragon's test ROM 102 of 107, krom's tests
  204 of 323 (built from source with an open boot code); fixes in the CPU, the CACHE instruction, PI DMA, the
  software RDP and the RSP.
- **PS1**: the JaCzekanski ps1-tests run in CI; GPU lines, triangles, texture flips and overflow, the CLUT cache and
  UV interpolation now match hardware dumps pixel for pixel; the MDEC no longer hangs.
- **Library**: PS1 `.chd` discs (and other files) that an older version failed to read are read again after an
  update; discs whose track 1 has a pregap are read correctly.
- **arm64**: trapped SIMD loads write the SVE registers too (devices with SVE, such as the AYN Thor).

## Known problems

- The MDEC's output can be one unit off from the hardware, and its FIFO timing isn't modelled.
- XMV « X8 » key frames, WMA and 5.1 movie sound are not decoded (those movies stay with the title's own player).
- N64 rewind costs up to a few ms per capture, hence off by default.
- No save states, rewind or fast-forward for Xbox titles.
- From 0.4.0: N64 on Qualcomm's own driver uses the slow software renderer; N64 upscaling needs a Turnip driver.

## Before you install

- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Updates 0.4.0 and older, and `xrecomp-latest`, in place (same signing key).
- **Prepare your games again.** The toolkit changed, so Xbox games show « stale (toolkit) » once, and GBA packs from
  before are refused until prepared again.
- **Save states**: GBA states from before this version are refused (the machine model changed); PS1 states still
  load. In-game saves, memory cards and paks are not affected.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, the Vulkan driver for N64, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach game files.
