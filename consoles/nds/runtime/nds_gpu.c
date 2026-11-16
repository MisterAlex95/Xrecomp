/* nds_gpu.c -- the 2D engines of the DS spike (XBA-223): text backgrounds (4 and 8 bits per pixel, any size, scroll,
 * flips, priorities), the backdrop, display modes off / normal / VRAM (engine A's LCDC bitmap). Enough for libnds's
 * console. Not there yet: affine and extended backgrounds, sprites, windows, blending, mosaic, extended palettes,
 * master brightness, the 3D engine. A whole frame is drawn at the start of VBlank from the registers then. */
#include <string.h>

#include "nds_internal.h"

static const uint8_t *page(nds_machine *m, int engine_b, uint32_t off)
{
    const uint8_t *p = engine_b ? m->vmap_bbg[(off >> 14) & 7] : m->vmap_abg[(off >> 14) & 31];
    return p ? p + (off & 0x3FFF) : NULL;
}

static uint8_t vram8(nds_machine *m, int b, uint32_t off)
{
    const uint8_t *p = page(m, b, off);
    return p ? *p : 0;
}

static uint16_t vram16(nds_machine *m, int b, uint32_t off)
{
    const uint8_t *p = page(m, b, off & ~1u);
    return p ? (uint16_t)(p[0] | p[1] << 8) : 0;
}

static void draw_text_bg(nds_machine *m, int b, uint32_t dispcnt, int bg, uint16_t *out)
{
    uint32_t io = b ? 0x1000u : 0;
    uint16_t cnt = nds_io16(m, io + 0x08 + 2u * (unsigned)bg);
    uint32_t hofs = nds_io16(m, io + 0x10 + 4u * (unsigned)bg) & 0x1FF;
    uint32_t vofs = nds_io16(m, io + 0x12 + 4u * (unsigned)bg) & 0x1FF;
    uint32_t chr = ((cnt >> 2) & 15) * 0x4000u, scr = ((cnt >> 8) & 31) * 0x800u;
    if (!b) {
        chr += ((dispcnt >> 24) & 7) * 0x10000u;
        scr += ((dispcnt >> 27) & 7) * 0x10000u;
    }
    bool c256 = cnt & 0x80;
    uint32_t size = (cnt >> 14) & 3;
    uint32_t wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    const uint8_t *pal = m->palette + (b ? 0x400 : 0);
    for (int y = 0; y < 192; y++) {
        uint32_t sy = ((uint32_t)y + vofs) & hmask;
        for (int x = 0; x < 256; x++) {
            uint32_t sx = ((uint32_t)x + hofs) & wmask;
            uint32_t block = (sx >> 8) + ((sy >> 8) * ((size & 1) ? 2u : 1u));
            uint32_t eoff = scr + block * 0x800u + (((sy & 255) >> 3) * 32u + ((sx & 255) >> 3)) * 2u;
            uint16_t e = vram16(m, b, eoff);
            uint32_t tx = sx & 7, ty = sy & 7;
            if (e & 0x400)
                tx = 7 - tx;
            if (e & 0x800)
                ty = 7 - ty;
            uint32_t tile = e & 0x3FF, ci;
            if (c256) {
                ci = vram8(m, b, chr + tile * 64u + ty * 8u + tx);
            } else {
                uint8_t two = vram8(m, b, chr + tile * 32u + ty * 4u + (tx >> 1));
                ci = (tx & 1) ? two >> 4 : two & 15;
                if (ci)
                    ci += (uint32_t)(e >> 12) * 16u;
            }
            if (!(ci & (c256 ? 0xFFu : 0x0Fu)))
                continue;
            out[y * 256 + x] = (uint16_t)((pal[ci * 2] | pal[ci * 2 + 1] << 8) & 0x7FFF);
        }
    }
}

static void render_engine(nds_machine *m, int b, uint16_t *out)
{
    uint32_t io = b ? 0x1000u : 0;
    uint32_t dispcnt = nds_io32(m, io);
    uint32_t mode = (dispcnt >> 16) & (b ? 1 : 3);
    if (mode == 0) {
        for (int i = 0; i < 256 * 192; i++)
            out[i] = 0x7FFF;
        return;
    }
    if (mode == 2) { /* VRAM display: an LCDC bank as a 256x192 bitmap */
        static const uint32_t off[4] = {0x00000, 0x20000, 0x40000, 0x60000};
        memcpy(out, m->vram + off[(dispcnt >> 18) & 3], 256 * 192 * 2);
        for (int i = 0; i < 256 * 192; i++)
            out[i] &= 0x7FFF;
        return;
    }
    if (mode == 3) { /* main memory display FIFO: not modelled */
        memset(out, 0, 256 * 192 * 2);
        return;
    }
    const uint8_t *pal = m->palette + (b ? 0x400 : 0);
    uint16_t backdrop = (uint16_t)((pal[0] | pal[1] << 8) & 0x7FFF);
    for (int i = 0; i < 256 * 192; i++)
        out[i] = backdrop;
    uint32_t bgmode = dispcnt & 7;
    for (int prio = 3; prio >= 0; prio--) {
        for (int bg = 3; bg >= 0; bg--) {
            if (!(dispcnt & (0x100u << bg)))
                continue;
            if ((nds_io16(m, io + 0x08 + 2u * (unsigned)bg) & 3) != (unsigned)prio)
                continue;
            bool text = bg <= 1 || (bg == 2 && (bgmode == 0 || bgmode == 1 || bgmode == 3)) || (bg == 3 && bgmode == 0);
            if (bg == 0 && !b && (dispcnt & 8))
                text = false; /* BG0 is the 3D layer */
            if (text)
                draw_text_bg(m, b, dispcnt, bg, out);
        }
    }
}

void nds_gpu_render(nds_machine *m)
{
    render_engine(m, 0, m->fb_a);
    render_engine(m, 1, m->fb_b);
}
