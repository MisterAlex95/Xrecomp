# ps1/hello

Resets the GPU, clears a 320x240 display at VRAM (0,0) to dark blue and prints `XBA HELLO PS1` /
`homebrew corpus XBA-149` with PSn00bSDK's debug font (font texture at VRAM (960,0)).

## Result protocol

Block at `0x801F0000` (see [../README.md](../README.md)), `test_id` = `HELO` (`0x48454C4F`). Halt loop after
`status`; PASS when every check holds.

| table | value | check |
|---|---|---|
| 0 | `0x12345678` | constant written by the program |
| 1 | `0xC0FFEE01` | an initialised `.data` word plus a `.bss` word: the EXE's data loaded, `.bss` cleared |
| 2 | 0 or 1 | `GetVideoMode()`: 0 NTSC, 1 PAL (informative, always passes) |
