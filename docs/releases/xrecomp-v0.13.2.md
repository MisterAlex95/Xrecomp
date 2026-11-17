# XRecomp 0.13.2

A small release of fixes, and the first one cut by the new release PR (docs/releases/README.md).

## Xbox

- **Black screen at boot**: a game that scans its memory map region by region (Yu-Gi-Oh! The Dawn of Destiny) no
  longer loops for ever past the top of user space; the kernel now stops it there as the console does.
- **Save states**: capturing a state no longer fails now and then with « freeze failed » while every thread was
  in fact stopped.

**Prepare your Xbox games again** to get these fixes (their Home offers it).

## Mac

- The toolkit builds again with the Nintendo DS, PSP, Dreamcast and PS2 players (they could not find their core's
  headers).

## Known problems

- Unchanged from 0.13.1: no cloud saves in this download; Resume where you left off does not show yet on device for
  the Xbox titles we tried; about one boot in ten of SpikeOut: Battle Street stays on a black screen; the Windows
  download has never been tried on a Windows PC by us.

## Before you install

- 0.12.2 and later offer this update themselves. Older versions and `xrecomp-latest`: install it by hand, in place
  (same signing key). Saves, states and settings are kept.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, and the log from « Share log » in the in-game menu ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach
  game files.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### CI

- xrecomp-latest no longer waits for linux-arm64; the APK chain takes the heavy-job lock first (XBA-269) (04781f62)

### GameCube tests

- the helper scripts run cross-built binaries under the emulator (1e4a471c)

### Kernel bridge

- NtQueryVirtualMemory ends at the top of user space (e1486c87)

### Kernel park

- a freeze is reached on the read that saw every thread parked (faf84236)

### macOS

- the NDS / PSP / Dreamcast / PS2 players link their core, not only -force_load it (5f23f9df)

### Release

- a release PR picks the next version from commit trailers (4fc731ef)

### Tests

- kernel_park waits for the state it checks instead of sleeping a fixed time (14785cb3)

<!-- release:changes end -->
