# n64/cpu

Hand-written VR4300 (MIPS III) sequences in [`ops.S`](ops.S) (`.set noreorder`, 64-bit GPRs and FPRs as in
libdragon's `-mabi=o64`), run with interrupts disabled, then libgcc/inline 64-bit C. The interesting cases for a
recompiler:

- 32-bit ops (`LUI`, `ADDIU`, `SLL`) **sign-extend** their result into the 64-bit register;
- `DADDIU`/`DSUBU` wrap, `DSLL32`/`DSRA32`/`DSRL32`, `DSLLV`/`DSRAV` with the amount masked to 6 bits;
- `DMULT`/`DMULTU`/`DDIV`/`DDIVU`, and 32-bit `MULT` whose HI/LO are sign-extended;
- `LD`, `LW` vs `LWU`, unaligned big-endian `LDL`/`LDR`, `LWL`/`LWR`, `SDL`/`SDR`;
- **branch-likely** (`BEQL`/`BNEL`/`BC1FL`): the delay slot is nullified when the branch is not taken;
- FPU: `add.d`, `mul.d`, `div.d` with the FCSR inexact cause/flag bits, `sqrt.d`, `cvt.w.d` under the four
  rounding modes, `trunc`/`round`/`floor`/`ceil`, `cvt.d.l`, `trunc.l.d`, `mul.s`, `cvt.d.s`, `c.lt.d` + `bc1t`.

## Result protocol

Block at physical `0x00300000` (see [../README.md](../README.md)), `test_id` = `CPU ` (`0x43505520`),
`count` = 116, `checksum` = FNV-1a of the 116 table words. Each 64-bit result takes two words: high half first.
Halt loop after `status`; PASS when every word equals its expected value. Green screen on pass, red on fail.

| table | content | expected |
|---|---|---|
| 2i, 2i+1 (i = 0-55) | result `i` of `ops.S` (see its `# N` comments), high then low half | `expected[]` in [`main.c`](main.c) |
| 112-113 | `0x0123456789ABCDEF / 12345` (inline `DDIVU`), low then high word | `0x45F5207D`, `0x0000060A` |
| 114 | `-1000000000000 % 7` | `0xFFFFFFFF` (-1) |
| 115 | `-1000000000000 >> 40` (arithmetic) | `0xFFFFFFFF` (-1) |
