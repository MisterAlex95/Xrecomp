# ps1/gfx

One deterministic 320x240 frame (display and drawing area at VRAM (0,0), dithering off, background (32,32,32)):

| Primitive | Where | Detail |
|---|---|---|
| `TILE` | (16,16) 64x48 | flat red |
| `POLY_F3` | (120,16) (200,100) (100,90) | flat green |
| `POLY_G4` | (220,16)-(300,96) | corners red, green, blue, white |
| `POLY_FT4` | (16,120)-(112,216) | 16x16 16-bit texture at VRAM (640,0), checker 4x4 magenta/white, neutral modulation |
| `SPRT` | (160,140) 16x16 | 4-bit texture at VRAM (640,256), CLUT at (640,480), page set by a `DR_TPAGE` |

Textures and CLUT are uploaded with `LoadImage`; pixels are read back with `StoreImage`.

## Result protocol

Block at `0x801F0000` (see [../README.md](../README.md)), `test_id` = `GFX ` (`0x47465820`).

- **Capture point**: `stage` becomes 1 one frame after the frame is drawn; the runner grabs the displayed frame at
  the first VBlank where `stage` is 1. 30 frames follow, then `status`.
- Halt loop after `status`; PASS when entries 0-4 hold.

| table | check |
|---|---|
| 0 | pixel (20,20) = TILE red (`0x001F`) |
| 1 | pixel (150,60) = triangle green (`0x03E0`) |
| 2 | pixel (5,5) = background (`0x1084`) |
| 3 | pixel (17,121) = top-left texel of the textured quad |
| 4 | 1 if pixel (260,56) of the gouraud quad has all three channels > 4/31 |
| 5 | raw value of that gouraud pixel (informative) |
