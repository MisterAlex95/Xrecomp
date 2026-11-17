# XRecomp 0.13.1

A release of fixes: a review of the whole project found and fixed about a hundred problems, from wrong code in
recompiled Xbox games to leaks, races and slow paths.

**Prepare your Xbox games again** to get the recompiler fixes (their Home offers it).

## Xbox

- **Recompiled code**: signed multiplies (`imul`), bit tests (`bt`), rotates and shifts by a register count now
  produce the right result in every case; some of these could make a game take the wrong branch. Bit scans
  (`bsf` / `bsr`) and repeated string compares run faster.
- **Prepare** is faster: about 30 % less time in the recompiler, and a Prepare with nothing changed skips almost all
  of its compile step.
- **Files**: two threads reading one file no longer get each other's position (garbled streamed data); deleting a
  folder, creating or overwriting a save give the answers a game expects.
- **Kernel**: a long session no longer runs out of event or lock slots after about 4,000 of them; a log line is no
  longer written for every file read (`RECOMP_READ_LOG=1` brings them back).
- **Graphics**: loading a state no longer leaks up to 32 MB of GPU memory; the texture cache keeps the textures in use
  instead of dropping everything every so often (fewer hitches); textures decode much faster; movies upload in one go.
- **Sound**: DirectSound buffers filled through `Lock` play, and two sounds no longer share one voice; bad data from a
  game no longer stops the voice processor.
- **Resume where I left off**: holding Quit on the bottom screen now saves where you left off first.

## GameCube

- A dozen fixes from a first review: a buffer overflow when drawing points, a Gecko cheat reading past its code,
  C2 hooks left in the game after the cheats are removed, damaged save states and disc images refused cleanly, sound
  that could hang while loading a state, a memory card that failed to write retried 60 times a second.

## GBA, PS1, N64

- Save states are checked before they are loaded: a damaged one is refused instead of crashing the game. Two N64
  overflows are fixed, one of them reachable by a game's own FlashRAM use.

## Android app

- Cloud saves no longer replace the saves of a game started during a sync.
- A saves export survives Android closing the app while the file picker is open.
- A library file that cannot be read for a moment is no longer replaced by an empty library.
- Cancelling a Prepare stops the whole build; two Prepares started back to back no longer step on each other.
- « Share log » works after a GameCube disc Prepare. Pads with accented names show in the companion screen.

## Mac

- The launcher no longer crashes on a full disk; Manage no longer freezes the window; a queued Re-prepare is a full
  rebuild; compressed and split images (`.cso`, `.zso`, `Game.iso.1`) are listed; settings written by the command
  line are kept.

## Known problems

- No cloud saves in this download (Storage › Cloud saves says « Not in this build »). Export / import works.
- Xbox: paths the kernel does not recognise are now refused instead of passed through. If a feature of a game breaks,
  the log line `Refused Xbox path` or `Unrecognized Xbox path` says which; please report it.
- Unchanged from 0.13.0: Resume where you left off does not show yet on device for the Xbox titles we tried (the Home
  line says why: send it to us); about one boot in ten of SpikeOut: Battle Street stays on a black screen; the Windows
  download has never been tried on a Windows PC by us.

## Before you install

- 0.12.2 and later offer this update themselves. Older versions and `xrecomp-latest`: install it by hand, in place
  (same signing key). Saves, states and settings are kept.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, and the log from « Share log » in the in-game menu ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach
  game files.
