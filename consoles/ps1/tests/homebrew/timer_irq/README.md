# ps1/timer_irq

Counts, for 60 frames: VBlanks (`VSyncCallback`), timer 1 IRQs (HBlank clock source, reset + IRQ at target 100,
repeat: one IRQ every 101 lines) and timer 2 IRQs (system clock / 8 = 4.2336 MHz, IRQ on `0xFFFF`, repeat), via
libpsn00b's `InterruptCallback` (BIOS interrupt chain underneath).

Expected per video standard: NTSC 263 lines x 60 = 15780 HBlanks -> ~156 timer 1 IRQs, 1.001 s -> ~64.7 timer 2
IRQs; PAL 314 lines -> ~186.5, 1.2 s -> ~77.5.

## Result protocol

Block at `0x801F0000` (see [../README.md](../README.md)), `test_id` = `TIRQ` (`0x54495251`), `frames` = VBlanks
counted (60 at the end). Halt loop after `status`; PASS when entry 0 is 60 and entries 1-2 are 1. Green screen on
pass, red on fail.

| table | value | check |
|---|---|---|
| 0 | 60 | VBlank callbacks while counting |
| 1 | 1 | timer 1 IRQs within [150, 165] (NTSC) or [180, 196] (PAL) |
| 2 | 1 | timer 2 IRQs within [62, 67] (NTSC) or [74, 81] (PAL) |
| 3 | raw | timer 1 IRQ count |
| 4 | raw | timer 2 IRQ count |
| 5 | 0 or 1 | PAL flag used for the ranges |
