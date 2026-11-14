# XRecomp 0.1.0

The first public release of XRecomp: original Xbox games, statically recompiled to native code, prepared and played
on an Android device, a Mac or a Linux machine. **Experimental**: few games get far yet.

## What is in it

- **The XRecomp Android app** (this APK): a launcher with a gamepad UI, your game library (an extracted game folder
  or an ISO), and **Prepare on the device**: it analyses the game's executable, lifts it to C and compiles it with
  the embedded clang, with no PC (about 20–30 min, once per game). Then **Play**. Dual screen on devices that have
  two, in-game menu, saves kept across updates.
- **The toolkit** (source at this tag): `python3 -m tools.prepare` does the same on Linux and macOS, and the Mac
  launcher app (`macos/`) wraps it.
- **The runtime**: the Xbox kernel, D3D8 / NV2A on OpenGL and OpenGL ES 3.0, audio, input, from upstream
  [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), with this fork's host layer for Android, macOS and Linux.
- **Other consoles** (GBA, PS1, N64): early groundwork only, nothing runs yet.

## What works

What each game does is in `profiles/titles/*/` (`title.json` « status », `NOTES.md`). Tested first on an AYN Thor
(Snapdragon 8 Gen 2, Android 13). Expect crashes, and expect to prepare a game again after an update.

## Before you install

- **Bring your own disc.** XRecomp contains no game. Use only games you dumped from your own discs.
- **App id `fr.adserver.xrecomp.shell`.** A build installed before this release (application id
  `fr.adserver.xboxrecomp.*`) is a different app: it stays installed next to this one. Move your saves over by hand
  if you want them ([saves](../technical/saves.md)), then uninstall it.
- Bug reports: the repository's issues, with your device, Android version, the version from About, and
  `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG` ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach
  game files.
