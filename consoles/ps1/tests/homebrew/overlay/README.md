# ps1/overlay

Code loaded from the disc at run time, as PS1 games load overlays: the static recompiler cannot see it in the EXE,
so the interpreter fallback (and `discovered.jsonl`) must cover it.

- `OVL_A.BIN` and `OVL_B.BIN` are flat binaries linked at `0x80100000` ([`ovl.ld`](ovl.ld)), entry point first,
  built from [`ovl_a.c`](ovl_a.c) / [`ovl_b.c`](ovl_b.c) with `-G0 -fno-pic` (no `$gp`). Signature of both:
  `uint32_t ovl_entry(uint32_t x, uint32_t (*cb)(uint32_t))`.
  - A: `(ops[x & 1](x) + cb(x)) ^ 0xA0A00000`, `ops` = { 2x, 3x }, a table of absolute pointers inside the overlay.
  - B: `(cb(x) + cb(x << 4)) | 0xB0000000`.
- The EXE finds each file with `CdSearchFile`, reads it with `CdRead` to `0x80100000`, calls `FlushCache()` (BIOS
  A(44h)) and jumps to it with x = `0x21`. The callback lives in the EXE (`cb(v) = v + 0x1000`, counts its calls).
- B replaces A at the same address: a runtime that caches translations by address must notice the new bytes.

## Result protocol

Block at `0x801F0000` (see [../README.md](../README.md)), `test_id` = `OVL ` (`0x4F564C20`). Halt loop after
`status`; PASS when entries 0-5 hold. Green screen on pass, red on fail.

| table | value | check |
|---|---|---|
| 0 | 1 | `CdInit()` succeeded |
| 1 | 1 | `\OVL_A.BIN;1` found and read |
| 2 | `0xA0A01084` | overlay A result |
| 3 | 1 | `\OVL_B.BIN;1` found and read |
| 4 | `0xB0002231` | overlay B result, same address |
| 5 | 3 | callback calls (1 from A, 2 from B) |
| 6 | raw | size of `OVL_A.BIN` (124 bytes with the pinned toolchain) |
| 7 | raw | size of `OVL_B.BIN` (72 bytes) |
