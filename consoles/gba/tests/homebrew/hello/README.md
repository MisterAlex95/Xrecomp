# gba/hello

Prints `XBA HELLO GBA` and `homebrew corpus XBA-149` with libtonc's tile text engine (mode 0, BG0, charblock 0,
screenblock 31), then writes its verdict.

## Result protocol

Block at `0x0203FE00` (see [../README.md](../README.md)), `test_id` = `HELO` (`0x48454C4F`). Halt loop after
`status` is written; `status` = PASS when every check holds.

| table | value | check |
|---|---|---|
| 0 | `0x12345678` | constant written by the program |
| 1 | `0xC0FFEE01` | an initialised `.data` word plus a `.bss` word: crt0 copied `.data` and cleared `.bss` |
| 2 | `0x00000100` | `REG_DISPCNT` read back (mode 0, BG0 on) |

The screen shows the two text lines in white on black at (8,8) and (8,24).
