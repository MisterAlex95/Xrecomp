# n64/rsp

The RSP differential test (XBA-173): the groups of [`../common/rsp_oracle.h`](../common/rsp_oracle.h) run on the
console's RSP through its register interface, and each group's FNV-1a checksum goes into the result table. Every
vector computational funct x 16 element selectors with random accumulators and flags, a VRCP / VRSQ sweep, every
vector load and store at random and wrapping addresses, SP DMA from the CPU side, SP_STATUS pairs and the semaphore,
and the scalar unit. Microcode is written with the C encoders of [`../common/rsp_asm.h`](../common/rsp_asm.h).

The ROM checks nothing itself: run on ares (the oracle), its table gives the expected values of the host test
`consoles/n64/tests/test_rsp.c` (`rsp_oracle_expected.h`), which runs the same groups on `n64_rsp.c`. A real
console would give the hardware's values the same way.

## Result protocol

Block at physical `0x00300000` (see [../README.md](../README.md)), `test_id` = `RSP ` (`0x52535020`), `stage` =
group being run (93 at the end), `count` = 93, `checksum` = FNV-1a of the table. PASS once every group ran.

| table | value |
|---|---|
| 0..63 | vector computational funct 0x00..0x3F |
| 64 | divide sweep |
| 65..76 | vector loads, LWC2 functions 0..11 |
| 77..88 | vector stores, SWC2 functions 0..11 |
| 89 | SP DMA |
| 90 | SP_STATUS and semaphore |
| 91 | scalar unit |
| 92 | the divide ROMs, entry by entry |
