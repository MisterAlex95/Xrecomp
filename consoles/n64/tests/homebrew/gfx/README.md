# n64/gfx

One deterministic 320x240 RGBA16 frame drawn by the RDP through rdpq, cleared to black (VI `FILTERS_RESAMPLE`: libdragon refuses `FILTERS_DISABLED` at 320 wide; the filter only touches the VI output, not the frame buffer the checks read):

| Draw | Mode | Where |
|---|---|---|
| fill rectangle, red | fill | (16,16)-(80,64) |
| fill rectangle, blue | fill | (40,80)-(140,120) |
| 16x16 RGBA16 texture (checker 4x4 magenta/white) 1:1 | copy | at (120,16) |
| same texture scaled 4x | standard | at (200,16) |
| triangle (160,130) (300,220) (40,220), vertex colours red, green, blue | standard, shade combiner | |

## Result protocol

Block at physical `0x00300000` (see [../README.md](../README.md)), `test_id` = `GFX ` (`0x47465820`).

- **Capture point**: `stage` becomes 1 two VIs after the frame is shown; the runner grabs the displayed frame at
  the first VI where `stage` is 1. 30 VIs follow, then `status`.
- Pixels are read back from the frame buffer through KSEG1 after `rdpq_detach_wait()`.
- Halt loop after `status`; PASS when entries 0-4 hold.

| table | check |
|---|---|
| 0 | pixel (20,20) = red `0xF801` |
| 1 | pixel (100,100) = blue `0x003F` |
| 2 | pixel (5,5) = black (alpha bit ignored) |
| 3 | pixel (121,17) = texel (1,1) of the 1:1 blit |
| 4 | 1 if pixel (160,200) of the triangle has all three channels > 2/31 |
| 5 | raw value of that triangle pixel (informative) |
