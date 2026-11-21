# XRecomp 0.18.1

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

Fixes from the first Fold8 problem report on Adventure Time (Xbox 360).

- **Screenshots of GPU pictures**: « Report a problem › Add a screenshot », the companion panel's screenshot and the
  automatic cover shot had no frame for a game presented as a GPU texture (Xbox 360, GameCube with GX or GPU-YUV).
  The window now draws the picture it shows into a picture of its own when one is asked, also while the game is
  paused behind the menu, with the right colours (the Xbox 360 swizzle and float frames). F12 uses the same grab.
- **Frame generation on the Xbox 360** reads the picture as the window shows it (no red / blue swap, float frames
  readable).
- **Frame generation without Lossless.dll**: a game's « On » is dimmed in its Options with the reason, and the game
  starts without frame generation instead of logging « no shader cache »; the choice applies once the DLL is imported.
- **Logs**: console games (GBA, PS1, N64, GameCube, Xbox 360) no longer log « the disc image cannot be read »; one line
  says the game goes to its own runtime.

Known: not checked on a device yet. On the Fold8, Adventure Time still draws only the interface, not the 3D scene
(under investigation). No Prepare needed; save states from 0.18.0 load.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### Android

- frame generation without Lossless.dll says so, runs off (1e740625)

### host_window

- screenshots of a texture present, grabbed by the presenting thread (d3bb8404)
- frame generation reads a GPU picture as the window shows it (91558f0b)

### Shell

- route console games explicitly, no « disc image cannot be read » (d9eff647)

<!-- release:changes end -->
