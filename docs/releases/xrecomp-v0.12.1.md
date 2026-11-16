# XRecomp 0.12.1

Fixes for Xbox games: the pad now works in titles whose executable is large, and games that draw their text with the
XDK's bitmap fonts show it. Found on SpikeOut: Battle Street, which is now playable with the on-screen pad on a phone.
No need to prepare your games again for these fixes: they are in the app.

## Xbox

- **Pads in titles with a large executable.** The emulated USB stack's memory (its descriptors and transfer buffers)
  could land inside the game's own executable, and the pad never enumerated: no input at all, physical or on-screen,
  although the on-screen pad still clicked. Contiguous memory now keeps out of the executable, as on the console
  (`RECOMP_CONTIG_AVOID_IMAGE=0` brings the old placement back). SpikeOut: Battle Street and Darkwatch had it.
- **Menus without text.** Games that draw their text with the XDK's bitmap fonts (`.bmf`, XFONT) showed empty menus:
  the kernel did not answer two file queries XFONT makes before it reads a font, so every font was dropped. They are
  answered now, with the related volume queries.
- **The log says where a pad press stops**: the first press of the on-screen pad that reaches the game, the first
  report the game reads from its pad, and the first one with a button held (« Share log » in the in-game menu). A
  press that does nothing can be traced without changing a setting.

## Known problems

- About one Xbox boot in ten of SpikeOut: Battle Street (seen on Linux) stays on a black screen after the logos: the
  game's USB driver loops while retiring a transfer. Start the game again. We are on it.
- Your Xbox games show « Prepared · out of date »: the toolkit changed. Preparing them again is optional; the fixes
  above apply to the games you have prepared already.
- From 0.12.0: no Xbox title has been resumed after a restart on a device yet; the Windows download has never been
  tried on a Windows PC by us; Xbox rewind often switches itself off on real games with the default budget.

## Before you install

- Updates 0.12.0 and older, and `xrecomp-latest`, in place (same signing key). Saves, states and settings are kept.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, and the log from « Share log » in the in-game menu ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach
  game files.
