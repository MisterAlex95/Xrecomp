# ps1/mcard

A save on memory card 1 the way games save: through the BIOS memory card functions (OpenBIOS implements them;
`card/device.c` of its source), not the SIO registers.

- `InitCARD(1)`, `StartCARD()`, `_bu_init()`, then `open("bu00:XBAMCARD", FREAD)`.
- The file is missing: `open` with `FCREAT | 1 << 16` (one 8 KB block) creates it; when that fails the card is not a
  formatted one, so `format("bu00:")` (BIOS B(41h), no PSn00bSDK wrapper) formats it and the create is retried
  (OpenBIOS has no `_get_errno`, B(54h), to tell the two apart). Then `open(FWRITE)`, `write` 8192 bytes of the
  pattern `byte[i] = (7 i + 0x5A) ^ (i >> 8)`, `close`, `open(FREAD)`, `read` 8192 bytes, compare.
- The file is there (a card an earlier run saved): read and compared, nothing written.
- `firstfile("bu00:*")` / `nextfile` must list `XBAMCARD` with 8192 bytes.

## Result protocol

Block at `0x801F0000` (see [../README.md](../README.md)), `test_id` = `MCRD` (`0x4D435244`), `stage` = table[0],
`checksum` = FNV-1a of the 8 KB read back (`0x8EEFA7C5` when it holds the pattern). Halt loop after `status`; PASS
when entries 2-6 hold. Green screen on pass, red on fail.

| table | value | check |
|---|---|---|
| 0 | 1 or 2 | 1: the save was created, 2: an earlier save was found |
| 1 | 0 or 1 | 1: the card had to be formatted first (a failed format fails the check) |
| 2 | 1 | the file opened (created, or found) |
| 3 | 8192 | bytes written (8192 when found: nothing to write) |
| 4 | 8192 | bytes read back |
| 5 | 1 | the data is the pattern |
| 6 | 1 | `firstfile` lists `XBAMCARD`, 8192 bytes |
| 7 | raw | files on the card |

ctest `ps1_mcard` (`consoles/ps1/tests/mcard_test.sh`) runs it under `ps1_run --card FILE` on a blank card, again on
the card it saved (table[0] = 2, the card unchanged) and on a card of zeros (table[1] = 1), and checks the card image
with `check_mcard.py`; the `ps1_runtime` test saves through the runtime's card files (debounce, then reload).
