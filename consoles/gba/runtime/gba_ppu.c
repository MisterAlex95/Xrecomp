/* Game Boy Advance PPU: a stateless scanline renderer (XBA-154).
 *
 * gba_ppu_render_line() renders one line from the IO registers, palette, VRAM and OAM as the core left them. It
 * keeps no state between calls: the core advances the BG2/BG3 affine reference points (see gba_ppu.h) and calls
 * the renderer once per visible line, typically at the start of HBlank.
 *
 * Per line: each enabled background is drawn into a line buffer (opaque pixels carry bit 15), the sprites into an
 * OBJ line buffer (colour, priority, semi-transparent flag) plus an OBJ window mask, the window mask is built, then
 * one pass per pixel picks the two front-most visible layers and applies the colour special effect.
 *
 * Reference: GBATEK. Ideas from ares (ISC). mGBA is only used as an external oracle, never as a source.
 * Approximations: the per-line sprite cycle budget drops whole sprites (the one that crosses the budget is still
 * drawn); vertical mosaic of affine BGs assumes PB/PD constant within a block; OBJ mosaic snaps to a screen grid
 * clamped at the sprite's left edge; windows with X1 > X2 or Y1 > Y2 wrap around; a disabled window effect bit also
 * disables semi-transparent OBJ blending.
 */
#include "gba_ppu.h"

#include <string.h>

enum {
    W = 240,
    REG_DISPCNT = 0x00,
    REG_GREENSWAP = 0x02,
    REG_BG0CNT = 0x08,
    REG_BG0HOFS = 0x10,
    REG_BG2PA = 0x20,
    REG_WIN0H = 0x40,
    REG_WIN1H = 0x42,
    REG_WIN0V = 0x44,
    REG_WIN1V = 0x46,
    REG_WININ = 0x48,
    REG_WINOUT = 0x4A,
    REG_MOSAIC = 0x4C,
    REG_BLDCNT = 0x50,
    REG_BLDALPHA = 0x52,
    REG_BLDY = 0x54,
    OPAQUE = 0x8000,
    LAYER_OBJ = 4,
    LAYER_BD = 5,
    OBJ_NONE = 0xFF,
};

static inline unsigned rd16(const uint8_t *p, unsigned off)
{
    return (unsigned)p[off] | ((unsigned)p[off + 1] << 8);
}

static inline uint32_t rd32(const uint8_t *p, unsigned off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) |
           ((uint32_t)p[off + 3] << 24);
}

typedef struct line_ctx {
    const gba_ppu_inputs *in;
    int line;
    unsigned dispcnt;
    unsigned mosaic;
    uint16_t bg[4][W];      /* colour | OPAQUE, 0 = transparent */
    uint16_t lut4[256];     /* BG palette | OPAQUE, colour 0 of each 16-colour bank = 0 (transparent) */
    uint16_t lut8[256];     /* BG palette | OPAQUE, colour 0 = 0 */
    uint16_t objc[W];       /* winning sprite colour | OPAQUE */
    uint8_t objp[W];        /* its priority, OBJ_NONE = none */
    uint8_t objsemi[W];     /* it is semi-transparent */
    uint8_t objwin[W];      /* an OBJ-window sprite covers the pixel */
    uint8_t winmask[W];     /* per pixel: bits 0-3 BG, 4 OBJ, 5 effects */
} line_ctx;

/* ---- backgrounds ---------------------------------------------------------------------------------------- */

static void bg_mosaic_h(uint16_t *dst, unsigned mh)
{
    for (unsigned x = 0; x < W; x += mh) {
        uint16_t c = dst[x];
        unsigned end = x + mh < W ? x + mh : W;
        for (unsigned k = x + 1; k < end; k++)
            dst[k] = c;
    }
}

static void render_text_bg(line_ctx *c, int bg)
{
    const uint8_t *io = c->in->io, *vram = c->in->vram;
    uint16_t *dst = c->bg[bg];
    unsigned cnt = rd16(io, REG_BG0CNT + 2 * bg);
    unsigned hofs = rd16(io, REG_BG0HOFS + 4 * bg) & 0x1FF;
    unsigned vofs = rd16(io, REG_BG0HOFS + 4 * bg + 2) & 0x1FF;
    unsigned size = cnt >> 14;
    unsigned wide = size & 1, tall = size >> 1;
    unsigned wmask = wide ? 511 : 255, hmask = tall ? 511 : 255;
    unsigned charbase = ((cnt >> 2) & 3) * 0x4000;
    unsigned screenbase = ((cnt >> 8) & 31) * 0x800;
    int bpp8 = (cnt >> 7) & 1;
    int line = c->line;

    if (cnt & 0x40) {
        unsigned mv = ((c->mosaic >> 4) & 15) + 1;
        line -= line % (int)mv;
    }
    unsigned y = ((unsigned)line + vofs) & hmask;
    unsigned rowbase = screenbase + (tall ? (y >> 8) * (wide ? 0x1000 : 0x800) : 0) + ((y & 255) >> 3) * 64;
    unsigned ty0 = y & 7;

    unsigned x = 0;
    while (x < W) {
        unsigned sx = (x + hofs) & wmask;
        unsigned maddr = (rowbase + (wide ? (sx >> 8) * 0x800 : 0) + ((sx & 255) >> 3) * 2) & 0xFFFF;
        unsigned e = rd16(vram, maddr);
        unsigned px = sx & 7, n = 8 - px;
        if (x + n > W)
            n = W - x;
        unsigned ty = (e & 0x800) ? 7 - ty0 : ty0;
        unsigned hf = (e & 0x400) ? 7 : 0;
        if (!bpp8) {
            unsigned addr = charbase + (e & 0x3FF) * 32 + ty * 4;
            uint32_t row = addr >= 0x10000 ? 0 : rd32(vram, addr); /* BG tiles cannot reach OBJ VRAM */
            if (row == 0) {   /* colour 0 everywhere: transparent */
                memset(dst + x, 0, n * sizeof *dst);
            } else {
                if (hf) {   /* mirrored: reverse the eight nibbles once instead of per pixel */
                    row = __builtin_bswap32(row);
                    row = ((row >> 4) & 0x0F0F0F0Fu) | ((row & 0x0F0F0F0Fu) << 4);
                }
                const uint16_t *lut = c->lut4 + (e >> 12) * 16;
                row >>= px * 4;
                if (n == 8) {
                    for (unsigned k = 0; k < 8; k++)
                        dst[x + k] = lut[(row >> (k * 4)) & 15];
                } else {
                    for (unsigned k = 0; k < n; k++)
                        dst[x + k] = lut[(row >> (k * 4)) & 15];
                }
            }
        } else {
            unsigned addr = charbase + (e & 0x3FF) * 64 + ty * 8;
            if (addr >= 0x10000) {
                memset(dst + x, 0, n * sizeof *dst);
            } else {
                const uint8_t *row = vram + addr;
                for (unsigned k = 0; k < n; k++)
                    dst[x + k] = c->lut8[row[(px + k) ^ hf]];
            }
        }
        x += n;
    }
    if (cnt & 0x40) {
        unsigned mh = (c->mosaic & 15) + 1;
        if (mh > 1)
            bg_mosaic_h(dst, mh);
    }
}

/* The line's affine start point and per-pixel step for BG2 (bg = 2) or BG3; vertical mosaic rewinds the reference
 * point to the first line of the mosaic block, assuming PB/PD did not change inside the block. */
static void affine_setup(const line_ctx *c, int bg, int32_t *x, int32_t *y, int32_t *pa, int32_t *pc)
{
    const uint8_t *io = c->in->io;
    unsigned base = REG_BG2PA + (unsigned)(bg - 2) * 16;
    unsigned cnt = rd16(io, REG_BG0CNT + 2 * bg);
    *pa = (int16_t)rd16(io, base);
    *pc = (int16_t)rd16(io, base + 4);
    *x = bg == 2 ? c->in->bg2x : c->in->bg3x;
    *y = bg == 2 ? c->in->bg2y : c->in->bg3y;
    if (cnt & 0x40) {
        int mv = (int)((c->mosaic >> 4) & 15) + 1;
        int back = c->line % mv;
        if (back) {
            *x -= back * (int32_t)(int16_t)rd16(io, base + 2);
            *y -= back * (int32_t)(int16_t)rd16(io, base + 6);
        }
    }
}

static void finish_affine(line_ctx *c, int bg)
{
    if (rd16(c->in->io, REG_BG0CNT + 2 * bg) & 0x40) {
        unsigned mh = (c->mosaic & 15) + 1;
        if (mh > 1)
            bg_mosaic_h(c->bg[bg], mh);
    }
}

static void render_affine_bg(line_ctx *c, int bg)
{
    const uint8_t *vram = c->in->vram;
    uint16_t *dst = c->bg[bg];
    unsigned cnt = rd16(c->in->io, REG_BG0CNT + 2 * bg);
    int32_t size = 128 << (cnt >> 14), mask = size - 1;
    unsigned tpr_shift = 4 + (cnt >> 14);   /* tiles per row = size / 8 */
    int wrap = (cnt >> 13) & 1;
    unsigned charbase = ((cnt >> 2) & 3) * 0x4000;
    unsigned screenbase = ((cnt >> 8) & 31) * 0x800;
    int32_t x, y, pa, pc;

    affine_setup(c, bg, &x, &y, &pa, &pc);
    for (int i = 0; i < W; i++, x += pa, y += pc) {
        int32_t ix = x >> 8, iy = y >> 8;   /* arithmetic shift on every supported compiler */
        if (wrap) {
            ix &= mask;
            iy &= mask;
        } else if ((uint32_t)ix >= (uint32_t)size || (uint32_t)iy >= (uint32_t)size) {
            dst[i] = 0;
            continue;
        }
        unsigned tile = vram[(screenbase + ((unsigned)iy >> 3 << tpr_shift) + ((unsigned)ix >> 3)) & 0xFFFF];
        dst[i] = c->lut8[vram[charbase + tile * 64 + (iy & 7) * 8 + (ix & 7)]];
    }
    finish_affine(c, bg);
}

static void render_bitmap_bg(line_ctx *c, int mode)
{
    const uint8_t *vram = c->in->vram;
    uint16_t *dst = c->bg[2];
    int32_t x, y, pa, pc;
    int32_t bw = mode == 5 ? 160 : 240, bh = mode == 5 ? 128 : 160;
    unsigned frame = (mode != 3 && (c->dispcnt & 0x10)) ? 0xA000 : 0;

    affine_setup(c, 2, &x, &y, &pa, &pc);
    if (pa == 0x100 && pc == 0) {   /* unscaled, unrotated (the usual case): one row, x + i */
        int32_t ix0 = x >> 8, iy = y >> 8;
        for (int i = 0; i < W; i++) {
            int32_t ix = ix0 + i;
            if ((uint32_t)ix >= (uint32_t)bw || (uint32_t)iy >= (uint32_t)bh)
                dst[i] = 0;
            else if (mode == 4)
                dst[i] = c->lut8[vram[frame + (unsigned)(iy * bw + ix)]];
            else
                dst[i] = (uint16_t)(rd16(vram, frame + (unsigned)(iy * bw + ix) * 2) | OPAQUE);
        }
        finish_affine(c, 2);
        return;
    }
    for (int i = 0; i < W; i++, x += pa, y += pc) {
        int32_t ix = x >> 8, iy = y >> 8;
        if ((uint32_t)ix >= (uint32_t)bw || (uint32_t)iy >= (uint32_t)bh) {
            dst[i] = 0;
            continue;
        }
        unsigned off = (unsigned)(iy * bw + ix);
        if (mode == 4) {
            dst[i] = c->lut8[vram[frame + off]];
        } else {
            dst[i] = (uint16_t)(rd16(vram, frame + off * 2) | OPAQUE);
        }
    }
    finish_affine(c, 2);
}

/* ---- sprites -------------------------------------------------------------------------------------------- */

static const uint8_t obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

/* Palette index of texel (tx, ty) of a sprite whose first tile is `tile` (32-byte units in OBJ VRAM). `stride` is
 * the tile distance between two rows of 8 pixels. */
static inline unsigned obj_texel(const uint8_t *objv, unsigned tile, unsigned tx, unsigned ty, int bpp8,
                                 unsigned stride)
{
    if (!bpp8) {
        unsigned a = ((tile + (ty >> 3) * stride + (tx >> 3)) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)) & 0x7FFF;
        unsigned b = objv[a];
        return (tx & 1) ? b >> 4 : b & 15;
    }
    unsigned a = ((tile + (ty >> 3) * stride + (tx >> 3) * 2) * 32 + (ty & 7) * 8 + (tx & 7)) & 0x7FFF;
    return objv[a];
}

static inline void obj_plot(line_ctx *c, int x, unsigned idx, unsigned color, unsigned prio, unsigned mode)
{
    if (!idx)
        return;
    if (mode == 2) {
        c->objwin[x] = 1;
    } else if (prio < c->objp[x]) {
        c->objc[x] = (uint16_t)(color | OPAQUE);
        c->objp[x] = (uint8_t)prio;
        c->objsemi[x] = mode == 1;
    }
}

static void render_sprites(line_ctx *c)
{
    const uint8_t *oam = c->in->oam, *pal = c->in->palette;
    const uint8_t *objv = c->in->vram + 0x10000;
    int line = c->line;
    int bitmap_mode = (c->dispcnt & 7) >= 3;
    int map1d = (c->dispcnt >> 6) & 1;
    int objwin_on = (c->dispcnt >> 15) & 1;
    unsigned mh = ((c->mosaic >> 8) & 15) + 1, mv = ((c->mosaic >> 12) & 15) + 1;
    /* Sprite rendering cycles per line (GBATEK): 1210, or 954 with "H-Blank Interval Free". */
    int cycles = (c->dispcnt & 0x20) ? 954 : 1210;

    for (int n = 0; n < 128 && cycles > 0; n++) {
        const uint8_t *o = oam + n * 8;
        unsigned a0 = rd16(o, 0), a1 = rd16(o, 2), a2 = rd16(o, 4);
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200))
            continue;   /* disabled */
        unsigned shape = a0 >> 14, sz = a1 >> 14;
        if (shape == 3)
            continue;
        int w = obj_w[shape][sz], h = obj_h[shape][sz];
        int dbl = affine && (a0 & 0x200);
        int bw = dbl ? 2 * w : w, bh = dbl ? 2 * h : h;
        int sy = a0 & 0xFF;
        int dy = (line - sy) & 0xFF;
        if (dy >= bh)
            continue;
        cycles -= affine ? 10 + 2 * bw : bw;   /* the cost is charged even when the sprite is off-screen in x */

        unsigned mode = (a0 >> 10) & 3;
        if (mode == 3 || (mode == 2 && !objwin_on))
            continue;
        unsigned tile = a2 & 0x3FF;
        if (bitmap_mode && tile < 512)
            continue;   /* that OBJ tile area holds the bitmap */
        int sx = a1 & 0x1FF;
        if (sx & 0x100)
            sx -= 512;
        if (sx >= W || sx + bw <= 0)
            continue;
        int bpp8 = (a0 >> 13) & 1;
        int mosaic = (a0 >> 12) & 1;
        unsigned prio = (a2 >> 10) & 3;
        unsigned pbase = 0x200 + (bpp8 ? 0 : (a2 >> 12) * 32);
        unsigned stride = map1d ? (unsigned)(w >> 3) << bpp8 : 32;
        if (bpp8 && !map1d)
            tile &= ~1u;
        if (mosaic && mv > 1) {
            int my = dy - (int)((unsigned)line % mv);
            if (my >= 0)
                dy = my;
        }
        int x0 = sx < 0 ? 0 : sx, x1 = sx + bw > W ? W : sx + bw;

        if (!affine) {
            unsigned ty = (a1 & 0x2000) ? (unsigned)(h - 1 - dy) : (unsigned)dy;
            int hflip = (a1 >> 12) & 1;
            for (int x = x0; x < x1; x++) {
                int i = x - sx;
                if (mosaic && mh > 1) {
                    i = x - (int)((unsigned)x % mh) - sx;
                    if (i < 0)
                        i = 0;
                }
                unsigned tx = hflip ? (unsigned)(w - 1 - i) : (unsigned)i;
                unsigned idx = obj_texel(objv, tile, tx, ty, bpp8, stride);
                obj_plot(c, x, idx, rd16(pal, pbase + idx * 2), prio, mode);
            }
        } else {
            const uint8_t *pp = oam + ((a1 >> 9) & 31) * 32;
            int32_t pa = (int16_t)rd16(pp, 6), pb = (int16_t)rd16(pp, 14);
            int32_t pc = (int16_t)rd16(pp, 22), pd = (int16_t)rd16(pp, 30);
            int32_t iy = dy - bh / 2;
            for (int x = x0; x < x1; x++) {
                int32_t ix = x - sx;
                if (mosaic && mh > 1) {
                    ix = x - (int)((unsigned)x % mh) - sx;
                    if (ix < 0)
                        ix = 0;
                }
                ix -= bw / 2;
                int32_t tx = ((pa * ix + pb * iy) >> 8) + w / 2;
                int32_t ty = ((pc * ix + pd * iy) >> 8) + h / 2;
                if ((uint32_t)tx >= (uint32_t)w || (uint32_t)ty >= (uint32_t)h)
                    continue;
                unsigned idx = obj_texel(objv, tile, (unsigned)tx, (unsigned)ty, bpp8, stride);
                obj_plot(c, x, idx, rd16(pal, pbase + idx * 2), prio, mode);
            }
        }
    }
}

/* ---- windows -------------------------------------------------------------------------------------------- */

static int win_in_v(unsigned v, int line)
{
    int y1 = (int)(v >> 8), y2 = (int)(v & 0xFF);
    return y1 <= y2 ? (line >= y1 && line < y2) : (line >= y1 || line < y2);
}

static void win_fill_h(uint8_t *mask, unsigned h, uint8_t val)
{
    unsigned x1 = h >> 8, x2 = h & 0xFF;
    if (x2 > W)
        x2 = W;
    if (x1 <= x2) {
        if (x1 < x2)
            memset(mask + x1, val, x2 - x1);
    } else {   /* wraps around: [x1, 240) and [0, x2) */
        if (x1 < W)
            memset(mask + x1, val, W - x1);
        memset(mask, val, x2);
    }
}

static void build_windows(line_ctx *c)
{
    const uint8_t *io = c->in->io;
    unsigned d = c->dispcnt;
    if (!(d & 0xE000)) {
        memset(c->winmask, 0x3F, W);
        return;
    }
    unsigned winin = rd16(io, REG_WININ), winout = rd16(io, REG_WINOUT);
    memset(c->winmask, winout & 0x3F, W);
    if (d & 0x8000) {
        uint8_t v = (uint8_t)((winout >> 8) & 0x3F);
        for (int x = 0; x < W; x++)
            if (c->objwin[x])
                c->winmask[x] = v;
    }
    if ((d & 0x4000) && win_in_v(rd16(io, REG_WIN1V), c->line))
        win_fill_h(c->winmask, rd16(io, REG_WIN1H), (uint8_t)((winin >> 8) & 0x3F));
    if ((d & 0x2000) && win_in_v(rd16(io, REG_WIN0V), c->line))
        win_fill_h(c->winmask, rd16(io, REG_WIN0H), (uint8_t)(winin & 0x3F));
}

/* ---- colour effects ------------------------------------------------------------------------------------- */

static inline unsigned blend_alpha(unsigned a, unsigned b, unsigned eva, unsigned evb)
{
    unsigned r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    unsigned g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    unsigned bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return r | (g << 5) | (bl << 10);
}

static inline unsigned brighten(unsigned a, unsigned evy)
{
    unsigned r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return r | (g << 5) | (b << 10);
}

static inline unsigned darken(unsigned a, unsigned evy)
{
    unsigned r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return r | (g << 5) | (b << 10);
}

/* ---- composition ---------------------------------------------------------------------------------------- */

static void compose(line_ctx *c, unsigned bgmask, uint16_t *out)
{
    const uint8_t *io = c->in->io;
    unsigned bldcnt = rd16(io, REG_BLDCNT), bldalpha = rd16(io, REG_BLDALPHA);
    unsigned eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = rd16(io, REG_BLDY) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    unsigned effect = (bldcnt >> 6) & 3;
    unsigned t1 = bldcnt & 0x3F, t2 = (bldcnt >> 8) & 0x3F;
    unsigned backdrop = rd16(c->in->palette, 0) & 0x7FFF;
    int obj_on = (c->dispcnt >> 12) & 1;

    /* Enabled backgrounds, front to back: lower priority value first, then lower BG number. */
    int order[4], prio[4], nbg = 0;
    for (int p = 0; p < 4; p++)
        for (int b = 0; b < 4; b++) {
            if (!(bgmask & (1u << b)))
                continue;
            int bp = (int)(rd16(io, REG_BG0CNT + 2 * b) & 3);
            if (bp == p) {
                order[nbg] = b;
                prio[nbg++] = bp;
            }
        }

    int any_semi = 0;
    if (obj_on)
        for (int x = 0; x < W && !any_semi; x++)
            any_semi = c->objsemi[x] && c->objp[x] != OBJ_NONE;
    int need_second = any_semi || effect == 1;

    /* The two front-most visible layers of each pixel (c1 / l1 in front, c2 / l2 behind it; the backdrop when there
     * are fewer), painted back to front: for each priority from 3 to 0, the backgrounds of that priority from the
     * highest number down, then the sprites of that priority (a sprite is in front of a background of its own
     * priority). A visible pixel pushes the front layer back to second place. That is the front-to-back search of
     * each pixel (lower priority first, then lower BG number, a sprite before the backgrounds of its priority) done
     * a layer at a time. The second layer only matters for blending: without it only the front one is tracked. */
    uint16_t c1[W], c2[W];
    uint8_t l1[W], l2[W];
    unsigned objprios = 0;   /* bit p: a sprite pixel of priority p is visible through the OBJ window bit */
    if (obj_on)
        for (int x = 0; x < W; x++)
            if (c->objp[x] != OBJ_NONE && (c->winmask[x] & 0x10))
                objprios |= 1u << c->objp[x];
    for (int x = 0; x < W; x++) {
        c1[x] = c2[x] = (uint16_t)backdrop;
        l1[x] = l2[x] = LAYER_BD;
    }
    for (int p = 3; p >= 0; p--) {
        for (int j = nbg - 1; j >= 0; j--) {
            if (prio[j] != p)
                continue;
            int b = order[j];
            const uint16_t *px = c->bg[b];
            if (need_second) {
                for (int x = 0; x < W; x++)
                    if ((px[x] & OPAQUE) && ((c->winmask[x] >> b) & 1)) {
                        c2[x] = c1[x];
                        l2[x] = l1[x];
                        c1[x] = px[x];
                        l1[x] = (uint8_t)b;
                    }
            } else {
                for (int x = 0; x < W; x++)
                    if ((px[x] & OPAQUE) && ((c->winmask[x] >> b) & 1)) {
                        c1[x] = px[x];
                        l1[x] = (uint8_t)b;
                    }
            }
        }
        if (!(objprios & (1u << p)))
            continue;
        for (int x = 0; x < W; x++)
            if (c->objp[x] == p && (c->winmask[x] & 0x10)) {
                c2[x] = c1[x];
                l2[x] = l1[x];
                c1[x] = c->objc[x];
                l1[x] = LAYER_OBJ;
            }
    }

    if (effect == 0 && !any_semi) {   /* no colour effect anywhere: the front layer as is */
        for (int x = 0; x < W; x++)
            out[x] = (uint16_t)(c1[x] & 0x7FFFu);
        return;
    }
    for (int x = 0; x < W; x++) {
        unsigned m = c->winmask[x];
        unsigned a = c1[x] & 0x7FFFu, l = l1[x];
        if (m & 0x20) {
            unsigned b = c2[x] & 0x7FFFu, lb = l2[x];
            if (l == LAYER_OBJ && c->objsemi[x] && ((t2 >> lb) & 1))
                a = blend_alpha(a, b, eva, evb);
            else if ((t1 >> l) & 1) {
                if (effect == 1) {
                    if ((t2 >> lb) & 1)
                        a = blend_alpha(a, b, eva, evb);
                } else if (effect == 2) {
                    a = brighten(a, evy);
                } else if (effect == 3) {
                    a = darken(a, evy);
                }
            }
        }
        out[x] = (uint16_t)a;
    }
}

/* ---- entry point ---------------------------------------------------------------------------------------- */

void gba_ppu_render_line(const gba_ppu_inputs *in, int line, uint16_t out[240])
{
    line_ctx c;
    unsigned dispcnt = rd16(in->io, REG_DISPCNT);

    if (dispcnt & 0x80) {   /* forced blank: white */
        for (int x = 0; x < W; x++)
            out[x] = 0x7FFF;
        return;
    }
    c.in = in;
    c.line = line;
    c.dispcnt = dispcnt;
    c.mosaic = rd16(in->io, REG_MOSAIC);

    unsigned mode = dispcnt & 7;
    unsigned bgmask = (dispcnt >> 8) & 15;
    static const uint8_t present[8] = {0xF, 0x7, 0xC, 0x4, 0x4, 0x4, 0, 0};
    bgmask &= present[mode];

    if (bgmask && mode != 3 && mode != 5) {
        for (unsigned i = 0; i < 256; i++)
            c.lut8[i] = (uint16_t)(rd16(in->palette, i * 2) | OPAQUE);
        memcpy(c.lut4, c.lut8, sizeof c.lut4);
        for (unsigned i = 0; i < 256; i += 16)
            c.lut4[i] = 0;
        c.lut8[0] = 0;
    }
    for (int b = 0; b < 4; b++) {
        if (!(bgmask & (1u << b)))
            continue;
        if (mode >= 3)
            render_bitmap_bg(&c, (int)mode);
        else if (mode == 0 || (mode == 1 && b < 2))
            render_text_bg(&c, b);
        else
            render_affine_bg(&c, b);
    }

    memset(c.objp, OBJ_NONE, W);
    memset(c.objwin, 0, W);
    memset(c.objsemi, 0, W);
    if (dispcnt & 0x1000)
        render_sprites(&c);

    build_windows(&c);
    compose(&c, bgmask, out);

    if (rd16(in->io, REG_GREENSWAP) & 1) {
        for (int x = 0; x < W; x += 2) {
            unsigned a = out[x], b = out[x + 1];
            out[x] = (uint16_t)((a & ~0x3E0u) | (b & 0x3E0));
            out[x + 1] = (uint16_t)((b & ~0x3E0u) | (a & 0x3E0));
        }
    }
}
