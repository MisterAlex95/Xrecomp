# gba/sound

The APU (XBA-155), audible and self-checking. Registers are written by address (GBATEK « GBA Sound Controller »).
Listen to it with `gba_run sound.gba --result --wav sound.wav` (about 4.3 s at 48 kHz).

- Phase A, 60 frames: square 1 at 440 Hz (volume 10, duty 50 %, no sweep or envelope); square 2 beeps at 880 Hz with
  length 0 (64/256 s), then square 1 stops by turning its DAC off.
- Phase B, 30 frames: noise, 15-bit LFSR (r = 1, s = 3), envelope 15 going down every 2/64 s.
- Phase C, 60 frames: wave channel, a 32-sample triangle in bank 1 at 218 Hz, after a bank-switching check.
- Phase D, 90 frames: Direct Sound A, a 440 + 660 Hz chord at 16384 Hz (built at start from libtonc's `sin_lut`),
  streamed by DMA1 (special timing, repeat, IRQ) into FIFO A on timer 0 overflows.
- Then the sound master is turned off.

## Result protocol

Block at `0x0203FE00` (see [../README.md](../README.md)), `test_id` = `SND ` (`0x534E4420`). `frames` counts every
VBlank since start. Halt loop after `status`; PASS when entries 0-10 hold their expected value.

| table | value | check |
|---|---|---|
| 0 | 0x200 | SOUNDBIAS level at boot |
| 1 | 0xA080 | SOUND1CNT_H read back after writing 0xA0BF, length bits masked |
| 2 | 0x81 | SOUNDCNT_X right after triggering square 1: master on + channel 1 |
| 3 | 1 | square 2 status bit set right after its trigger |
| 4 | 1 | square 2 status bit clear 20 frames later (length expired after 15) |
| 5 | 1 | square 1 off once its DAC is turned off |
| 6 | 1 | noise status bit set after its trigger |
| 7 | 1 | wave RAM: the CPU reads and writes the bank that is not selected |
| 8 | 1 | wave status bit set after its trigger |
| 9 | 1 | DMA1 refills of FIFO A in 60 frames within [1018, 1039] (16 bytes each, 1028.7 expected) |
| 10 | 1 | master off: SOUNDCNT_X, SOUND2CNT_L and SOUNDCNT_L read 0 |
| 11 | raw | DMA1 refill count (1028 in mGBA and in our core) |
| 12 | raw | SOUND1CNT_H as read (0xA080 here; mGBA 0.10.1 returns the length bits too: 0xA0BF) |

Validated in mGBA 0.10.1 (`xba-mgba-check` image, GDB stub, watchpoint on `status`, 2026-11-14): PASS, with the same
DMA count as our core.

Once done, the screen shows the verdict: backdrop green on pass, red on fail (the screenshot of
`tools/consoles/gba_corpus_packs.py`).
