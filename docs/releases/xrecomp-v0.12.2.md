# XRecomp 0.12.2

XRecomp now updates itself. When a new version is out, the app says so when it starts and installs it in a few taps:
no more looking for the APK on GitHub. This is the last update you install by hand.

## Android app

- **Updates in the app.** When XRecomp starts (at most once an hour), it asks GitHub whether a newer XRecomp is out.
  If one is, a window shows the new version, yours, its size and what changed, with **Install now**, **Later** and
  **Skip this version**. Install now downloads it, checks it (its size and checksum as published, the same app,
  a newer version, signed with the same key) and hands it to Android's installer. Android asks you to confirm the
  first time; from Android 12 on, the updates after that install without asking. Your games, saves, packs and
  settings are kept.
- **Settings › General › Updates**: install the update, **Check now** (Y), **Check at launch** (on by default; off,
  XRecomp asks GitHub only when you press Check now) and the **Channel**: **Releases** (the tagged versions, the
  default for this download) or **Test builds** (also `xrecomp-latest`, built from every change: newer, less tested).
- What is sent: one request to GitHub's public API per check, with no information about you or your device. A game's
  own app never checks or updates (update XRecomp, then create the game's app again from Manage).
- An update cannot be installed while a game or a Prepare is running (the update closes the app): the window says so.

## Known problems

- Versions before this one cannot update themselves: install 0.12.2 by hand once (as below).
- Unchanged from 0.12.1: about one Xbox boot in ten of SpikeOut: Battle Street stays on a black screen after the logos
  (start it again); no Xbox title has been resumed after a restart on a device yet; the Windows download has never
  been tried on a Windows PC by us; Xbox rewind often switches itself off on real games with the default budget.

## Before you install

- Updates 0.12.1 and older, and `xrecomp-latest`, in place (same signing key). Saves, states and settings are kept.
  Nothing to prepare again.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, and the log from « Share log » in the in-game menu ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach
  game files.
