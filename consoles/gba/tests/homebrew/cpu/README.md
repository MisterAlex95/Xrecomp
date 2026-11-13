# gba/cpu

Hand-written ARM and Thumb sequences in [`ops.s`](ops.s), called from Thumb C through linker interworking veneers;
each sequence also calls a leaf in the other state with `BX` (ARM -> Thumb -> ARM and Thumb -> ARM -> Thumb).
Then BIOS SWIs (`Div`, `DivMod`, `Sqrt`) and libgcc division helpers.

Covered: barrel shifter (immediate and register amounts, >= 32, RRX, carry-out), NZCV after ADDS/SUBS, ADC/SBC/RSC
with carry in, MUL/MLA/UMULL/SMULL/UMLAL/SMLAL, LDRB/LDRSB/LDRH/LDRSH, unaligned LDR rotation (ARMv4), pre-index
writeback, STMIA/LDMDB with writeback, SWP, conditional execution (signed and unsigned), MSR/MRS flags, Thumb
shifts/ADC/SBC/NEG/MUL, register-offset loads, PUSH/POP, STMIA/LDMIA, conditional branches, high registers, logic ops,
`POP {r3}; BX r3` returns (ARMv4T `POP {PC}` does not interwork).

## Result protocol

Block at `0x0203FE00` (see [../README.md](../README.md)), `test_id` = `CPU ` (`0x43505520`), `count` = 76,
`checksum` = FNV-1a of the 76 table words. Halt loop after `status`; PASS when every entry equals its expected value
(`detail` names the first mismatch, 1-based). Backdrop green on pass, red on fail.

| table | content | expected |
|---|---|---|
| 0-44 | ARM results, in the order of the `@ N` comments in `ops.s` | `arm_expected[]` in [`main.c`](main.c) |
| 45-69 | Thumb results (Thumb index + 45) | `thumb_expected[]` in [`main.c`](main.c) |
| 70 | BIOS `Div(-100, 7)` | `0xFFFFFFF2` (-14) |
| 71 | BIOS `DivMod(-100, 7)` | `0xFFFFFFFE` (-2) |
| 72 | BIOS `Sqrt(1000000)` | `1000` |
| 73 | libgcc `-100 / 7` | `0xFFFFFFF2` |
| 74-75 | libgcc `0x0123456789ABCDEF / 12345`, low then high word | `0x45F5207D`, `0x0000060A` |
