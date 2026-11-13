# gba/gfx

Two deterministic scenes for renderer comparison (reference images are produced later, by the runtime ticket, from
an emulator run; none are committed here).

- **Scene 1, mode 0**: 4 procedural 4bpp tiles (solid, checker, stripes, diagonal), 16-colour BG and OBJ palettes,
  BG0 (screenblock 30, map `(x + 2y) & 3`, priority 1), BG1 (screenblock 31, diagonal of h-flipped tile 3,
  scrolled (3,5), priority 0), 4 sprites in 1D mapping: 8x8 tile 1 at (40,40), 16x16 at (100,60), 16x16 h-flipped
  at (180,100), 8x8 tile 3 at (220,140).
- **Scene 2, mode 3**: 240x160 gradient (R by x, G by y, B = 12), white rectangle (20,20)-(100,60), black rectangle
  (140,90)-(220,150), magenta diagonal from (40,0).

## Result protocol

Block at `0x0203FE00` (see [../README.md](../README.md)), `test_id` = `GFX ` (`0x47465820`).

- **Capture points**: `stage` becomes 1 when scene 1 has been on screen for 30 frames, 2 when scene 2 has. The runner
  grabs the frame at the first VBlank where `stage` takes a new value. 30 more frames follow each capture.
- `checksum` = FNV-1a of the mode 3 frame buffer (VRAM `0x06000000`, 76800 bytes) after drawing:
  `0x702BBC34` in mGBA.
- Halt loop after `status`; PASS when every entry is 0.

| table | check |
|---|---|
| 0 | tile data read back from VRAM equals what was written (count of mismatching halfwords) |
| 1 | BG palette read back |
| 2 | OAM (first 4 entries) read back |
| 3 | four mode 3 pixels read back (gradient corner, both rectangles, diagonal) |
