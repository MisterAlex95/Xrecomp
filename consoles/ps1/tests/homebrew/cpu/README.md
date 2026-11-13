# ps1/cpu

Hand-written R3000A sequences in [`ops.s`](ops.s) (`.set noreorder`: every delay slot is explicit), then libgcc
helpers. The interesting cases for a recompiler:

- branch delay slots, taken and not taken; `JAL` / `JALR` / `JR` delay slots;
- the **load delay slot**: the instruction after `LW` still reads the old register value (entry 3);
- `DIV`/`DIVU` written as `div $zero, rs, rt` so the assembler adds no trap: divide by zero (LO = -1 or 1 by sign,
  HI = dividend; unsigned LO = `0xFFFFFFFF`) and `0x80000000 / -1` (LO = `0x80000000`, HI = 0);
- `LWL`/`LWR` alone and as a back-to-back pair, `SWL`/`SWR` (little-endian merge rules);
- `BLTZAL` writes `$ra` even when not taken.

## Result protocol

Block at `0x801F0000` (see [../README.md](../README.md)), `test_id` = `CPU ` (`0x43505520`), `count` = 56,
`checksum` = FNV-1a of the 56 table words. Halt loop after `status`; PASS when every entry equals its expected value.
Green screen on pass, red on fail.

| table | content | expected |
|---|---|---|
| 0-50 | MIPS results, in the order of the `# N` comments in `ops.s` | `expected[]` in [`main.c`](main.c) |
| 51 | C `-100 / 7` (compiler-emitted DIV with its `BREAK 7` divide-by-zero guard) | `0xFFFFFFF2` |
| 52 | C `-100 % 7` | `0xFFFFFFFE` |
| 53-54 | libgcc `__udivdi3`: `0x0123456789ABCDEF / 12345`, low then high word | `0x45F5207D`, `0x0000060A` |
| 55 | `strlen("homebrew")` from libpsn00b's libc | 8 |
