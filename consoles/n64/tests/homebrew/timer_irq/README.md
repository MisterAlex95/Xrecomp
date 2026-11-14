# n64/timer_irq

Counts VI interrupts (`register_VI_handler`) for 60 frames while a continuous libdragon timer fires every 10 ms
(COUNT/COMPARE interrupt, `new_timer(TIMER_TICKS(10000), TF_CONTINUOUS, ...)`), and measures the elapsed COUNT
ticks (46.875 MHz, half the CPU clock) with `get_ticks()`.

Expected: NTSC/MPAL 60 frames = 1.001 s -> ~100 timer callbacks, ~46.92 M ticks; PAL 50 Hz -> 1.2 s -> ~120
callbacks, 56.25 M ticks.

## Result protocol

Block at physical `0x00300000` (see [../README.md](../README.md)), `test_id` = `TIRQ` (`0x54495251`), `frames` =
VIs counted (60 at the end). Halt loop after `status`; PASS when entry 0 is 60 and entries 1-2 are 1. Green screen
on pass, red on fail.

| table | value | check |
|---|---|---|
| 0 | 60 | VI interrupts while counting |
| 1 | 1 | timer callbacks within [95, 103] (NTSC) or [115, 123] (PAL) |
| 2 | 1 | elapsed COUNT ticks within +-5 % of the expected duration |
| 3 | raw | timer callback count |
| 4 | raw | elapsed ticks (low 32 bits) |
| 5 | 0 or 1 | PAL flag used for the ranges |
