# n64/hello

Initialises the VI at 320x240 RGBA16 (two buffers), fills the screen dark blue and draws `XBA HELLO N64` /
`homebrew corpus XBA-149` in white at (16,16) and (16,32) with libdragon's software text renderer.

## Result protocol

Block at physical `0x00300000` (see [../README.md](../README.md)), `test_id` = `HELO` (`0x48454C4F`). Halt loop
after `status`; PASS when every check holds.

| table | value | check |
|---|---|---|
| 0 | `0x12345678` | constant written by the program |
| 1 | `0xC0FFEE01` | an initialised `.data` word plus a `.bss` word: IPL3 loaded the data, `.bss` cleared |
| 2 | 0, 1 or 2 | `get_tv_type()`: 0 PAL, 1 NTSC, 2 MPAL (informative, always passes) |
