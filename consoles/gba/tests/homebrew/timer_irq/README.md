# gba/timer_irq

Interrupts through libtonc's switchboard (`irq_init`, which installs its master ISR at `0x03007FFC` for the BIOS IRQ
vector) and `VBlankIntrWait` (SWI 0x05).

- Phase A, 60 frames: VBlank IRQ + timer 0 overflow IRQ (1024-cycle prescaler, reload `0xFF00`: one overflow every
  262144 cycles), timer 1 in cascade mode counting timer 0 overflows. Expected: 60 frames x 280896 cycles = 64.3
  overflows.
- Phase B, 10 frames: VBlank + HBlank IRQ. HBlank fires on all 228 lines: 2280 expected.

## Result protocol

Block at `0x0203FE00` (see [../README.md](../README.md)), `test_id` = `TIRQ` (`0x54495251`). `frames` counts every
VBlank since start (72 at the end in mGBA). Halt loop after `status`; PASS when entries 0-3 are 1.

| table | value | check |
|---|---|---|
| 0 | 1 | timer 0 IRQs in phase A within [63, 65] |
| 1 | 1 | timer 1 (cascade) count within [63, 65] |
| 2 | 1 | timer 1 count within 1 of the timer 0 IRQ count |
| 3 | 1 | HBlank IRQs in phase B within 2280 +- 228 |
| 4 | raw | timer 0 IRQ count (64 in mGBA) |
| 5 | raw | timer 1 count (64 in mGBA) |
| 6 | raw | HBlank IRQ count (2280 in mGBA) |

Phase A and B are separate on purpose: with HBlank and timer IRQs enabled together, mGBA 0.10 dropped 3 of 64 timer
IRQs; the split keeps the test about counting, not about IRQ-flood ordering.
