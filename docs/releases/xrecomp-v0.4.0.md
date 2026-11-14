# XRecomp 0.4.0

Sharper consoles and fewer menus. PS1 and N64 games can render at 2×, 4× or more, every console gets frame
generation, and GBA / PS1 games pick up where you left them, with quick saves on the pad. GBA, PS1 and N64 games
get cover art in the library.

## What is new

- **Internal resolution for PS1 and N64** (Settings › Graphics › Internal resolution, under « All consoles »:
  Native / 2× / 4× / 8×; also per game in its Options and in the in-game menu).
  - PS1: the software GPU draws polygons and rectangles again into a larger shadow VRAM. What the game reads back
    stays the same as at 1×, and save states keep their 0.3.0 size.
  - N64: paraLLEl-RDP's upscaling on the Vulkan renderer. The factor is lowered (and logged) when the GPU's limits
    call for it. The software renderer stays at 1×.
- **Frame generation for GBA, PS1 and N64** (LSFG, with your own Lossless.dll, as for the Xbox): generated frames
  take the place of the repeats of a 30 fps game. It is off during fast-forward, and a game's Options can turn it on
  or off for that game.
- **Resume where I left off** (GBA, PS1; in-game menu › Settings, on by default): leaving a game (Quit, Back to
  launcher, Home, the screen turning off) saves an automatic slot, and the next start loads it before the first
  frame. Load state lists it as « Where you left off ».
- **Quick save / load on the pad** (GBA, PS1): **Back + RB** saves and **Back + LB** loads the last slot used,
  without opening the menu. A message at the top of the screen says what happened. The desktop players get the same
  chords.
- **Cover art for GBA, PS1 and N64 games** (Settings › General › « Download cover art », on by default): after a
  library scan, the box art is looked up on [libretro-thumbnails](https://github.com/libretro-thumbnails) (the
  collection RetroArch uses), by the game's file name. Only that name is sent. With the option off, or when no cover
  is found, a frame of the game is kept as its cover after 45 seconds of play. Covers stay on your device.

## What works

- As in 0.3.0: Ocarina of Time (PAL) on an AYN Thor with a Turnip driver, Crash Bandicoot, Pokémon Pinball: Ruby &
  Sapphire, and the homebrew and test suites of each console in CI.

## Known problems

- Cover art is found only when the file is named after the No-Intro / Redump name of the game (« Crash Bandicoot
  (Europe) (EDC).cue »). Other names fall back to a captured frame.
- Resume and quick saves need save states, so Xbox and N64 games have neither yet.
- N64 upscaling needs the Vulkan renderer (a Turnip driver on Adreno). 8× is lowered to 4× on most phones.
- From 0.3.0: N64 on Qualcomm's own driver uses the slow software renderer; the touch layouts cannot be edited yet.

## Before you install

- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Updates 0.3.0 and older, and `xrecomp-latest`, in place (same signing key). Prepared games stay prepared, and 0.3.0
  save states still load.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, the Vulkan driver for N64, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach game files.
