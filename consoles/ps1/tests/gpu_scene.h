/* Helpers shared by the GPU tests and the micro benchmark (XBA-165): GP0 packet builders and a deterministic busy
 * scene (a few hundred mixed primitives, the kind of frame a 3D game sends). */
#ifndef GPU_SCENE_H
#define GPU_SCENE_H

#include "ps1_devices.h"

#include <stdint.h>

static inline uint32_t XY(int x, int y) { return (uint32_t)(uint16_t)y << 16 | (uint16_t)x; }
static inline uint32_t UV(int u, int v, int attr)
{
    return (uint32_t)attr << 16 | (uint32_t)(v & 0xFF) << 8 | (uint32_t)(u & 0xFF);
}
static inline uint32_t RGB(int r, int g, int b) { return (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r; }
static inline uint16_t P15(int r, int g, int b) { return (uint16_t)(r | g << 5 | b << 10); }
static inline uint32_t TPAGE(int x64, int y256, int semi, int depth) { return (uint32_t)(x64 | y256 << 4 | semi << 5 | depth << 7); }
static inline uint32_t CLUT(int x, int y) { return (uint32_t)(x / 16 | y << 6); }

static inline void gp0n(ps1_gpu *g, const uint32_t *w, int n)
{
    for (int i = 0; i < n; i++)
        ps1_gpu_gp0(g, w[i]);
}
#define GP0(g, ...)                                                                                                \
    do {                                                                                                          \
        const uint32_t w_[] = { __VA_ARGS__ };                                                                    \
        gp0n(g, w_, (int)(sizeof w_ / sizeof w_[0]));                                                             \
    } while (0)

/* Drawing area = all VRAM, offset 0, everything else as after GP1(00h). */
static inline void gpu_open(ps1_gpu *g)
{
    ps1_gpu_gp1(g, 0);
    GP0(g, 0xE3000000, 0xE4000000 | 511 << 10 | 1023, 0xE5000000);
}

/* CPU->VRAM upload of w x h pixels. */
static inline void upload(ps1_gpu *g, int x, int y, int w, int h, const uint16_t *px)
{
    GP0(g, 0xA0000000, XY(x, y), XY(w, h));
    int n = w * h;
    for (int i = 0; i < n; i += 2)
        ps1_gpu_gp0(g, px[i] | (uint32_t)(i + 1 < n ? px[i + 1] : 0) << 16);
}

static inline uint32_t fnv1a16(const uint16_t *vram, int x, int y, int w, int h)
{
    uint32_t hsh = 2166136261u;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint16_t p = vram[(y + j) * 1024 + x + i];
            hsh = (hsh ^ (p & 0xFF)) * 16777619u;
            hsh = (hsh ^ (p >> 8)) * 16777619u;
        }
    return hsh;
}

static inline uint32_t scene_rand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* Textures for the busy scene: a 4 bpp 64x64 page at (640,0) with its CLUT at (0,480), an 8 bpp 64x64 at (704,0)
 * with its CLUT at (0,481), a 15 bpp 64x64 at (768,0). */
static inline void scene_textures(ps1_gpu *g)
{
    static uint16_t buf[64 * 64];
    for (int i = 0; i < 16 * 64; i++)
        buf[i] = (uint16_t)(0x1234u * (unsigned)(i + 1) ^ (unsigned)i >> 3);
    upload(g, 640, 0, 16, 64, buf);
    for (int i = 0; i < 32 * 64; i++)
        buf[i] = (uint16_t)(0x2F1Bu * (unsigned)(i + 7));
    upload(g, 704, 0, 32, 64, buf);
    for (int i = 0; i < 64 * 64; i++)
        buf[i] = (uint16_t)(((i & 63) >> 1) | ((i >> 6) >> 1) << 5 | ((i * 7) & 31) << 10 | (i % 5 == 0 ? 0x8000 : 0));
    upload(g, 768, 0, 64, 64, buf);
    for (int i = 0; i < 256; i++)
        buf[i] = (uint16_t)(i == 0 ? 0 : (i * 0x0421u) ^ (i & 1 ? 0x8000u : 0));
    upload(g, 0, 480, 256, 1, buf);
    upload(g, 0, 481, 256, 1, buf);
}

/* One busy 320x240 frame at (0,0): background fill, then n primitives (textured gouraud and flat triangles and quads
 * in all depths, sprites, a few semi-transparent and dithered, some lines). */
static inline void scene_busy(ps1_gpu *g, int n, uint32_t seed)
{
    uint32_t s = seed;
    GP0(g, 0xE3000000, 0xE4000000 | 239 << 10 | 319, 0xE5000000, 0xE1000200, 0xE2000000, 0xE6000000);
    GP0(g, 0x02202020, XY(0, 0), XY(320, 240));
    for (int i = 0; i < n; i++) {
        uint32_t r = scene_rand(&s);
        int kind = r % 10;
        int x = (int)(scene_rand(&s) % 300), y = (int)(scene_rand(&s) % 220);
        int sz = 4 + (int)(scene_rand(&s) % 28);
        uint32_t c0 = scene_rand(&s) & 0xFFFFFF, c1 = scene_rand(&s) & 0xFFFFFF, c2 = scene_rand(&s) & 0xFFFFFF;
        int depth = (int)(scene_rand(&s) % 3);
        uint32_t tp = TPAGE(10 + depth, 0, (int)(r >> 8) & 3, depth), clut = CLUT(0, 480 + (depth == 1));
        int semi = (r >> 12) % 8 == 0 ? 2 : 0;
        switch (kind) {
        case 0: case 1: case 2: case 3: /* textured gouraud quad (POLY_GT4) */
            GP0(g, (0x3Cu | semi) << 24 | c0, XY(x, y), UV(0, 0, clut), c1, XY(x + sz, y), UV(63, 0, tp), c2,
                XY(x, y + sz), UV(0, 63, 0), c0 ^ 0x404040, XY(x + sz, y + sz), UV(63, 63, 0));
            break;
        case 4: case 5: /* textured gouraud triangle (POLY_GT3) */
            GP0(g, (0x34u | semi) << 24 | c0, XY(x, y), UV(5, 3, clut), c1, XY(x + sz, y + sz / 3), UV(60, 10, tp),
                c2, XY(x + sz / 4, y + sz), UV(20, 62, 0));
            break;
        case 6: /* flat textured quad (POLY_FT4) */
            GP0(g, (0x2Cu | semi) << 24 | c0, XY(x, y), UV(0, 0, clut), XY(x + sz, y), UV(31, 0, tp), XY(x, y + sz),
                UV(0, 31, 0), XY(x + sz, y + sz), UV(31, 31, 0));
            break;
        case 7: /* gouraud triangle (POLY_G3) */
            GP0(g, (0x30u | semi) << 24 | c0, XY(x, y), c1, XY(x + sz, y + 5), c2, XY(x + 3, y + sz));
            break;
        case 8: /* sprite (SPRT 16x16 / variable) */
            GP0(g, 0xE1000200 | tp);
            GP0(g, (0x64u | semi) << 24 | c0, XY(x, y), UV(3, 7, clut), XY(sz, sz / 2 + 1));
            break;
        default: /* gouraud line and a flat triangle */
            GP0(g, 0x50000000 | c0, XY(x, y), c1, XY(x + sz * 2, y + sz / 2));
            GP0(g, (0x20u | semi) << 24 | c2, XY(x, y + 4), XY(x + sz / 2, y + 4), XY(x + 2, y + sz / 2));
            break;
        }
    }
}

#endif
