# XRecomp 0.13.12

<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->

Warhammer: Battle March (Xbox 360) gets past the Bandai logo on the Thor.

- **Xbox 360: no crash on 8 and 16 bpp textures.** The texture decoder read 4 bytes for every texel. On the last
  texel of an 8 bpp texture whose buffer ended on a memory page, that read went past the end and the game crashed
  a few seconds after the logo. The decoder now reads only the bytes of each texel. 0.13.11's crash log found it.

## Changes

<!-- release:changes start (generated from the commits; rewritten on every push to main) -->

### X360

- texel() reads one texel, not 4 bytes past an 8 or 16 bpp one (3d86e3fc, XBA-318)

<!-- release:changes end -->
