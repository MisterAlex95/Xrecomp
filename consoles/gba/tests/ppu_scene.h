/* Synthetic GBA PPU scenes for the tests and the benchmark (XBA-154): plain memory images the tests fill by hand. */
#ifndef PPU_SCENE_H
#define PPU_SCENE_H

#include "gba_ppu.h"

#include <stdint.h>
#include <string.h>

typedef struct scene {
    uint8_t io[0x400];
    uint8_t pal[0x400];
    uint8_t vram[0x18000];
    uint8_t oam[0x400];
    gba_ppu_inputs in;
} scene;

static inline void w16(uint8_t *p, unsigned off, unsigned v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
}

static inline void w32(uint8_t *p, unsigned off, uint32_t v)
{
    w16(p, off, v & 0xFFFF);
    w16(p, off + 2, v >> 16);
}

/* Every sprite disabled, every register 0, all memory 0. */
static inline void scene_init(scene *s)
{
    memset(s, 0, sizeof *s);
    for (int n = 0; n < 128; n++)
        w16(s->oam, n * 8, 0x200);
    s->in.io = s->io;
    s->in.palette = s->pal;
    s->in.vram = s->vram;
    s->in.oam = s->oam;
}

static inline void bg_color(scene *s, unsigned i, unsigned c) { w16(s->pal, i * 2, c); }
static inline void obj_color(scene *s, unsigned i, unsigned c) { w16(s->pal, 0x200 + i * 2, c); }

/* A solid 4bpp tile of colour index `idx` at byte address `addr`. */
static inline void tile4_solid(scene *s, unsigned addr, unsigned idx)
{
    memset(s->vram + addr, (int)(idx | (idx << 4)), 32);
}

static inline void tile8_solid(scene *s, unsigned addr, unsigned idx)
{
    memset(s->vram + addr, (int)idx, 64);
}

/* One 4bpp texel. */
static inline void tile4_px(scene *s, unsigned addr, unsigned x, unsigned y, unsigned idx)
{
    uint8_t *b = s->vram + addr + y * 4 + x / 2;
    *b = (x & 1) ? (uint8_t)((*b & 0x0F) | (idx << 4)) : (uint8_t)((*b & 0xF0) | idx);
}

static inline void sprite(scene *s, int n, unsigned a0, unsigned a1, unsigned a2)
{
    w16(s->oam, n * 8, a0);
    w16(s->oam, n * 8 + 2, a1);
    w16(s->oam, n * 8 + 4, a2);
}

static inline void obj_affine(scene *s, int group, int pa, int pb, int pc, int pd)
{
    w16(s->oam, group * 32 + 6, (unsigned)pa & 0xFFFF);
    w16(s->oam, group * 32 + 14, (unsigned)pb & 0xFFFF);
    w16(s->oam, group * 32 + 22, (unsigned)pc & 0xFFFF);
    w16(s->oam, group * 32 + 30, (unsigned)pd & 0xFFFF);
}

static inline uint32_t fnv1a(const void *p, size_t n, uint32_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

/* A busy mode 0 scene: 4 text BGs (two 4bpp, two 8bpp, different sizes and scroll), 128 sprites of mixed sizes and
 * priorities (some affine, some semi-transparent), alpha blending of BG0+OBJ over BG1-3. Also the benchmark load. */
static inline void scene_busy(scene *s)
{
    scene_init(s);
    uint32_t r = 12345;
#define RND() (r = r * 1103515245u + 12345u, (r >> 16) & 0x7FFF)
    for (unsigned i = 0; i < 512; i++)
        w16(s->pal, i * 2, RND());
    for (unsigned i = 0; i < 0x18000; i++)
        s->vram[i] = (uint8_t)RND();
    /* BG maps live in screenblocks 24-31 (0xC000-0xFFFF); tiles in char blocks 0-2. */
    w16(s->io, 0x08, 0x0000 | (24 << 8) | (1 << 14) | 0);          /* BG0 4bpp 512x256, prio 0 */
    w16(s->io, 0x0A, 0x0004 | (26 << 8) | (0 << 14) | 1 | 0x80);   /* BG1 8bpp 256x256, prio 1 */
    w16(s->io, 0x0C, 0x0008 | (28 << 8) | (2 << 14) | 2);          /* BG2 4bpp 256x512, prio 2 */
    w16(s->io, 0x0E, 0x0000 | (30 << 8) | (0 << 14) | 3 | 0x80);   /* BG3 8bpp 256x256, prio 3 */
    for (unsigned i = 0; i < 4; i++) {
        w16(s->io, 0x10 + 4 * i, 37 * i + 5);
        w16(s->io, 0x12 + 4 * i, 11 * i + 3);
    }
    for (unsigned i = 0; i < 0x4000; i += 2)   /* tile numbers limited so 8bpp tiles stay below 0x10000 */
        w16(s->vram, 0xC000 + i, (RND() & 0xFCFF) & ~0x300u);
    for (int n = 0; n < 128; n++) {
        unsigned shape = RND() % 3;
        unsigned size = RND() % 2;
        unsigned a0 = RND() % 160;
        unsigned a1 = RND() % 240;
        a0 |= shape << 14;
        a1 |= size << 14;
        if (n % 4 == 0) {
            a0 |= 0x100;
            a1 |= (unsigned)(n / 4 % 32) << 9;
        }
        if (n % 5 == 0)
            a0 |= 0x400;   /* semi-transparent */
        if (n % 3 == 0)
            a0 |= 0x2000;  /* 8bpp */
        unsigned tile = RND() & 0x3FF, bank = RND() & 15;
        sprite(s, n, a0, a1, tile | ((unsigned)(n & 3) << 10) | (bank << 12));
    }
    for (int g = 0; g < 32; g++)
        obj_affine(s, g, 0x100 - g * 4, g * 3, -g * 3, 0x100 + g * 2);
    w16(s->io, 0x00, 0x1F00 | 0x40);                 /* mode 0, BG0-3, OBJ, 1D */
    w16(s->io, 0x50, 0x0011 | 0x40 | (0x2E << 8));   /* alpha: BG0+OBJ over BG1-3+BD */
    w16(s->io, 0x52, 0x0A06);
#undef RND
}

#endif
