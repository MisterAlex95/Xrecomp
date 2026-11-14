# gba/iwram_copy

Code that executes from RAM, which a static recompiler cannot see in the ROM image as it will run: the case the
interpreter fallback (and its `discovered.jsonl`) must cover.

1. `iwram_fn`, a C function in section `.iwram` compiled as ARM: crt0 copies it from ROM to IWRAM at boot.
2. `pic_routine` ([`routine.s`](routine.s)), position-independent ARM with a PC-relative literal, copied from ROM
   into an IWRAM buffer at run time and called through a function pointer (`BX` to an even address).
   `pic_routine(x) = ((x * 3) + IMM) ^ 0x5A5A0000`, IMM = low byte of the instruction at `pic_patch_offset`.
3. The IWRAM copy re-installed with a new immediate, then patched in place (one word): each call must run the
   current bytes, never a stale translation.
4. A second copy in EWRAM with its own immediate; the IWRAM copy must be unaffected.

## Result protocol

Block at `0x0203FE00` (see [../README.md](../README.md)), `test_id` = `IWRM` (`0x4957524D`). Halt loop after
`status`; PASS when every entry equals its expected value.

| table | value | check |
|---|---|---|
| 0 | 1 | `iwram_fn` address is in IWRAM (`0x03xxxxxx`) |
| 1 | `0xE138D6CF` | `iwram_fn(20, 0x1234)` equals the same loop compiled in ROM as Thumb |
| 2 | `0x5A5A0311` | IWRAM copy, IMM `0x11`, x = `0x100` |
| 3 | `0x5A5A0322` | re-installed at the same address, IMM `0x22` |
| 4 | `0x5A5A0644` | one word patched to IMM `0x44`, x = `0x200` |
| 5 | 1 | EWRAM buffer address is in EWRAM (`0x02xxxxxx`) |
| 6 | `0x5A5A0333` | EWRAM copy, IMM `0x33` |
| 7 | `0x5A5A0344` | IWRAM copy called again: still the patched code |

Once done, the screen shows the verdict: backdrop green on pass, red on fail (the screenshot of
`tools/consoles/gba_corpus_packs.py`).
