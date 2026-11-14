# XRecomp 0.3.0

The first Nintendo 64 commercial game we have played through its opening on a phone: **The Legend of Zelda: Ocarina
of Time** (PAL) runs at full speed on an AYN Thor (Adreno 740) with a custom Vulkan driver. The consoles also get the
same options as the Xbox, save states for GBA and PS1, and the PlayStation no longer needs any BIOS image.

## What is new

- **Custom Vulkan drivers** (Settings › Graphics › Vulkan driver, N64 renderer for now). Qualcomm's own driver cannot
  compile paraLLEl-RDP's shaders on Adreno 7xx; a Mesa **Turnip** driver can. The Drivers screen lists the Turnip
  releases of [purple-turnip](https://github.com/MrPurple666/purple-turnip) and
  [K11MCH1/AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers) (Turnip packages only), downloads one
  (checked against GitHub's SHA-256), or imports an AdrenoTools `.zip`; drivers are loaded with libadrenotools
  (BSD-2). Never part of the app: you choose and download them.
  - A default in Settings, and a choice per game (a game's Options, or the in-game menu for the next start).
  - Fallback chain: the chosen driver, then the system driver, then the software renderer, each logged. A driver
    that crashed is skipped at the next start; one that loads but cannot run the renderer is marked « Not
    compatible with the N64 renderer ».
  - A game's own app (« Create an app ») follows XRecomp's choice. This is the only reason XRecomp now asks for the
    INTERNET permission; game apps do not get it.
- **Options aligned across consoles**: Settings › Graphics and Audio are grouped « All consoles » (vertical sync,
  scale, picture filter, audio buffer, controller rumble) then « Xbox only ». GBA, PS1 and N64 games get Scale and
  Filter per game (their Options, or the in-game menu), and the in-game menu's volume, FPS overlay and screenshot now
  work for them. A console game played after an Xbox game no longer shows or applies the Xbox game's settings.
- **Save states and fast-forward for GBA and PS1**: four slots per game with a thumbnail (in-game menu Save state /
  Load state; desktop F1–F4, Shift+F1–F4), fast-forward toggle or hold (`RECOMP_FF_SPEED`, default 4×).
- **PlayStation without a BIOS image**: a high-level BIOS in C is now the default when no BIOS file is found;
  OpenBIOS stays available.
- **Xbox multi-disc-image games prepare faster**: the first Prepare builds `default.xbe` only; another image is
  prepared the first time the game launches it.

## What is fixed

- **N64, CIC-6105 games (Ocarina of Time)**: our boot left the RSP in a state the game's anti-piracy check rejects;
  the game then corrupted its own memory at random and froze during the intro (with a small yellow bar, its crash
  screen, at the top left). The boot now leaves what a console's IPL leaves, and the check passes.
- **N64 picture edges**: the rightmost column showed un-faded pixels (a bright line on dark scenes). The VI now
  blanks the edges as the hardware does, and the window crops them (`RECOMP_N64_CROP_EDGES=0` shows them).
- **N64 diagnostics**: a guest crash or stall is now logged (`n64: guest fault …`, `n64: guest stuck …`).

## What works

- **Ocarina of Time** (PAL, Rev 1) on an AYN Thor with purple-turnip T30: intro, Link's house, Kokiri Forest at
  50 fps (PAL full speed), no frame skipped once running.
- Xbox, GBA, PS1: as in 0.2.0; GBA homebrew is now also checked as recompiled packs with screenshots in CI.

## Known problems

- N64 on Qualcomm's own driver: no Vulkan renderer, so the software renderer, which is too slow for 3D on a phone and
  draws some 3D scenes with visible triangle edges. Use a Turnip driver.
- K11MCH1's « Turnip v26.0.0 R8 » loads but lacks what the N64 renderer needs (« Not compatible »).
- CIC-6105: one RSP register is set to a value derived from the game's check, not measured on a console; only Ocarina
  of Time has been tried.
- The touch layouts cannot be edited yet; the N64 touch pad has no D-pad.

## Before you install

- **Bring your own games.** XRecomp contains no game and no BIOS dump. Use only games you dumped yourself.
- Updates 0.2.0, 0.1.0 and `xrecomp-latest` in place (same signing key). Prepared games stay prepared.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, the Vulkan driver for N64, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach game files.
