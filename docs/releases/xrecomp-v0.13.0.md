# XRecomp 0.13.0

GameCube arrives in the app: homebrew and disc images, prepared on the device, with sound, memory cards, save states
and cheats. Your saves can now leave the device: export them to a zip and import them back, on any file location the
system picker offers. And a round of fixes from a review of the whole project, one of them for a freeze when leaving
an Xbox game.

## GameCube

- **Disc images and homebrew.** ISO / GCM, CISO, GCZ, RVZ and WIA images, and DOL programs. The disc boots through its
  own apploader, as the console's IPL would (no BIOS dump needed).
- **Prepare on the device.** A GameCube game's Home has **Prepare**, like the other consoles. Any image format is read
  by the app; the REL modules a game loads at run time are lifted at their load address.
- **Graphics** on OpenGL ES 3.0 (GX: TEV, indirect textures, EFB copies, fog), with internal resolution scaling. The
  shaders a game uses are compiled ahead after Prepare and kept, so they do not stutter the second time.
- **Sound**: the DSP, ARAM and the audio interface; the AX, ASND, Zelda / JAudio and AESND sound programs; disc
  streaming.
- **Memory cards** in slot A (and B if asked), one file per game in the save folder. They go into the saves zip like
  every other save.
- **Save states**, rewind, Resume where you left off, and **cheats** (Action Replay decrypted, Gecko, or
  `ADDRESS:VALUE`).
- This is a first version: we test it with homebrew and our own test programs. Expect commercial games to fail or
  run with glitches; reports are welcome.

## Android app

- **Export and import saves.** Storage › **Export all saves** / **Import saves**, and per game in Manage and the
  game's Options. The zip holds every console's saves; an import is checked before anything is replaced, and the
  saves it replaces are kept aside.
- **Which games resume where you left off**: a « Resume » tag on their card, a line on their Home, and the in-game
  setting always shown, with the reason when a game cannot (an Xbox game prepared before resume support: prepare it
  again).
- **Xbox: why it did not resume.** When an Xbox game's automatic save could not be taken, its Home says why
  (« Where you left off was not saved · … »).
- A folder copied in through the documents picker can no longer write outside its copy.

## Fixes

- **Xbox: leaving a game with « Resume where I left off » on no longer freezes the app.** The automatic save
  waited for itself.
- Save states larger than 64 MB (big Xbox titles) are saved and loaded instead of failing.
- Saving a state and closing the menu right away (or leaving and coming back to the app) no longer leaves the game
  paused for good.
- Xbox: the kernel's text formatting printed 64-bit numbers and decimals wrong (1.5 came out as a huge integer).
- Xbox (KOTOR): time frozen by the menu or a state capture no longer counts against the game's timeouts (the pad was
  lost after the first rewind capture); a timed wait serves the devices once before giving up.
- Xbox sound: an audio health line in every log (underruns, read latency), and `RECOMP_GUEST_CPUS=one` pins the game
  to the fastest core.
- Desktop Prepare: preparing a disc image twice without `--work` works; a split, compressed Xbox `.cso` is no longer
  taken for a GameCube disc.
- PS1 and GameCube image readers check two sizes read from the file before using them.

## Known problems

- **No cloud saves in this download**: the Google sign-in needs a client this build was made without (Storage ›
  Cloud saves says « Not in this build »). Export / import works.
- Xbox: Resume where you left off still does not show on device for the titles we tried: the new Home line gives the
  reason, which we need from you (« Share log » in the in-game menu, or a photo of the line).
- Unchanged from 0.12.2: about one Xbox boot in ten of SpikeOut: Battle Street stays on a black screen after the
  logos (start it again); the Windows download has never been tried on a Windows PC by us; Xbox rewind often switches
  itself off on real games with the default budget.

## Before you install

- From 0.12.2, the app offers this update itself. Older versions and `xrecomp-latest`: install it by hand, in place
  (same signing key). Saves, states and settings are kept. Nothing to prepare again, except Xbox games prepared
  before 0.12.2 that you want to resume (their Home says so).
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, and the log from « Share log » in the in-game menu ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach
  game files.
