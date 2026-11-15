# XRecomp 0.9.0

A game's settings get a new layout. Everything else is as in [0.8.0](xrecomp-v0.8.0.md).

## Game settings

- **Tabs, as in Settings:** General, Graphics, Audio and Advanced (those with settings for the game's console), then
  a last **This game** tab with its controls, cheats, theme, enhancements, reset, its own app and its stored files.
  L1 / R1 change tab; each tab keeps its focus.
- **What the game changes is easy to see:** the game's name is the title, each tab says how many of its values are
  the game's own (« Graphics · 2 »), those values are drawn in the accent colour, and the footer gives the total.
- The **Game settings** button on a game's Home (Xbox and consoles) shows that count in a bubble.
- Tabs that do not fit their bar are drawn smaller (French on a square screen).

## Known problems

As in [0.8.0](xrecomp-v0.8.0.md#known-problems).

## Before you install

- Updates 0.8.0 and older, and `xrecomp-latest`, in place (same signing key).
- Games prepared with 0.8.0 stay prepared; save states and per-game settings are kept.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art. Use only games you dumped
  yourself.
- Bug reports: the repository's issues, with your device, Android version, the version from About, the game's
  console, the Vulkan driver for N64, and `adb logcat -s xboxrecomp SDL AndroidRuntime DEBUG`
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)). Never attach game files.
