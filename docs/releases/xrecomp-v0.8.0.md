# XRecomp 0.8.0

N64 games get steadier at 4x, a sharper picture on screen, an opt-in 8x, and no more black screen while shaders
compile. A GBA, PS1 or N64 game can now become an app of its own. Duke Nukem: Time to Kill (PS1) no longer freezes.

## N64: steadier frames

Measured on an AYN Thor, Ocarina of Time PAL, PurpleVK Turnip driver, upscale 4x:

| | 0.7.0 | 0.8.0 |
|---|---|---|
| Frames shown per second | 50 | 50 |
| Frames over 20 ms per 10 s | ~166 of 500 | ~70-84 of 500 |
| Slowest frame | 40-76 ms | 27-39 ms |
| Time the emulation waits on a full renderer queue | ~5 ms per frame | 0 |
| Spare time per frame | ~4 ms | ~10 ms |

- **The renderer's work leaves the busy frame.** When a game draws one picture every two or three screen refreshes,
  the end of its drawing is reported to the game at the next refresh instead of at once, so the emulation no longer
  waits for the GPU in the frame that already does the most work. The game sees the same timing it can tolerate on
  hardware; it backs off by itself if a game would lose a refresh. `RECOMP_N64_RDP_DEFER=0` turns it off.
- **A 1 MB renderer command queue** (16 KB before): queuing a picture never waits for the GPU.
- **One copy from the GPU to the screen** instead of five; the picture is read back while the GPU is idle.
  `RECOMP_N64_PRESENT_DIRECT=0` restores the old path.

## A sharper picture

- **Shrunk pictures are averaged.** At 4x the N64 picture is 2560 pixels wide and the screen shows it about 1440 wide.
  Each screen pixel now averages the pixels it covers instead of picking one, so text, the HUD and distant edges
  shimmer much less, with the Smooth or the Sharp filter. Every console benefits when its picture is shrunk.
  `RECOMP_PRESENT_DOWNSCALE=0` restores single sampling.
- **Two new picture shaders**, also switchable live from the in-game menu: **Sharpen (CAS)** with a strength, and
  **Edge smoothing (FXAA)**.

## N64: upscale 8x (opt-in)

- 8x now starts on Turnip drivers that have Mesa's SSBO emulation (PurpleVK does): the app enables it by itself for
  8x only, never for 1x-4x, with its own shader cache. It uses about 2 GB of GPU memory, down from 4.1 GB, which
  got the app killed for memory in a first try.
- **It is slow:** Ocarina of Time runs at about 40 % speed at 8x on the Thor. After 10 s below full speed the log
  advises 4x. Useful for screenshots, not for playing.
- Every renderer buffer is now checked against the driver's limits before start; one that does not fit lowers the
  factor and the log names it. At 4x this also keeps a buffer that used to exceed Turnip's 128 MB limit within it.

## N64: shaders without a black screen

- **« Compiling shaders… 7/13 »** with a bar (or a spinner on the first start) while an N64 game's renderer
  compiles at start. A start from the cache shows nothing.
- **Compiled ahead of time**, in the background and never next to a game: at the end of an N64 Prepare, a few
  seconds after you change the Vulkan driver or the internal resolution, and after an app update. « Shaders ready »
  when it compiled something.
- A driver whose start-up check fails no longer loses the shaders it did compile.

## PS1

- **Duke Nukem: Time to Kill** no longer freezes at « CHECKING MEMORY CARD ». The memory card's acknowledge signal
  now lasts a few microseconds, as on hardware, instead of until the next byte. A save state taken while frozen
  resumes (it may ask to continue without a card once).
- A game stuck with interrupts off now says so in the log (`ps1: the game has not left pc …`).

## Library and Home

- **Create an app for a GBA, PS1 or N64 game** (Options › Create an app): an app of its own with the game's files and
  its pack inside. It starts straight into the game, with no XRecomp, no file access and no Prepare.
- A game's Home offers **Resume** (where you left off) and **Start** (from the boot) when it has a saved point.
- A GBA, PS1 or N64 game's Home and its Preparing screen show the game's cover, as its library card does.
- The launcher fits 4:3 and square screens (single-screen handhelds): the art stacks above the title, the library
  has fewer columns, and lists keep their description column.
- A focused row's first letter is no longer cut.

## Diagnostics

- `n64: perf detail` adds the deferred renderer completions, early read-backs and the time waited at their deadline.
- The Vulkan renderer's start line lists the driver's storage range, buffer and allocation limits and the largest
  memory heap; a start logs how many shaders it compiled and how long they took.

## Known problems

- Upscale 8x runs at about 40 % speed in Ocarina of Time on the Thor; play at 4x.
- At 4x, Ocarina of Time still has about one busy frame in six over 20 ms (up to ~35 ms), now spent waiting for the
  picture before it is read back; it does not show on screen.
- Text and 2D art (texture rectangles) are drawn at 1x when upscaling; `RECOMP_N64_UPSCALE_NATIVE_TEXRECT=0` in
  `recomp.cfg` upscales them too (may show seams in some games). Low-resolution game textures stay blocky.
- From 0.7.0: the N64's record of code found while playing grows by MBs per session; Prepare again from time to time.
- From 0.5.0: N64 rewind costs up to a few ms per capture, hence off by default; no save states, rewind or
  fast-forward for Xbox titles.
- From 0.4.0: N64 on Qualcomm's own driver uses the software renderer; N64 upscaling needs a Turnip driver
  (PurpleVK or another with 8-bit storage).

## Before you install

- Updates 0.7.0 and older, and `xrecomp-latest`, in place (same signing key).
- Games prepared with 0.7.0 stay prepared; save states made with 0.7.0 still load. The first N64 start after the
  update compiles shaders once (with the progress line), unless the background warm-up got there first.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, the Vulkan driver for N64, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach game files.
