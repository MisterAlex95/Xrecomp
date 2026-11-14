/* Game Boy Advance PPU: a pure scanline renderer (XBA-154). The core owns the memory and timing; the PPU reads
 * the IO registers, palette, VRAM and OAM as the CPU left them and renders one line. No globals, no state. */
#ifndef GBA_PPU_H
#define GBA_PPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gba_ppu_inputs {
    const uint8_t *io;      /* 0x04000000.. 0x400 bytes, little-endian registers as the CPU wrote them */
    const uint8_t *palette; /* 1 KB */
    const uint8_t *vram;    /* 96 KB */
    const uint8_t *oam;     /* 1 KB */
    /* internal affine reference points for BG2/BG3, latched at VBlank and advanced by PB/PD each line by the core */
    int32_t bg2x, bg2y, bg3x, bg3y;
} gba_ppu_inputs;

void gba_ppu_render_line(const gba_ppu_inputs *in, int line, uint16_t out[240]); /* RGB555 */

#ifdef __cplusplus
}
#endif

#endif
