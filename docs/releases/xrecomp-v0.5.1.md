# XRecomp 0.5.1

A fix for Xbox games whose disc names the game's executable in capitals.

## Fixed

- **Prepare stopped with « no default.xbe »** on some Xbox discs read in place (seen on SpikeOut: Battle Street),
  although the library listed the game. Discs ignore the case of names and the app keeps each file's name as on the
  disc (`DEFAULT.XBE`, `Default.xbe`), but Prepare looked for `default.xbe` exactly. It now finds it whatever the
  case.

## Before you install

- Everything in the [0.5.0 notes](xrecomp-v0.5.0.md) applies (prepare your games again when coming from 0.4.0 or
  older; GBA save states from before 0.5.0 are refused).
- Updates 0.5.0 and older, and `xrecomp-latest`, in place (same signing key). Games prepared with 0.5.0 stay
  prepared.
- **Bring your own games.** XRecomp contains no game, no BIOS dump and no cover art.
