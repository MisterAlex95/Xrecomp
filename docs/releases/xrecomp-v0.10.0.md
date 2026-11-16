# XRecomp 0.10.0

Platform parity: Xbox games catch up with the consoles. Xbox games can render at 2x or 4x, fast-forward, take cheat
codes and always get a cover. Every platform logs the same performance line, and the Mac launcher gets frame
generation and per-game settings.

## Xbox games, like the consoles

- **Internal resolution** (Settings › Graphics › Internal resolution, « All consoles », also per game and in the
  in-game menu): Xbox games with the GPU renderer now follow it, at 2x or 4x (8x draws at 4x, as on PS1). The Xbox
  « Resolution » row still wins when it is not Auto. What the game reads back from the GPU stays at its own size.
- **Fast-forward**: the in-game menu toggle, the Select + R2 hold and the on / off hotkey now work for Xbox titles.
  The game's clock runs faster, at `RECOMP_FF_SPEED` in `recomp.cfg` (default 4x, `max` = 16x), muted while it runs.
- **Cheats**: the Cheats screen (Game settings › This game, and the in-game menu) takes codes for Xbox titles, in
  XRecomp's own format: 8 / 16 / 32-bit writes, « if equal » tests and plain `ADDRESS:VALUE` lines
  (`consoles/README.md`). They are applied every frame, only to the game's RAM.
- **Covers**: an Xbox game without a title image in its executable now shows its save image instead of a letter, in
  the library and on its Home (Android and macOS).

## All platforms

- **One performance line**: GBA, PS1 and Xbox now log a `perf` line every 10 s and a summary when the game closes,
  like `n64: perf`. Every line ends with the mean and 99th-percentile frame time. The performance overlay shows the
  same fields on all four platforms, and is now drawn for GBA, PS1 and N64 games too.
- **Profiling on Android** (developers): `tools/android/profile.sh 30` records a simpleperf flame graph of the running
  game, with lifted functions named by their guest address ([profiling](../android/profiling.md)). Needs the
  debuggable `xrecomp-latest` build.

## Mac launcher

- **Frame generation** with your own Lossless.dll, translated in the app (Settings › Graphics). Needs MoltenVK
  (`brew install molten-vk vulkan-loader`), which Setup now lists as optional.
- **Per-game settings** that follow Default settings or have the game's own value, with « Reset to Default
  settings ».
- New Xbox settings: widescreen, 480p, controller rumble, picture shader.

## Not on every platform, and why

- **Xbox save states, rewind and « Resume where I left off »**: not yet. Recompiled Xbox code keeps part of its state
  on the host's own stacks, so a console-style snapshot does not exist. A design for titles whose threads all reach
  the kernel regularly is in [xbox-save-states.md](../technical/xbox-save-states.md); its first parts (capture of
  memory, threads, kernel objects and devices, no restore yet) are in this release and do nothing visible.
- **Hotkeys in the Mac launcher**: the desktop Xbox runtime has none yet; console games do not run from the Mac app.
- **Internal resolution with the Xbox software renderer**: none; it applies to the GPU renderer.

## Known problems

- Fast-forward cannot speed up an Xbox game that paces itself on its sound buffers; Xbox movies play at normal speed
  during fast-forward.
- Xbox cheat codes on heap addresses may only work for one run (addresses can move between runs); the app has no
  memory search.
- With 4x or 8x set for PS1 / N64 in Settings, Xbox games now draw at 4x too, which is heavy on a handheld: set the
  Xbox game's own Internal resolution if it runs slowly.
- The Mac launcher's new settings were built without a Mac at hand: tell us if `macos/scripts/check.sh` fails.
- From 0.8.0: upscale 8x runs at about 40 % speed in Ocarina of Time on the Thor; N64 rewind costs a few ms per
  capture, hence off by default.

## Before you install

- Updates 0.9.0 and older, and `xrecomp-latest`, in place (same signing key).
- Games prepared with 0.9.0 stay prepared; if the library says « Prepared · out of date », Prepare again. Save states
  and per-game settings are kept.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, the Vulkan driver for N64, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach game files.
