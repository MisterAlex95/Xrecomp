# gba/pad

Interactive check of the controls and of saves on a real host (`gba_play`, the Android app); not a headless test.

- The ten keys (A, B, L, R, START, SELECT, UP, DOWN, LEFT, RIGHT), one line each, shown as `> NAME` while held.
- A `*` moved by the D-pad (the left stick too: the hosts map it to the D-pad).
- A pressed counts up; the count lives in SRAM (byte 0 = `0x50`, bytes 1-2 = count), next to the `SRAM_V113`
  library string, so it survives a quit and a restart once the host writes the save.
- `F<n>`: the frame counter (it moves: the game runs).

## Result protocol

Block at `0x0203FE00` (see [../README.md](../README.md)), `test_id` = `PAD ` (`0x50414420`); `status` = PASS as soon
as the screen is set up (the program keeps running, so a headless run stops there). table[0] = `0x53` (`'S'`, the
first byte of the library string).
