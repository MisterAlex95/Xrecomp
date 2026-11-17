/* ps1_gpu.c -- the PlayStation GPU (Plane P-7, XBA-165): GP0 / GP1, GPUSTAT / GPUREAD, 1 MB VRAM, a software
 * rasterizer and the display output, behind the GPU part of ps1_devices.h.
 *
 * Behaviour follows psx-spx (nocash) and the hardware VRAM dumps of JaCzekanski's ps1-tests (XBA-200: their GPU
 * programs match pixel for pixel, consoles/ps1/tests ps1_jcz_gpu_*); structure is our own. Commands run as soon as their last word arrives, so the
 * GPU is never busy: GPUSTAT reports ready for commands, DMA blocks and (during a VRAM->CPU transfer) GPUREAD.
 *
 * Rasterization (hardware rules where known):
 *   - vertices are 11-bit signed, plus the 11-bit signed drawing offset; a triangle with an edge of |dx| >= 1024 or
 *     |dy| >= 512 is not drawn; a quad is the triangles (v0 v1 v2) and (v1 v2 v3), each culled on its own;
 *   - coverage samples pixel corners with the top-left rule: rows [ytop, ybottom), columns [ceil(xleft),
 *     ceil(xright)), exact integer edge walking (no right column, no bottom row, shared edges drawn once);
 *   - colours and texture coordinates are planes over the triangle, so quads show the hardware's per-triangle
 *     shading seam; the hardware's precision: gradients in 1/4096 truncated, stepped from the leftmost vertex's
 *     value plus a half;
 *   - textures 4 / 8 bpp through a CLUT and 15 bpp, texture page and window, texel 0000h transparent, bit 15 selects
 *     semi-transparency; modulation (texel * colour) / 128 clamped; the CLUT goes through the GPU's CLUT cache
 *     (kept while the CLUT position and the entries needed allow, cleared by GP0(01h)); a rectangle flipped in x
 *     starts at u0 + 1;
 *   - lines step in 32.32 with steps rounded away from zero, both ends drawn;
 *   - semi-transparency B/2+F/2, B+F, B-F, B+F/4 on 5-bit channels; dithering (4x4 matrix) for gouraud-shaded or
 *     texture-modulated polygons and every line, when GP0(E1h) bit 9 is set; rectangles never dither;
 *   - mask bit: GP0(E6h) set (bit 15 forced on written pixels) and check (pixels with bit 15 are kept);
 *   - drawing area clip (inclusive), interlaced 480-line mode skips the displayed field's lines unless GP0(E1h)
 *     bit 10 allows drawing to the displayed area.
 * Speed: spans are drawn by straight-line loops specialised per (textured, depth, raw, gouraud, semi-transparent)
 * combination, picked once per primitive; no per-pixel indirect calls.
 *
 * Internal resolution upscale (XBA-184, ps1_gpu_set_upscale), the way Beetle PSX's software renderer does it: the
 * native 1024 x 512 VRAM stays the truth (CPU and DMA reads, texture and CLUT fetches, VRAM-to-VRAM copies all see
 * it exactly as at 1x, so a game cannot tell); at N = 2 or 4 a shadow VRAM of N*1024 x N*512 is drawn too:
 *   - polygons and rectangles are rasterized a second time into the shadow with their vertices scaled by N (same
 *     rules, same span loops, the drawing area scaled), sampling textures from the native VRAM, so edges, gouraud
 *     and texture mapping get N times the detail (no PGXP: vertices stay on the native integer grid);
 *   - lines are drawn at N times the positional detail and N shadow pixels thick (a native line's width);
 *   - fills and CPU->VRAM writes set whole N x N blocks; VRAM-to-VRAM copies copy the shadow's blocks (an
 *     off-screen picture copied to the display keeps its detail); the mask bit works on each copy on its own;
 *   - the display reads the shadow: the picture is N times wider and higher (24-bit pictures, i.e. MDEC movies, are
 *     native pixels repeated).
 * The shadow is derived, never saved: a state holds the native GPU only (the same bytes as before XBA-184) and the
 * shadow is rebuilt from the native VRAM when a state loads (block copies: the detail returns with the next frame).
 * At 1x there is no shadow and nothing above runs: output and timing are those of the native renderer alone.
 * The shadow pass costs N^2 times the native one, so a big primitive's shadow rows are split between the calling
 * thread and up to three workers (ps1_gpu_set_threads; RECOMP_PS1_GPU_THREADS), rows interleaved: every pixel is
 * computed as by one thread (the same picture whatever the count), the native VRAM is not written meanwhile (the
 * workers read textures from it), and the caller waits for the bands before the next command. Idle workers spin a
 * little, then sleep. */
#include "ps1_devices.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VRAM_W 1024
#define VRAM_H 512

enum { MODE_CMD, MODE_WRITE };

/* An 8-bit channel plus dither (-4..+3) to 5 bits, clamped, by one table load: index value + SAT5_BIAS, value from
 * -1 - 4 (gouraud rounding) up to 31 * 255 / 16 + 3 (modulation). */
#define SAT5_BIAS 16
#define SAT5_SIZE 528 /* GP0 state: command words, or CPU->VRAM data */

struct ps1_gpu {
    ps1_irq_sink irq;

    /* Drawing state (GP0 E1h..E6h). */
    uint32_t e1;           /* draw mode word as last set (bits 0-13), polygons update bits 0-8 and 11 */
    uint32_t tex_window;   /* E2h bits 0-19 */
    uint32_t area_tl;      /* E3h bits 0-19 */
    uint32_t area_br;      /* E4h bits 0-19 */
    uint32_t offset_raw;   /* E5h bits 0-21 */
    int area_x1, area_y1, area_x2, area_y2;
    int off_x, off_y;
    int set_mask, check_mask;
    int tex_disable_allowed; /* GP1(09h) */

    /* Display state (GP1). */
    int display_off;
    int dma_dir;
    int disp_x, disp_y;
    int hrange_x1, hrange_x2, vrange_y1, vrange_y2;
    uint32_t dmode; /* GP1(08h) bits 0-7 */
    int irq_flag;

    /* Beam (ps1_gpu_scanline). */
    int field;      /* interlace field: 1 = odd */
    int odd_line;   /* GPUSTAT bit 31 */
    int in_vblank;

    /* GP0 command collection. */
    uint32_t cmd[16];
    int cmd_len, cmd_need;
    int mode;

    /* Polyline collection: active, gouraud, semi, words wanted next (colour or vertex), previous vertex. */
    int pl_active, pl_gouraud, pl_semi, pl_count, pl_expect_color;
    uint32_t pl_color, pl_next_color;
    int pl_x, pl_y;
    uint32_t pl_prev_color;

    /* CPU->VRAM transfer. */
    int wr_x, wr_y, wr_w, wr_h, wr_i;
    uint32_t wr_left; /* pixels left */
    /* VRAM->CPU transfer. */
    int rd_active, rd_x, rd_y, rd_w, rd_h, rd_i;
    uint32_t rd_left;
    uint32_t read_latch;

    uint8_t sat5[SAT5_SIZE]; /* clamp(i - SAT5_BIAS, 0, 255) >> 3 */
    uint16_t vram[VRAM_W * VRAM_H];

    /* Upscale (XBA-184): host side, not state (everything above is; GPU_STATE_SIZE). */
    uint16_t *hi;  /* (VRAM_W << up) x (VRAM_H << up) shadow; NULL at 1x */
    int up;        /* log2 of the factor: 0, 1 or 2 */
    int threads;   /* workers wanted for the shadow pass (-1: by the host's cores) */
    struct gpu_pool *pool; /* those workers, while upscaled */

    /* The CLUT cache (XBA-200): the palette a 4 / 8 bpp primitive loaded, kept until a primitive needs another CLUT
     * position or more entries (4 bpp loads 16, 8 bpp 256) or GP0(01h) clears it; VRAM writes do not reach it
     * (ps1-tests gpu/clut-cache's hardware dump). Outside the state (states keep their size): a state loaded starts
     * with it empty, the next primitive loads it from VRAM. */
    uint16_t clut_cache[256];
    uint32_t clut_tag; /* the CLUT attribute loaded */
    int clut_n;        /* entries loaded: 0 (empty), 16 or 256 */
};

/* A state is the GPU up to the shadow: the size it had before XBA-184 (vram ends where the struct's alignment puts
 * the shadow pointer, as its tail padding did), so states saved by 0.3.0 still load. */
#define GPU_STATE_SIZE offsetof(ps1_gpu, hi)

/* Where a primitive is rasterized: the native VRAM, or the shadow (shift = up), with the drawing area in its pixels
 * (x2 / y2 exclusive); a shadow pass worker draws the rows y with y % parts == part only. */
typedef struct target {
    uint16_t *base;
    int stride, shift;
    int x1, y1, x2, y2;
    int part, parts;
} target;

static inline target target_native(ps1_gpu *g)
{
    target t = { g->vram, VRAM_W, 0, g->area_x1, g->area_y1, g->area_x2 + 1, g->area_y2 + 1, 0, 1 };
    return t;
}

static inline target target_hi(ps1_gpu *g)
{
    int s = g->up;
    target t = { g->hi, VRAM_W << s, s, g->area_x1 << s, g->area_y1 << s, (g->area_x2 + 1) << s,
                 (g->area_y2 + 1) << s, 0, 1 };
    return t;
}

static const int8_t dither_matrix[4][4] = {
    { -4, 0, -3, 1 },
    { 2, -2, 3, -1 },
    { -3, 1, -4, 0 },
    { 3, -1, 2, -2 },
};
static const int8_t dither_none[4][4];

static inline int sext11(uint32_t v) { return (int32_t)(v << 21) >> 21; }
static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline int iabs(int v) { return v < 0 ? -v : v; }

static inline uint16_t rgb24to15(uint32_t c)
{
    return (uint16_t)(((c >> 3) & 0x1F) | ((c >> 11) & 0x1F) << 5 | ((c >> 19) & 0x1F) << 10);
}

/* ---- Semi-transparency ------------------------------------------------------------------------------------------ */
static inline uint16_t blend(uint16_t bg, uint16_t fg, int mode)
{
    int br = bg & 31, bgc = (bg >> 5) & 31, bb = (bg >> 10) & 31;
    int fr = fg & 31, fgc = (fg >> 5) & 31, fb = (fg >> 10) & 31;
    int r, g, b;
    switch (mode) {
    case 0:
        r = (br + fr) >> 1;
        g = (bgc + fgc) >> 1;
        b = (bb + fb) >> 1;
        break;
    case 1:
        r = br + fr;
        g = bgc + fgc;
        b = bb + fb;
        r = r > 31 ? 31 : r;
        g = g > 31 ? 31 : g;
        b = b > 31 ? 31 : b;
        break;
    case 2:
        r = br - fr;
        g = bgc - fgc;
        b = bb - fb;
        r = r < 0 ? 0 : r;
        g = g < 0 ? 0 : g;
        b = b < 0 ? 0 : b;
        break;
    default:
        r = br + (fr >> 2);
        g = bgc + (fgc >> 2);
        b = bb + (fb >> 2);
        r = r > 31 ? 31 : r;
        g = g > 31 ? 31 : g;
        b = b > 31 ? 31 : b;
        break;
    }
    return (uint16_t)(r | g << 5 | b << 10 | (fg & 0x8000));
}

/* ---- Per-primitive raster state and the specialised span loops -------------------------------------------------- */
typedef struct raster {
    uint16_t *vram;
    int semi_mode;
    unsigned tp_x, tp_y;
    unsigned tw_and_u, tw_or_u, tw_and_v, tw_or_v;
    int fr, fg, fb;        /* flat colour, 8-bit per channel */
    uint16_t flat15;       /* flat colour, 15-bit (untextured flat) */
    uint16_t check_mask;   /* 0x8000 when GP0(E6h) bit 1 */
    uint16_t set_mask;     /* 0x8000 when GP0(E6h) bit 0 */
    const int8_t (*dither)[4];
    const uint8_t *sat5;   /* points SAT5_BIAS into ps1_gpu.sat5 */
    int32_t drdx, dgdx, dbdx, dudx, dvdx;
    uint16_t clut[256];
} raster;

typedef void (*span_fn)(const raster *rs, uint16_t *row, int y, int x, int xe, uint32_t r, uint32_t g, uint32_t b,
                        uint32_t u, uint32_t v);

/* The texture state a span loop keeps in registers. */
typedef struct texstate {
    const uint16_t *vram;
    const uint16_t *clut;
    unsigned tp_x, tp_y, and_u, or_u, and_v, or_v;
} texstate;

static inline uint16_t fetch(const texstate *ts, uint32_t u16, uint32_t v16, int depth)
{
    unsigned u = ((u16 >> 16) & ts->and_u) | ts->or_u;
    unsigned v = ((v16 >> 16) & ts->and_v) | ts->or_v;
    const uint16_t *line = ts->vram + ((ts->tp_y + v) & (VRAM_H - 1)) * VRAM_W;
    if (depth == 0) {
        uint16_t w = line[(ts->tp_x + (u >> 2)) & (VRAM_W - 1)];
        return ts->clut[(w >> ((u & 3) * 4)) & 15];
    }
    if (depth == 1) {
        uint16_t w = line[(ts->tp_x + (u >> 1)) & (VRAM_W - 1)];
        return ts->clut[(w >> ((u & 1) * 8)) & 255];
    }
    return line[(ts->tp_x + u) & (VRAM_W - 1)];
}

/* One span loop. TEX: textured; DEPTH: 0 = 4 bpp, 1 = 8 bpp, 2 = 15 bpp; RAW: texture not modulated; GOUR: colour
 * per pixel; SEMI: semi-transparent. The constant flags fold away, leaving one tight loop per combination. */
#define SPAN_BODY(TEX, DEPTH, RAW, GOUR, SEMI)                                                                     \
    const int8_t *drow = rs->dither[y & 3];                                                                       \
    const uint16_t chk = rs->check_mask, setm = rs->set_mask;                                                     \
    const texstate ts = { rs->vram, rs->clut, rs->tp_x, rs->tp_y, rs->tw_and_u, rs->tw_or_u, rs->tw_and_v,        \
                          rs->tw_or_v };                                                                          \
    const uint8_t *sat = rs->sat5;                                                                                \
    const int fr = rs->fr, fg = rs->fg, fb = rs->fb, smode = rs->semi_mode;                                       \
    (void)ts;                                                                                                     \
    (void)fr;                                                                                                     \
    (void)fg;                                                                                                     \
    (void)fb;                                                                                                     \
    (void)smode;                                                                                                  \
    const int32_t drdx = rs->drdx, dgdx = rs->dgdx, dbdx = rs->dbdx, dudx = rs->dudx, dvdx = rs->dvdx;            \
    (void)drow;                                                                                                   \
    (void)drdx;                                                                                                   \
    (void)dgdx;                                                                                                   \
    (void)dbdx;                                                                                                   \
    (void)dudx;                                                                                                   \
    (void)dvdx;                                                                                                   \
    for (; x < xe; x++, r += GOUR ? (uint32_t)drdx : 0, g += GOUR ? (uint32_t)dgdx : 0,                            \
                   b += GOUR ? (uint32_t)dbdx : 0, u += TEX ? (uint32_t)dudx : 0, v += TEX ? (uint32_t)dvdx : 0) { \
        uint16_t *d = row + x;                                                                                    \
        uint16_t px;                                                                                              \
        if (*d & chk)                                                                                             \
            continue;                                                                                             \
        if (TEX) {                                                                                                \
            uint16_t t = fetch(&ts, u, v, DEPTH);                                                                   \
            if (!t)                                                                                               \
                continue;                                                                                         \
            if (RAW) {                                                                                            \
                px = t;                                                                                           \
            } else {                                                                                              \
                int mr = GOUR ? (int32_t)r >> 16 : fr;                                                            \
                int mg = GOUR ? (int32_t)g >> 16 : fg;                                                            \
                int mb = GOUR ? (int32_t)b >> 16 : fb;                                                            \
                int dv = drow[x & 3];                                                                             \
                unsigned cr = sat[(((t & 31) * mr) >> 4) + dv];                                                   \
                unsigned cg = sat[((((t >> 5) & 31) * mg) >> 4) + dv];                                            \
                unsigned cb = sat[((((t >> 10) & 31) * mb) >> 4) + dv];                                           \
                px = (uint16_t)(cr | cg << 5 | cb << 10 | (t & 0x8000));                                          \
            }                                                                                                     \
            if (SEMI && (t & 0x8000))                                                                             \
                px = blend(*d, px, smode);                                                                        \
        } else {                                                                                                  \
            if (GOUR) {                                                                                           \
                int dv = drow[x & 3];                                                                             \
                unsigned cr = sat[((int32_t)r >> 16) + dv];                                                       \
                unsigned cg = sat[((int32_t)g >> 16) + dv];                                                       \
                unsigned cb = sat[((int32_t)b >> 16) + dv];                                                       \
                px = (uint16_t)(cr | cg << 5 | cb << 10);                                                         \
            } else {                                                                                              \
                px = rs->flat15;                                                                                  \
            }                                                                                                     \
            if (SEMI)                                                                                             \
                px = blend(*d, px, smode) & 0x7FFF;                                                               \
        }                                                                                                         \
        *d = px | setm;                                                                                           \
    }

#define DEFINE_SPAN(NAME, TEX, DEPTH, RAW, GOUR, SEMI)                                                             \
    static void NAME(const raster *rs, uint16_t *row, int y, int x, int xe, uint32_t r, uint32_t g, uint32_t b,    \
                     uint32_t u, uint32_t v)                                                                      \
    {                                                                                                             \
        SPAN_BODY(TEX, DEPTH, RAW, GOUR, SEMI)                                                                    \
    }

/* Flat, opaque, untextured: a fill (with the mask check when it is on). */
static void span_fill(const raster *rs, uint16_t *row, int y, int x, int xe, uint32_t r, uint32_t g, uint32_t b,
                      uint32_t u, uint32_t v)
{
    (void)y, (void)r, (void)g, (void)b, (void)u, (void)v;
    uint16_t px = rs->flat15 | rs->set_mask;
    if (!rs->check_mask) {
        for (; x < xe; x++)
            row[x] = px;
    } else {
        for (; x < xe; x++)
            if (!(row[x] & 0x8000))
                row[x] = px;
    }
}

DEFINE_SPAN(span_f_s, 0, 0, 0, 0, 1)
DEFINE_SPAN(span_g, 0, 0, 0, 1, 0)
DEFINE_SPAN(span_g_s, 0, 0, 0, 1, 1)
#define DEFINE_TEX_SPANS(D)                                                                                         \
    DEFINE_SPAN(span_t##D##_raw, 1, D, 1, 0, 0)                                                                    \
    DEFINE_SPAN(span_t##D##_raw_s, 1, D, 1, 0, 1)                                                                  \
    DEFINE_SPAN(span_t##D##_f, 1, D, 0, 0, 0)                                                                      \
    DEFINE_SPAN(span_t##D##_f_s, 1, D, 0, 0, 1)                                                                    \
    DEFINE_SPAN(span_t##D##_g, 1, D, 0, 1, 0)                                                                      \
    DEFINE_SPAN(span_t##D##_g_s, 1, D, 0, 1, 1)
DEFINE_TEX_SPANS(0)
DEFINE_TEX_SPANS(1)
DEFINE_TEX_SPANS(2)

/* [semi] for untextured: [gouraud][semi]; textured: [depth][shade: 0 raw, 1 flat, 2 gouraud][semi]. */
static const span_fn spans_untex[2][2] = { { span_fill, span_f_s }, { span_g, span_g_s } };
static const span_fn spans_tex[3][3][2] = {
    { { span_t0_raw, span_t0_raw_s }, { span_t0_f, span_t0_f_s }, { span_t0_g, span_t0_g_s } },
    { { span_t1_raw, span_t1_raw_s }, { span_t1_f, span_t1_f_s }, { span_t1_g, span_t1_g_s } },
    { { span_t2_raw, span_t2_raw_s }, { span_t2_f, span_t2_f_s }, { span_t2_g, span_t2_g_s } },
};

/* ---- Primitive setup ---------------------------------------------------------------------------------------------- */
typedef struct vtx {
    int x, y;
    int r, g, b;
    int u, v;
} vtx;

typedef struct prim {
    int textured, raw, gouraud, semi, dither;
    uint32_t clut;  /* CLUT attribute (polygons / rectangles) */
} prim;

static inline int texpage_depth(const ps1_gpu *g)
{
    int d = (g->e1 >> 7) & 3;
    return d == 3 ? 2 : d;
}

/* Fills the parts of rs that come from the GPU state and the primitive; returns the span loop to use. */
static span_fn raster_setup(ps1_gpu *g, raster *rs, const prim *p, uint32_t flat_color)
{
    rs->vram = g->vram;
    rs->semi_mode = (g->e1 >> 5) & 3;
    rs->check_mask = g->check_mask ? 0x8000 : 0;
    rs->set_mask = g->set_mask ? 0x8000 : 0;
    rs->fr = flat_color & 0xFF;
    rs->fg = (flat_color >> 8) & 0xFF;
    rs->fb = (flat_color >> 16) & 0xFF;
    rs->flat15 = rgb24to15(flat_color);
    rs->dither = p->dither ? dither_matrix : dither_none;
    rs->sat5 = g->sat5 + SAT5_BIAS;
    rs->drdx = rs->dgdx = rs->dbdx = rs->dudx = rs->dvdx = 0;
    int semi = p->semi ? 1 : 0;
    if (!p->textured)
        return spans_untex[p->gouraud ? 1 : 0][semi];

    int depth = texpage_depth(g);
    rs->tp_x = (g->e1 & 15) * 64;
    rs->tp_y = ((g->e1 >> 4) & 1) * 256;
    unsigned mx = g->tex_window & 31, my = (g->tex_window >> 5) & 31;
    unsigned ox = (g->tex_window >> 10) & 31, oy = (g->tex_window >> 15) & 31;
    rs->tw_and_u = ~(mx * 8) & 0xFF;
    rs->tw_or_u = (ox & mx) * 8;
    rs->tw_and_v = ~(my * 8) & 0xFF;
    rs->tw_or_v = (oy & my) * 8;
    if (depth < 2) {
        int n = depth == 0 ? 16 : 256;
        if (g->clut_n < n || g->clut_tag != (p->clut & 0x7FFF)) {
            unsigned cx = (p->clut & 0x3F) * 16, cy = (p->clut >> 6) & 0x1FF;
            const uint16_t *src = g->vram + cy * VRAM_W;
            for (int i = 0; i < n; i++)
                g->clut_cache[i] = src[(cx + i) & (VRAM_W - 1)];
            g->clut_tag = p->clut & 0x7FFF;
            g->clut_n = n;
        }
        memcpy(rs->clut, g->clut_cache, (size_t)n * sizeof rs->clut[0]);
    }
    int shade = p->raw ? 0 : p->gouraud ? 2 : 1;
    /* Flat modulation by 808080h without dithering is the texel itself. */
    if (shade == 1 && !p->dither && rs->fr == 128 && rs->fg == 128 && rs->fb == 128)
        shade = 0;
    return spans_tex[depth][shade][semi];
}

/* Rows of the displayed field are not drawn in 480-line interlaced mode unless GP0(E1h) bit 10 allows it. */
static inline int skip_field_line(const ps1_gpu *g)
{
    return (g->dmode & 0x24) == 0x24 && !(g->e1 & 0x400);
}

/* Exact edge walk: x = ceil(N / dy) with N = xa * dy + (y - ya) * dx, kept as quotient + remainder. */
typedef struct edge {
    int x;    /* ceil of the edge's x at the current row */
    int e;    /* x * dy - N, in [0, dy) */
    int dy, qs, rs;
} edge;

static inline int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static void edge_init(edge *ed, int xa, int ya, int xb, int yb, int y)
{
    int dx = xb - xa, dy = yb - ya;
    int n = xa * dy + (y - ya) * dx;
    ed->dy = dy;
    ed->x = -floordiv(-n, dy); /* ceil */
    ed->e = ed->x * dy - n;
    ed->qs = floordiv(dx, dy);
    ed->rs = dx - ed->qs * dy;
}

static inline void edge_step(edge *ed)
{
    ed->x += ed->qs;
    ed->e -= ed->rs;
    if (ed->e < 0) {
        ed->x++;
        ed->e += ed->dy;
    }
}

static inline int32_t sat32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

/* Rasterizes a triangle (already culled) into tg. flat_forced: shade it flat with a's colour (the native pass did,
 * for a sliver). Returns 1 when it was shaded flat that way. */
static int raster_tri(ps1_gpu *g, const target *tg, const vtx *a, const vtx *b, const vtx *c, const prim *p,
                      uint32_t flat, int flat_forced)
{
    int x10 = b->x - a->x, y10 = b->y - a->y, x20 = c->x - a->x, y20 = c->y - a->y;
    int64_t area = (int64_t)x10 * y20 - (int64_t)x20 * y10;
    if (area == 0)
        return 0;

    raster rs;
    span_fn span = raster_setup(g, &rs, p, flat);
    int flattened = 0;

    /* Attribute planes: A(x, y) = base + dAdx * x + dAdy * y, the hardware's way (ps1-tests gpu/triangle and
     * gpu/uv-interpolation's VRAM dumps, XBA-200): gradients in 1/4096 truncated toward zero, the value at the
     * "core" vertex (the leftmost; ties as below) plus a half, stepped from there. Kept as 16.16 (exact: the 1/4096
     * values shifted). */
    const vtx *cv = (b->x <= a->x) ? ((c->x <= b->x) ? c : b) : (c->x < a->x) ? c : a;
    int64_t base_r = 0, base_g = 0, base_b = 0, base_u = 0, base_v = 0;
    int32_t dy_r = 0, dy_g = 0, dy_b = 0, dy_u = 0, dy_v = 0;
#define PLANE(A, DX, DY, BASE)                                                                                     \
    do {                                                                                                          \
        int d1 = b->A - a->A, d2 = c->A - a->A;                                                                   \
        DX = sat32((((int64_t)d1 * y20 - (int64_t)d2 * y10) * 4096) / area * 16);                                 \
        DY = sat32((((int64_t)x10 * d2 - (int64_t)x20 * d1) * 4096) / area * 16);                                 \
        BASE = ((int64_t)cv->A << 16) + 0x8000 - (int64_t)DX * cv->x - (int64_t)DY * cv->y;                       \
    } while (0)
    if (p->gouraud && !(p->textured && p->raw)) {
        PLANE(r, rs.drdx, dy_r, base_r);
        PLANE(g, rs.dgdx, dy_g, base_g);
        PLANE(b, rs.dbdx, dy_b, base_b);
        /* A sliver (tiny area) can need a gradient beyond 16.16: shade it flat with v0's colour rather than read
         * colours far outside 0..255. The shadow pass follows the native one's choice. */
        int32_t gr[6] = { rs.drdx, rs.dgdx, rs.dbdx, dy_r, dy_g, dy_b };
        for (int i = 0; i < 6 && !flattened; i++)
            if (flat_forced || gr[i] == INT32_MAX || gr[i] == INT32_MIN) {
                prim q = *p;
                q.gouraud = 0;
                span = raster_setup(g, &rs, &q, (uint32_t)(a->r | a->g << 8 | a->b << 16));
                flattened = 1;
            }
    }
    if (p->textured) {
        PLANE(u, rs.dudx, dy_u, base_u);
        PLANE(v, rs.dvdx, dy_v, base_v);
    }
#undef PLANE

    /* Sort by y. */
    const vtx *t = a, *m = b, *bt = c, *tmp;
    if (m->y < t->y) tmp = t, t = m, m = tmp;
    if (bt->y < m->y) tmp = m, m = bt, bt = tmp;
    if (m->y < t->y) tmp = t, t = m, m = tmp;
    if (t->y == bt->y)
        return flattened;
    /* Is the middle vertex left of the long edge t->bt? */
    int64_t side = (int64_t)(m->x - t->x) * (bt->y - t->y) - (int64_t)(bt->x - t->x) * (m->y - t->y);
    int mid_left = side < 0;

    int y0 = t->y, y1 = bt->y;
    if (y0 < tg->y1) y0 = tg->y1;
    if (y1 > tg->y2) y1 = tg->y2;
    if (y0 >= y1)
        return flattened;
    int xmin = tg->x1, xmax = tg->x2, shift = tg->shift, part = tg->part, parts = tg->parts;
    int skip_field = skip_field_line(g);

    edge lng, sh;
    edge_init(&lng, t->x, t->y, bt->x, bt->y, y0);
    int upper = y0 < m->y;
    if (upper)
        edge_init(&sh, t->x, t->y, m->x, m->y, y0);
    else
        edge_init(&sh, m->x, m->y, bt->x, bt->y, y0);

    for (int y = y0; y < y1; y++) {
        if (upper && y == m->y) {
            upper = 0;
            edge_init(&sh, m->x, m->y, bt->x, bt->y, y);
        }
        int xl = mid_left ? sh.x : lng.x, xr = mid_left ? lng.x : sh.x;
        edge_step(&lng);
        edge_step(&sh);
        if (skip_field && ((y >> shift) & 1) == g->field)
            continue;
        if (parts > 1 && y % parts != part)
            continue;
        if (xl < xmin) xl = xmin;
        if (xr > xmax) xr = xmax;
        if (xl >= xr)
            continue;
        uint32_t r = (uint32_t)(base_r + (int64_t)rs.drdx * xl + (int64_t)dy_r * y);
        uint32_t gg = (uint32_t)(base_g + (int64_t)rs.dgdx * xl + (int64_t)dy_g * y);
        uint32_t bb = (uint32_t)(base_b + (int64_t)rs.dbdx * xl + (int64_t)dy_b * y);
        uint32_t u = (uint32_t)(base_u + (int64_t)rs.dudx * xl + (int64_t)dy_u * y);
        uint32_t v = (uint32_t)(base_v + (int64_t)rs.dvdx * xl + (int64_t)dy_v * y);
        span(&rs, tg->base + (size_t)y * (size_t)tg->stride, y, xl, xr, r, gg, bb, u, v);
    }
    return flattened;
}

static int pool_tri(ps1_gpu *g, const vtx *a, const vtx *b, const vtx *c, const prim *p, uint32_t flat, int ff);

static void draw_triangle(ps1_gpu *g, const vtx *a, const vtx *b, const vtx *c, const prim *p, uint32_t flat)
{
    if (iabs(a->x - b->x) >= 1024 || iabs(b->x - c->x) >= 1024 || iabs(a->x - c->x) >= 1024 ||
        iabs(a->y - b->y) >= 512 || iabs(b->y - c->y) >= 512 || iabs(a->y - c->y) >= 512)
        return;
    target tn = target_native(g);
    int flattened = raster_tri(g, &tn, a, b, c, p, flat, 0);
    if (g->hi) { /* the shadow: the same triangle, its vertices scaled */
        int n = 1 << g->up;
        vtx sa = *a, sb = *b, sc = *c;
        sa.x *= n, sa.y *= n;
        sb.x *= n, sb.y *= n;
        sc.x *= n, sc.y *= n;
        if (!pool_tri(g, &sa, &sb, &sc, p, flat, flattened)) {
            target th = target_hi(g);
            raster_tri(g, &th, &sa, &sb, &sc, p, flat, flattened);
        }
    }
}

/* ---- GP0 commands ------------------------------------------------------------------------------------------------- */
static void set_texpage(ps1_gpu *g, uint32_t attr)
{
    g->e1 = (g->e1 & ~0x9FFu) | (attr & 0x1FF);
    if (g->tex_disable_allowed)
        g->e1 |= attr & 0x800;
}

static inline int textures_off(const ps1_gpu *g) { return g->tex_disable_allowed && (g->e1 & 0x800); }

static void cmd_polygon(ps1_gpu *g)
{
    uint32_t op = g->cmd[0] >> 24;
    int n = (op & 8) ? 4 : 3, tex = (op & 4) != 0, gour = (op & 0x10) != 0;
    vtx v[4];
    prim p = { 0 };
    int idx = 1;
    uint32_t color = g->cmd[0] & 0xFFFFFF;
    for (int i = 0; i < n; i++) {
        uint32_t c = (i > 0 && gour) ? g->cmd[idx++] : color;
        uint32_t xy = g->cmd[idx++];
        v[i].x = sext11(xy) + g->off_x;
        v[i].y = sext11(xy >> 16) + g->off_y;
        v[i].r = c & 0xFF;
        v[i].g = (c >> 8) & 0xFF;
        v[i].b = (c >> 16) & 0xFF;
        v[i].u = v[i].v = 0;
        if (tex) {
            uint32_t uv = g->cmd[idx++];
            v[i].u = uv & 0xFF;
            v[i].v = (uv >> 8) & 0xFF;
            if (i == 0)
                p.clut = uv >> 16;
            else if (i == 1)
                set_texpage(g, uv >> 16);
        }
    }
    if (tex && textures_off(g))
        tex = 0;
    p.textured = tex;
    p.raw = tex && (op & 1);
    p.gouraud = gour;
    p.semi = (op & 2) != 0;
    p.dither = (g->e1 & 0x200) && ((gour && !p.raw) || (tex && !p.raw));
    draw_triangle(g, &v[0], &v[1], &v[2], &p, color);
    if (n == 4)
        draw_triangle(g, &v[1], &v[2], &v[3], &p, color);
}

/* A rectangle (native position x, y, size w x h, texture origin u0 v0) into tg: N x N shadow pixels per texel at
 * N x, texture coordinates stepped by 1/N (flipped: from the texel's far edge, so each texel still covers N). */
static void raster_rect(ps1_gpu *g, const target *tg, raster *rs, span_fn span, int x, int y, int w, int h, int u0,
                        int v0)
{
    int s = tg->shift, n = 1 << s;
    int flipx = (g->e1 >> 12) & 1, flipy = (g->e1 >> 13) & 1;
    int32_t step = 65536 >> s, bias = 65536 - step; /* bias: 0 at 1x */
    rs->dudx = flipx ? -step : step;

    int rx = x * n, ry = y * n;
    int x0 = rx, y0 = ry, x1 = (x + w) * n, y1 = (y + h) * n;
    if (x0 < tg->x1) x0 = tg->x1;
    if (y0 < tg->y1) y0 = tg->y1;
    if (x1 > tg->x2) x1 = tg->x2;
    if (y1 > tg->y2) y1 = tg->y2;
    if (x0 >= x1 || y0 >= y1)
        return;
    int skip_field = skip_field_line(g);
    uint32_t k = (uint32_t)(x0 - rx);
    /* x-flipped, the hardware starts one texel right of u0 (u0 + 1, u0, u0 - 1, ...; y-flipped: v0, v0 - 1, ...):
     * ps1-tests gpu/texture-flip's hardware VRAM dump (XBA-200) */
    uint32_t u = ((uint32_t)(u0 + flipx) << 16) +
                 (flipx ? (uint32_t)bias - k * (uint32_t)step : k * (uint32_t)step);
    for (int yy = y0; yy < y1; yy++) {
        if (skip_field && ((yy >> s) & 1) == g->field)
            continue;
        if (tg->parts > 1 && yy % tg->parts != tg->part)
            continue;
        uint32_t j = (uint32_t)(yy - ry);
        uint32_t vv = ((uint32_t)v0 << 16) + (flipy ? (uint32_t)bias - j * (uint32_t)step : j * (uint32_t)step);
        span(rs, tg->base + (size_t)yy * (size_t)tg->stride, yy, x0, x1, 0, 0, 0, u, vv);
    }
}

/* ---- Shadow pass workers ------------------------------------------------------------------------------------- */
#define POOL_MAX 3
#define POOL_SPIN 20000        /* polls before an idle worker sleeps (some tens of microseconds) */
#define POOL_MIN_PIXELS 1000  /* smaller primitives: the caller alone (a hand-off costs about a microsecond) */

enum { JOB_TRI, JOB_RECT };

typedef struct job {
    int kind;
    vtx a, b, c;
    prim p;
    uint32_t flat;
    int flat_forced;
    raster rs; /* rectangles */
    span_fn span;
    int x, y, w, h, u0, v0;
} job;

typedef struct gpu_pool {
    ps1_gpu *g;
    int n;
    pthread_t th[POOL_MAX];
    atomic_uint gen;  /* a new job */
    atomic_int done;  /* workers done with it */
    atomic_int quit;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int sleepers;     /* under mu */
    job jb;
} gpu_pool;

typedef struct worker_arg {
    gpu_pool *pool;
    int index;
    ps1_gpu_thread_hook hook; /* as set when the thread was made: the same one hears it end */
    void *hook_user;
} worker_arg;

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

/* Share `part` of `parts` of the job: every parts-th row (rows interleaved, so a triangle's wide and narrow rows are
 * shared out evenly). */
static void job_run(ps1_gpu *g, const job *jb, int part, int parts)
{
    target t = target_hi(g);
    t.part = part;
    t.parts = parts;
    if (jb->kind == JOB_TRI) {
        raster_tri(g, &t, &jb->a, &jb->b, &jb->c, &jb->p, jb->flat, jb->flat_forced);
    } else {
        raster rs = jb->rs;
        raster_rect(g, &t, &rs, jb->span, jb->x, jb->y, jb->w, jb->h, jb->u0, jb->v0);
    }
}

static ps1_gpu_thread_hook s_thread_hook; /* ps1_gpu_set_thread_hook */
static void *s_thread_hook_user;

void ps1_gpu_set_thread_hook(ps1_gpu_thread_hook fn, void *user)
{
    s_thread_hook = fn;
    s_thread_hook_user = user;
}

static void *pool_loop(worker_arg wa);

static void *pool_worker(void *arg)
{
    worker_arg wa = *(worker_arg *)arg;
    free(arg);
    if (wa.hook)
        wa.hook(1, wa.hook_user);
    pool_loop(wa);
    if (wa.hook)
        wa.hook(0, wa.hook_user);
    return NULL;
}

static void *pool_loop(worker_arg wa)
{
    gpu_pool *pool = wa.pool;
    unsigned seen = 0; /* the generation the pool started at: a job published before this thread ran is not missed */
    for (;;) {
        unsigned gen;
        int spins = 0;
        while ((gen = atomic_load_explicit(&pool->gen, memory_order_acquire)) == seen) {
            if (atomic_load_explicit(&pool->quit, memory_order_relaxed))
                return NULL;
            if (++spins < POOL_SPIN) {
                cpu_relax();
                continue;
            }
            pthread_mutex_lock(&pool->mu);
            pool->sleepers++;
            while (atomic_load_explicit(&pool->gen, memory_order_acquire) == seen &&
                   !atomic_load_explicit(&pool->quit, memory_order_relaxed))
                pthread_cond_wait(&pool->cv, &pool->mu);
            pool->sleepers--;
            pthread_mutex_unlock(&pool->mu);
            spins = 0;
        }
        seen = gen;
        if (atomic_load_explicit(&pool->quit, memory_order_relaxed))
            return NULL;
        job_run(pool->g, &pool->jb, wa.index + 1, pool->n + 1);
        atomic_fetch_add_explicit(&pool->done, 1, memory_order_release);
    }
}

static void pool_stop(ps1_gpu *g)
{
    gpu_pool *pool = g->pool;
    if (!pool)
        return;
    atomic_store(&pool->quit, 1);
    pthread_mutex_lock(&pool->mu);
    pthread_cond_broadcast(&pool->cv);
    pthread_mutex_unlock(&pool->mu);
    for (int i = 0; i < pool->n; i++)
        pthread_join(pool->th[i], NULL);
    pthread_cond_destroy(&pool->cv);
    pthread_mutex_destroy(&pool->mu);
    free(pool);
    g->pool = NULL;
}

/* Workers for the shadow pass: g->threads, or by the host's cores (3 from 8 cores, 2 from 4, 1 from 2). */
static void pool_start(ps1_gpu *g)
{
    pool_stop(g);
    int n = g->threads;
    if (n < 0) {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        n = cores >= 8 ? 3 : cores >= 4 ? 2 : cores >= 2 ? 1 : 0;
    }
    if (n > POOL_MAX)
        n = POOL_MAX;
    if (!g->hi || n <= 0)
        return;
    gpu_pool *pool = calloc(1, sizeof *pool);
    if (!pool)
        return;
    pool->g = g;
    pthread_mutex_init(&pool->mu, NULL);
    pthread_cond_init(&pool->cv, NULL);
    atomic_init(&pool->gen, 0);
    atomic_init(&pool->done, 0);
    atomic_init(&pool->quit, 0);
    for (int i = 0; i < n; i++) {
        worker_arg *wa = malloc(sizeof *wa);
        if (!wa)
            break;
        wa->pool = pool;
        wa->index = i;
        wa->hook = s_thread_hook;
        wa->hook_user = s_thread_hook_user;
        if (pthread_create(&pool->th[i], NULL, pool_worker, wa) != 0) {
            free(wa);
            break;
        }
        pool->n++;
    }
    g->pool = pool;
    if (!pool->n)
        pool_stop(g);
}

/* Runs pool->jb on every share (the caller's is share 0) and waits for them all. */
static void pool_run(gpu_pool *pool)
{
    atomic_store_explicit(&pool->done, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&pool->gen, 1, memory_order_release);
    pthread_mutex_lock(&pool->mu);
    if (pool->sleepers)
        pthread_cond_broadcast(&pool->cv);
    pthread_mutex_unlock(&pool->mu);
    job_run(pool->g, &pool->jb, 0, pool->n + 1);
    while (atomic_load_explicit(&pool->done, memory_order_acquire) < pool->n)
        cpu_relax();
}

/* The shadow pass of a triangle (scaled vertices) on the workers when it is big enough; 0: the caller draws it. */
static int pool_tri(ps1_gpu *g, const vtx *a, const vtx *b, const vtx *c, const prim *p, uint32_t flat, int ff)
{
    gpu_pool *pool = g->pool;
    if (!pool)
        return 0;
    int ya = a->y < b->y ? a->y : b->y, yb = a->y > b->y ? a->y : b->y;
    int xa = a->x < b->x ? a->x : b->x, xb = a->x > b->x ? a->x : b->x;
    if (c->y < ya) ya = c->y;
    if (c->y > yb) yb = c->y;
    if (c->x < xa) xa = c->x;
    if (c->x > xb) xb = c->x;
    target t = target_hi(g);
    if (ya < t.y1) ya = t.y1;
    if (yb > t.y2) yb = t.y2;
    if (xa < t.x1) xa = t.x1;
    if (xb > t.x2) xb = t.x2;
    if (yb - ya < 2 * (pool->n + 1) || (int64_t)(yb - ya) * (xb - xa) / 2 < POOL_MIN_PIXELS)
        return 0;
    job *jb = &pool->jb;
    jb->kind = JOB_TRI;
    jb->a = *a;
    jb->b = *b;
    jb->c = *c;
    jb->p = *p;
    jb->flat = flat;
    jb->flat_forced = ff;
    pool_run(pool);
    return 1;
}

/* The same for a rectangle (native position and size). */
static int pool_rect(ps1_gpu *g, const raster *rs, span_fn span, int x, int y, int w, int h, int u0, int v0)
{
    gpu_pool *pool = g->pool;
    if (!pool)
        return 0;
    target t = target_hi(g);
    int n = 1 << g->up;
    int ya = y * n, yb = (y + h) * n, xa = x * n, xb = (x + w) * n;
    if (ya < t.y1) ya = t.y1;
    if (yb > t.y2) yb = t.y2;
    if (xa < t.x1) xa = t.x1;
    if (xb > t.x2) xb = t.x2;
    if (yb - ya < 2 * (pool->n + 1) || (int64_t)(yb - ya) * (xb - xa) < POOL_MIN_PIXELS)
        return 0;
    job *jb = &pool->jb;
    jb->kind = JOB_RECT;
    jb->rs = *rs;
    jb->span = span;
    jb->x = x;
    jb->y = y;
    jb->w = w;
    jb->h = h;
    jb->u0 = u0;
    jb->v0 = v0;
    pool_run(pool);
    return 1;
}

static void cmd_rect(ps1_gpu *g)
{
    uint32_t op = g->cmd[0] >> 24;
    int tex = (op & 4) != 0, size = (op >> 3) & 3;
    int idx = 1;
    uint32_t color = g->cmd[0] & 0xFFFFFF;
    uint32_t xy = g->cmd[idx++];
    int x = sext11(xy) + g->off_x, y = sext11(xy >> 16) + g->off_y;
    prim p = { 0 };
    int u0 = 0, v0 = 0;
    if (tex) {
        uint32_t uv = g->cmd[idx++];
        u0 = uv & 0xFF;
        v0 = (uv >> 8) & 0xFF;
        p.clut = uv >> 16;
    }
    int w, h;
    if (size == 0) {
        uint32_t wh = g->cmd[idx++];
        w = wh & 0x3FF;
        h = (wh >> 16) & 0x1FF;
    } else {
        w = h = size == 1 ? 1 : size == 2 ? 8 : 16;
    }
    if (tex && textures_off(g))
        tex = 0;
    p.textured = tex;
    p.raw = tex && (op & 1);
    p.semi = (op & 2) != 0;
    raster rs;
    span_fn span = raster_setup(g, &rs, &p, color);
    target tn = target_native(g);
    raster_rect(g, &tn, &rs, span, x, y, w, h, u0, v0);
    if (g->hi && !pool_rect(g, &rs, span, x, y, w, h, u0, v0)) {
        target th = target_hi(g);
        raster_rect(g, &th, &rs, span, x, y, w, h, u0, v0);
    }
}

/* A line in the shadow (native ends, already culled): from the first end's N x N block to the last's along the major
 * axis, stepped per shadow pixel, N pixels thick across it (a native line's width, N times the positional detail). */
static void line_hi(ps1_gpu *g, int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int gour, int semi)
{
    int s = g->up, nn = 1 << s;
    int dx = x1 - x0, dy = y1 - y0, xmajor = iabs(dx) >= iabs(dy);
    int sx0 = x0 * nn, sy0 = y0 * nn, sx1 = x1 * nn, sy1 = y1 * nn;
    if (xmajor) {
        if (dx >= 0) sx1 += nn - 1; else sx0 += nn - 1;
    } else {
        if (dy >= 0) sy1 += nn - 1; else sy0 += nn - 1;
    }
    dx = sx1 - sx0;
    dy = sy1 - sy0;
    int n = xmajor ? iabs(dx) : iabs(dy);
    const int64_t one = 4294967296LL; /* 32.32 */
    int64_t xs = sx0 * one + one / 2, ys = sy0 * one + one / 2;
    int64_t xstep = 0, ystep = 0;
    int32_t r = (int32_t)((c0 & 0xFF) << 16) + 0x8000, gg = (int32_t)(((c0 >> 8) & 0xFF) << 16) + 0x8000;
    int32_t b = (int32_t)(((c0 >> 16) & 0xFF) << 16) + 0x8000;
    int32_t rstep = 0, gstep = 0, bstep = 0;
    if (n) {
        xstep = dx * one / n;
        ystep = dy * one / n;
        if (xstep < 0) xs--;
        if (ystep < 0) ys--;
        if (gour) {
            rstep = ((int)(c1 & 0xFF) - (int)(c0 & 0xFF)) * 65536 / n;
            gstep = ((int)((c1 >> 8) & 0xFF) - (int)((c0 >> 8) & 0xFF)) * 65536 / n;
            bstep = ((int)((c1 >> 16) & 0xFF) - (int)((c0 >> 16) & 0xFF)) * 65536 / n;
        }
    }
    const int8_t (*dm)[4] = (g->e1 & 0x200) ? dither_matrix : dither_none; /* flat lines too */
    int mode = (g->e1 >> 5) & 3;
    uint16_t chk = g->check_mask ? 0x8000 : 0, setm = g->set_mask ? 0x8000 : 0;
    int skip_field = skip_field_line(g);
    target th = target_hi(g);
    for (int k = 0; k <= n; k++, xs += xstep, ys += ystep, r += rstep, gg += gstep, b += bstep) {
        int bx = (int)(xs >> 32), by = (int)(ys >> 32);
        for (int t = 0; t < nn; t++) {
            int x = xmajor ? bx : bx + t, y = xmajor ? by + t : by;
            if (x < th.x1 || x >= th.x2 || y < th.y1 || y >= th.y2)
                continue;
            if (skip_field && ((y >> s) & 1) == g->field)
                continue;
            uint16_t *d = th.base + (size_t)y * (size_t)th.stride + x;
            if (*d & chk)
                continue;
            int dv = dm[y & 3][x & 3];
            int cr = clamp255((r >> 16) + dv), cg = clamp255((gg >> 16) + dv), cb = clamp255((b >> 16) + dv);
            uint16_t px = (uint16_t)((cr >> 3) | (cg >> 3) << 5 | (cb >> 3) << 10);
            if (semi)
                px = blend(*d, px, mode) & 0x7FFF;
            *d = px | setm;
        }
    }
}

/* A line's position step: delta / k in 32.32, rounded away from zero (the hardware's: a line from y to y + 1 over
 * 80 pixels steps down at its exact middle; ps1-tests gpu/lines, XBA-200). */
static inline int64_t line_step(int delta, int k)
{
    int64_t d = (int64_t)delta * 4294967296LL;
    if (d < 0)
        d -= k - 1;
    else if (d > 0)
        d += k - 1;
    return d / k;
}

/* Lines: max(|dx|, |dy|) + 1 pixels, both ends included. Positions in 32.32 from the pixel centre (less 1024 in x,
 * and in y when it decreases), colours in 20.12 from the half; steps rounded as the hardware does (gpu/lines' VRAM
 * dump matches pixel for pixel). */
static void draw_line(ps1_gpu *g, int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int gour, int semi)
{
    int dx = x1 - x0, dy = y1 - y0;
    if (iabs(dx) >= 1024 || iabs(dy) >= 512)
        return;
    int n = iabs(dx) > iabs(dy) ? iabs(dx) : iabs(dy);
    const int64_t half = 2147483648LL;
    int64_t xs = x0 * 4294967296LL + half, ys = y0 * 4294967296LL + half;
    int64_t xstep = 0, ystep = 0;
    int32_t r = (int32_t)((c0 & 0xFF) << 12) | 0x800, gg = (int32_t)(((c0 >> 8) & 0xFF) << 12) | 0x800;
    int32_t b = (int32_t)(((c0 >> 16) & 0xFF) << 12) | 0x800;
    int32_t rstep = 0, gstep = 0, bstep = 0;
    if (n) {
        xstep = line_step(dx, n);
        ystep = line_step(dy, n);
        if (gour) {
            rstep = ((int)(c1 & 0xFF) - (int)(c0 & 0xFF)) * 4096 / n;
            gstep = ((int)((c1 >> 8) & 0xFF) - (int)((c0 >> 8) & 0xFF)) * 4096 / n;
            bstep = ((int)((c1 >> 16) & 0xFF) - (int)((c0 >> 16) & 0xFF)) * 4096 / n;
        }
    }
    xs -= 1024;
    if (ystep < 0)
        ys -= 1024;
    const int8_t (*dm)[4] = (g->e1 & 0x200) ? dither_matrix : dither_none; /* flat lines too */
    int mode = (g->e1 >> 5) & 3;
    uint16_t chk = g->check_mask ? 0x8000 : 0, setm = g->set_mask ? 0x8000 : 0;
    int skip_field = skip_field_line(g);
    for (int k = 0; k <= n; k++, xs += xstep, ys += ystep, r += rstep, gg += gstep, b += bstep) {
        int x = (int)(xs >> 32), y = (int)(ys >> 32);
        if (x < g->area_x1 || x > g->area_x2 || y < g->area_y1 || y > g->area_y2)
            continue;
        if (skip_field && (y & 1) == g->field)
            continue;
        uint16_t *d = g->vram + y * VRAM_W + x;
        if (*d & chk)
            continue;
        int dv = dm[y & 3][x & 3];
        int cr = clamp255((r >> 12) + dv), cg = clamp255((gg >> 12) + dv), cb = clamp255((b >> 12) + dv);
        uint16_t px = (uint16_t)((cr >> 3) | (cg >> 3) << 5 | (cb >> 3) << 10);
        if (semi)
            px = blend(*d, px, mode) & 0x7FFF;
        *d = px | setm;
    }
    if (g->hi)
        line_hi(g, x0, y0, c0, x1, y1, c1, gour, semi);
}

static void cmd_line(ps1_gpu *g)
{
    uint32_t op = g->cmd[0] >> 24;
    int gour = (op & 0x10) != 0;
    uint32_t c0 = g->cmd[0] & 0xFFFFFF, c1 = c0;
    uint32_t v0 = g->cmd[1], v1;
    if (gour) {
        c1 = g->cmd[2] & 0xFFFFFF;
        v1 = g->cmd[3];
    } else {
        v1 = g->cmd[2];
    }
    draw_line(g, sext11(v0) + g->off_x, sext11(v0 >> 16) + g->off_y, c0, sext11(v1) + g->off_x,
              sext11(v1 >> 16) + g->off_y, c1, gour, (op & 2) != 0);
}

/* Polyline words after the command word: [colour] vertex, repeated, until a 5xxx5xxxh terminator in place of the
 * next vertex's first word (from the third vertex on). */
static void polyline_word(ps1_gpu *g, uint32_t w)
{
    int first_word_of_vertex = !g->pl_gouraud || g->pl_expect_color;
    if (first_word_of_vertex && g->pl_count >= 2 && (w & 0xF000F000u) == 0x50005000u) {
        g->pl_active = 0;
        return;
    }
    if (g->pl_gouraud && g->pl_expect_color) {
        g->pl_next_color = w & 0xFFFFFF;
        g->pl_expect_color = 0;
        return;
    }
    int x = sext11(w) + g->off_x, y = sext11(w >> 16) + g->off_y;
    uint32_t c = g->pl_gouraud ? g->pl_next_color : g->pl_color;
    if (g->pl_count > 0)
        draw_line(g, g->pl_x, g->pl_y, g->pl_prev_color, x, y, c, g->pl_gouraud, g->pl_semi);
    g->pl_x = x;
    g->pl_y = y;
    g->pl_prev_color = c;
    g->pl_count++;
    g->pl_expect_color = g->pl_gouraud;
}

static void cmd_fill(ps1_gpu *g)
{
    uint16_t c = rgb24to15(g->cmd[0]);
    int x = g->cmd[1] & 0x3F0, y = (g->cmd[1] >> 16) & 0x1FF;
    int w = ((g->cmd[2] & 0x3FF) + 15) & ~15, h = (g->cmd[2] >> 16) & 0x1FF;
    for (int j = 0; j < h; j++) {
        uint16_t *row = g->vram + ((y + j) & (VRAM_H - 1)) * VRAM_W;
        for (int i = 0; i < w; i++)
            row[(x + i) & (VRAM_W - 1)] = c;
    }
    if (g->hi) { /* the same rectangle of the shadow */
        int sft = g->up, hw = VRAM_W << sft, hh = VRAM_H << sft;
        for (int j = 0; j < h << sft; j++) {
            uint16_t *row = g->hi + (size_t)(((y << sft) + j) & (hh - 1)) * (size_t)hw;
            for (int i = 0; i < w << sft; i++)
                row[((x << sft) + i) & (hw - 1)] = c;
        }
    }
}

/* The shadow's N x N block of native pixel (x, y) set to px. */
static inline void hi_block(ps1_gpu *g, int x, int y, uint16_t px)
{
    int sft = g->up, n = 1 << sft;
    uint16_t *d = g->hi + ((size_t)y << sft) * ((size_t)VRAM_W << sft) + ((size_t)x << sft);
    for (int j = 0; j < n; j++, d += VRAM_W << sft)
        for (int i = 0; i < n; i++)
            d[i] = px;
}

/* The whole shadow from the native VRAM (pixels repeated): when it is made, and when a state loads. */
static void hi_rebuild(ps1_gpu *g)
{
    for (int y = 0; y < VRAM_H; y++)
        for (int x = 0; x < VRAM_W; x++)
            hi_block(g, x, y, g->vram[y * VRAM_W + x]);
}

static inline void xfer_size(uint32_t wh, int *w, int *h)
{
    *w = (int)(((wh & 0x3FF) - 1) & 0x3FF) + 1;
    *h = (int)((((wh >> 16) & 0x1FF) - 1) & 0x1FF) + 1;
}

static void cmd_copy(ps1_gpu *g)
{
    int sx = g->cmd[1] & 0x3FF, sy = (g->cmd[1] >> 16) & 0x1FF;
    int dx = g->cmd[2] & 0x3FF, dy = (g->cmd[2] >> 16) & 0x1FF;
    int w, h;
    xfer_size(g->cmd[3], &w, &h);
    uint16_t chk = g->check_mask ? 0x8000 : 0, setm = g->set_mask ? 0x8000 : 0;
    uint16_t buf[VRAM_W];
    for (int j = 0; j < h; j++) {
        const uint16_t *src = g->vram + ((sy + j) & (VRAM_H - 1)) * VRAM_W;
        uint16_t *dst = g->vram + ((dy + j) & (VRAM_H - 1)) * VRAM_W;
        for (int i = 0; i < w; i++)
            buf[i] = src[(sx + i) & (VRAM_W - 1)];
        for (int i = 0; i < w; i++) {
            uint16_t *d = &dst[(dx + i) & (VRAM_W - 1)];
            if (!(*d & chk))
                *d = buf[i] | setm;
        }
    }
    if (g->hi) { /* the shadow's blocks, so a picture copied keeps its detail; its own mask bits */
        int sft = g->up, hw = VRAM_W << sft, hh = VRAM_H << sft;
        uint16_t hbuf[VRAM_W << 2];
        for (int j = 0; j < h << sft; j++) {
            const uint16_t *src = g->hi + (size_t)(((sy << sft) + j) & (hh - 1)) * (size_t)hw;
            uint16_t *dst = g->hi + (size_t)(((dy << sft) + j) & (hh - 1)) * (size_t)hw;
            for (int i = 0; i < w << sft; i++)
                hbuf[i] = src[((sx << sft) + i) & (hw - 1)];
            for (int i = 0; i < w << sft; i++) {
                uint16_t *d = &dst[((dx << sft) + i) & (hw - 1)];
                if (!(*d & chk))
                    *d = hbuf[i] | setm;
            }
        }
    }
}

static void write_pixel(ps1_gpu *g, uint16_t px)
{
    int x = (g->wr_x + g->wr_i) & (VRAM_W - 1), y = g->wr_y & (VRAM_H - 1);
    uint16_t *d = g->vram + y * VRAM_W + x;
    if (!(g->check_mask && (*d & 0x8000))) {
        *d = px | (g->set_mask ? 0x8000 : 0);
        if (g->hi)
            hi_block(g, x, y, *d);
    }
    if (++g->wr_i == g->wr_w) {
        g->wr_i = 0;
        g->wr_y++;
    }
}

static uint16_t read_pixel(ps1_gpu *g)
{
    int x = (g->rd_x + g->rd_i) & (VRAM_W - 1), y = g->rd_y & (VRAM_H - 1);
    uint16_t px = g->vram[y * VRAM_W + x];
    if (++g->rd_i == g->rd_w) {
        g->rd_i = 0;
        g->rd_y++;
    }
    return px;
}

static int cmd_words(uint32_t w)
{
    uint32_t op = w >> 24;
    switch (op >> 5) {
    case 0:
        return op == 0x02 ? 3 : 1;
    case 1: { /* polygon */
        int n = (op & 8) ? 4 : 3;
        return 1 + n * ((op & 4) ? 2 : 1) + ((op & 0x10) ? n - 1 : 0);
    }
    case 2: /* line (polylines are collected word by word) */
        return (op & 0x10) ? 4 : 3;
    case 3: /* rectangle */
        return 2 + ((op & 4) ? 1 : 0) + (((op >> 3) & 3) == 0 ? 1 : 0);
    case 4:
        return 4;
    case 5:
    case 6:
        return 3;
    default:
        return 1;
    }
}

static void cmd_env(ps1_gpu *g, uint32_t w)
{
    switch (w >> 24) {
    case 0xE1:
        g->e1 = (w & 0x37FF) | (g->tex_disable_allowed ? (w & 0x800) : 0);
        break;
    case 0xE2:
        g->tex_window = w & 0xFFFFF;
        break;
    case 0xE3:
        g->area_tl = w & 0xFFFFF;
        g->area_x1 = w & 0x3FF;
        g->area_y1 = (w >> 10) & 0x3FF;
        if (g->area_y1 > VRAM_H - 1) g->area_y1 = VRAM_H - 1;
        break;
    case 0xE4:
        g->area_br = w & 0xFFFFF;
        g->area_x2 = w & 0x3FF;
        g->area_y2 = (w >> 10) & 0x3FF;
        if (g->area_y2 > VRAM_H - 1) g->area_y2 = VRAM_H - 1;
        break;
    case 0xE5:
        g->offset_raw = w & 0x3FFFFF;
        g->off_x = sext11(w);
        g->off_y = sext11(w >> 11);
        break;
    case 0xE6:
        g->set_mask = w & 1;
        g->check_mask = (w >> 1) & 1;
        break;
    default:
        break;
    }
}

static void execute(ps1_gpu *g)
{
    uint32_t w = g->cmd[0], op = w >> 24;
    switch (op >> 5) {
    case 0:
        if (op == 0x01)
            g->clut_n = 0; /* clear cache */
        else if (op == 0x02)
            cmd_fill(g);
        else if (op == 0x1F && !g->irq_flag) {
            g->irq_flag = 1;
            if (g->irq.raise)
                g->irq.raise(g->irq.ctx, PS1_IRQ_GPU);
        }
        break;
    case 1:
        cmd_polygon(g);
        break;
    case 2:
        cmd_line(g);
        break;
    case 3:
        cmd_rect(g);
        break;
    case 4:
        cmd_copy(g);
        break;
    case 5:
        g->wr_x = g->cmd[1] & 0x3FF;
        g->wr_y = (g->cmd[1] >> 16) & 0x1FF;
        xfer_size(g->cmd[2], &g->wr_w, &g->wr_h);
        g->wr_i = 0;
        g->wr_left = (uint32_t)g->wr_w * (uint32_t)g->wr_h;
        g->mode = MODE_WRITE;
        break;
    case 6:
        g->rd_x = g->cmd[1] & 0x3FF;
        g->rd_y = (g->cmd[1] >> 16) & 0x1FF;
        xfer_size(g->cmd[2], &g->rd_w, &g->rd_h);
        g->rd_i = 0;
        g->rd_left = (uint32_t)g->rd_w * (uint32_t)g->rd_h;
        g->rd_active = 1;
        break;
    default:
        cmd_env(g, w);
        break;
    }
}

void ps1_gpu_gp0(ps1_gpu *g, uint32_t w)
{
    if (g->mode == MODE_WRITE) {
        write_pixel(g, (uint16_t)w);
        if (--g->wr_left) {
            write_pixel(g, (uint16_t)(w >> 16));
            g->wr_left--;
        }
        if (!g->wr_left)
            g->mode = MODE_CMD;
        return;
    }
    if (g->pl_active) {
        polyline_word(g, w);
        return;
    }
    if (g->cmd_len == 0) {
        uint32_t op = w >> 24;
        if ((op & 0xE8) == 0x48) { /* polyline */
            g->pl_active = 1;
            g->pl_gouraud = (op & 0x10) != 0;
            g->pl_semi = (op & 2) != 0;
            g->pl_color = w & 0xFFFFFF;
            g->pl_next_color = g->pl_color;
            g->pl_count = 0;
            g->pl_expect_color = 0; /* the first vertex's colour is in the command word */
            return;
        }
        g->cmd_need = cmd_words(w);
    }
    g->cmd[g->cmd_len++] = w;
    if (g->cmd_len < g->cmd_need)
        return;
    g->cmd_len = 0;
    execute(g);
}

void ps1_gpu_dma_write(ps1_gpu *g, const uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ps1_gpu_gp0(g, words[i]);
}

uint32_t ps1_gpu_read(ps1_gpu *g)
{
    if (g->rd_active) {
        uint32_t lo = read_pixel(g), hi = 0;
        if (--g->rd_left) {
            hi = read_pixel(g);
            g->rd_left--;
        }
        if (!g->rd_left)
            g->rd_active = 0;
        g->read_latch = lo | hi << 16;
    }
    return g->read_latch;
}

void ps1_gpu_dma_read(ps1_gpu *g, uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++)
        words[i] = ps1_gpu_read(g);
}

/* ---- GP1 ---------------------------------------------------------------------------------------------------------- */
static void reset_fifo(ps1_gpu *g)
{
    g->cmd_len = 0;
    g->pl_active = 0;
    g->mode = MODE_CMD;
    g->rd_active = 0;
}

void ps1_gpu_reset(ps1_gpu *g)
{
    reset_fifo(g);
    g->clut_n = 0;
    g->irq_flag = 0;
    g->display_off = 1;
    g->dma_dir = 0;
    g->disp_x = g->disp_y = 0;
    g->hrange_x1 = 0x200;
    g->hrange_x2 = 0x200 + 256 * 10;
    g->vrange_y1 = 0x10;
    g->vrange_y2 = 0x10 + 240;
    g->dmode = 0;
    g->tex_disable_allowed = 0;
    for (uint32_t c = 0xE1; c <= 0xE6; c++)
        cmd_env(g, c << 24);
    g->field = 0;
    g->odd_line = 0;
}

void ps1_gpu_gp1(ps1_gpu *g, uint32_t w)
{
    uint32_t op = (w >> 24) & 0x3F;
    switch (op) {
    case 0x00:
        ps1_gpu_reset(g);
        break;
    case 0x01:
        reset_fifo(g);
        break;
    case 0x02:
        g->irq_flag = 0;
        break;
    case 0x03:
        g->display_off = w & 1;
        break;
    case 0x04:
        g->dma_dir = w & 3;
        break;
    case 0x05:
        g->disp_x = w & 0x3FF;
        g->disp_y = (w >> 10) & 0x1FF;
        break;
    case 0x06:
        g->hrange_x1 = w & 0xFFF;
        g->hrange_x2 = (w >> 12) & 0xFFF;
        break;
    case 0x07:
        g->vrange_y1 = w & 0x3FF;
        g->vrange_y2 = (w >> 10) & 0x3FF;
        break;
    case 0x08:
        g->dmode = w & 0xFF;
        if (!(g->dmode & 0x20))
            g->field = 0;
        break;
    case 0x09:
        g->tex_disable_allowed = w & 1;
        break;
    default:
        if (op >= 0x10 && op <= 0x1F) {
            switch (w & 0xF) {
            case 2: g->read_latch = g->tex_window; break;
            case 3: g->read_latch = g->area_tl; break;
            case 4: g->read_latch = g->area_br; break;
            case 5: g->read_latch = g->offset_raw; break;
            case 7: g->read_latch = 2; break; /* GPU version: the 208-pin GPU (CXD8561Q and later) */
            case 8: g->read_latch = 0; break;
            default: break;
            }
        }
        break;
    }
}

uint32_t ps1_gpu_stat(ps1_gpu *g)
{
    uint32_t s = (g->e1 & 0x7FF) | (uint32_t)g->set_mask << 11 | (uint32_t)g->check_mask << 12;
    int interlaced = (g->dmode & 0x20) != 0;
    if (!interlaced || g->field)
        s |= 1u << 13;
    s |= (g->dmode & 0x80) ? 1u << 14 : 0;
    s |= (g->e1 & 0x800) ? 1u << 15 : 0;
    s |= ((g->dmode >> 6) & 1) << 16 | (g->dmode & 3) << 17 | ((g->dmode >> 2) & 1) << 19 |
         ((g->dmode >> 3) & 1) << 20 | ((g->dmode >> 4) & 1) << 21 | ((g->dmode >> 5) & 1) << 22;
    s |= (uint32_t)g->display_off << 23 | (uint32_t)g->irq_flag << 24;
    int cmd_ready = g->mode == MODE_CMD && g->cmd_len == 0 && !g->pl_active;
    int read_ready = g->rd_active;
    s |= (uint32_t)cmd_ready << 26 | (uint32_t)read_ready << 27 | 1u << 28;
    switch (g->dma_dir) {
    case 1: s |= 1u << 25; break; /* FIFO never full */
    case 2: s |= 1u << 25; break; /* = bit 28 */
    case 3: s |= (uint32_t)read_ready << 25; break;
    default: break;
    }
    s |= (uint32_t)g->dma_dir << 29 | (uint32_t)g->odd_line << 31;
    return s;
}

/* ---- Timing, beam, display ---------------------------------------------------------------------------------------- */
static int dot_divider(const ps1_gpu *g)
{
    static const int div[4] = { 10, 8, 5, 4 };
    return (g->dmode & 0x40) ? 7 : div[g->dmode & 3];
}

void ps1_gpu_get_timing(ps1_gpu *g, ps1_gpu_timing *t)
{
    t->pal = (g->dmode >> 3) & 1;
    t->interlaced = (g->dmode >> 5) & 1;
    t->dot_divider = dot_divider(g);
    t->lines = t->pal ? 314 : 263;
    t->cycles_per_line = t->pal ? 3406 : 3413;
}

void ps1_gpu_scanline(ps1_gpu *g, int line)
{
    int vblank = line < g->vrange_y1 || line >= g->vrange_y2;
    int interlaced = (g->dmode & 0x20) != 0;
    if (vblank && !g->in_vblank && interlaced)
        g->field ^= 1; /* the next field starts with the vertical blank */
    g->in_vblank = vblank;
    if (vblank)
        g->odd_line = 0;
    else if (interlaced && (g->dmode & 4))
        g->odd_line = g->field;
    else
        g->odd_line = line & 1;
}

static inline uint32_t expand15(uint16_t p)
{
    uint32_t r = p & 31, gg = (p >> 5) & 31, b = (p >> 10) & 31;
    r = r << 3 | r >> 2;
    gg = gg << 3 | gg >> 2;
    b = b << 3 | b >> 2;
    return r << 16 | gg << 8 | b;
}

/* The display area (ww x hh native pixels) from the shadow: N times wider and higher. 24-bit: the native pixels,
 * each repeated N x N (the bytes of a 24-bit picture are only meaningful at native resolution). */
static int display_hi(ps1_gpu *g, uint32_t *xrgb, int ww, int hh, int *w, int *h)
{
    int sft = g->up, n = 1 << sft, hw = VRAM_W << sft, hhi = VRAM_H << sft, ow = ww << sft;
    if (w) *w = g->display_off ? 0 : ow;
    if (h) *h = g->display_off ? 0 : hh << sft;
    if (g->display_off)
        return -1;
    if (g->dmode & 0x10) {
        for (int j = 0; j < hh; j++) {
            const uint16_t *row = g->vram + ((g->disp_y + j) & (VRAM_H - 1)) * VRAM_W;
            uint32_t *out = xrgb + ((size_t)j << sft) * (size_t)ow;
            unsigned byte = (unsigned)g->disp_x * 2;
            for (int i = 0; i < ww; i++, byte += 3) {
                uint32_t c = 0;
                for (int k = 0; k < 3; k++) {
                    unsigned bb = (byte + k) & 2047;
                    c |= (uint32_t)((row[bb >> 1] >> ((bb & 1) * 8)) & 0xFF) << (8 * k);
                }
                c = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
                for (int k = 0; k < n; k++)
                    out[(i << sft) + k] = c;
            }
            for (int k = 1; k < n; k++)
                memcpy(out + (size_t)k * (size_t)ow, out, (size_t)ow * sizeof *out);
        }
        return 0;
    }
    int x0 = g->disp_x << sft, y0 = g->disp_y << sft;
    for (int j = 0; j < hh << sft; j++) {
        const uint16_t *row = g->hi + (size_t)((y0 + j) & (hhi - 1)) * (size_t)hw;
        uint32_t *out = xrgb + (size_t)j * (size_t)ow;
        for (int i = 0; i < ow; i++)
            out[i] = expand15(row[(x0 + i) & (hw - 1)]);
    }
    return 0;
}

int ps1_gpu_display(ps1_gpu *g, uint32_t *xrgb, int max_w, int max_h, int *w, int *h)
{
    static const int nominal_w[4] = { 256, 320, 512, 640 };
    int nominal = (g->dmode & 0x40) ? 368 : nominal_w[g->dmode & 3];
    int pal = (g->dmode >> 3) & 1;
    int ww = g->hrange_x2 > g->hrange_x1 ? ((g->hrange_x2 - g->hrange_x1) / dot_divider(g) + 2) & ~3 : 0;
    if (ww < 16 || ww > nominal)
        ww = nominal;
    int max_lines = pal ? 288 : 240;
    int hh = g->vrange_y2 - g->vrange_y1;
    if (hh < 16 || hh > max_lines)
        hh = max_lines;
    if ((g->dmode & 0x24) == 0x24)
        hh *= 2;
    if (g->hi && (ww << g->up) <= max_w && (hh << g->up) <= max_h)
        return display_hi(g, xrgb, ww, hh, w, h);
    if (ww > max_w) ww = max_w;
    if (hh > max_h) hh = max_h;
    if (w) *w = g->display_off ? 0 : ww;
    if (h) *h = g->display_off ? 0 : hh;
    if (g->display_off)
        return -1;
    if (g->dmode & 0x10) { /* 24-bit: three bytes per pixel from the start's byte address */
        for (int j = 0; j < hh; j++) {
            const uint16_t *row = g->vram + ((g->disp_y + j) & (VRAM_H - 1)) * VRAM_W;
            uint32_t *out = xrgb + (size_t)j * ww;
            unsigned byte = (unsigned)g->disp_x * 2;
            for (int i = 0; i < ww; i++, byte += 3) {
                uint32_t c = 0;
                for (int k = 0; k < 3; k++) {
                    unsigned bb = (byte + k) & 2047;
                    c |= (uint32_t)((row[bb >> 1] >> ((bb & 1) * 8)) & 0xFF) << (8 * k);
                }
                out[i] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
            }
        }
    } else {
        for (int j = 0; j < hh; j++) {
            const uint16_t *row = g->vram + ((g->disp_y + j) & (VRAM_H - 1)) * VRAM_W;
            uint32_t *out = xrgb + (size_t)j * ww;
            for (int i = 0; i < ww; i++)
                out[i] = expand15(row[(g->disp_x + i) & (VRAM_W - 1)]);
        }
    }
    return 0;
}

const uint16_t *ps1_gpu_vram(ps1_gpu *g) { return g->vram; }

const uint16_t *ps1_gpu_vram_hi(ps1_gpu *g) { return g->hi ? g->hi : g->vram; }

int ps1_gpu_set_upscale(ps1_gpu *g, int factor)
{
    int up = factor >= 4 ? 2 : factor >= 2 ? 1 : 0;
    if (up == g->up && (up == 0) == (g->hi == NULL))
        return 1 << g->up;
    uint16_t *hi = NULL;
    if (up && !(hi = malloc(((size_t)VRAM_W << up) * ((size_t)VRAM_H << up) * sizeof *hi)))
        up = 0; /* no memory for it: native */
    pool_stop(g);
    free(g->hi);
    g->hi = hi;
    g->up = up;
    if (hi) {
        hi_rebuild(g);
        pool_start(g);
    }
    return 1 << up;
}

int ps1_gpu_upscale(ps1_gpu *g) { return 1 << g->up; }

int ps1_gpu_set_threads(ps1_gpu *g, int n)
{
    g->threads = n < 0 ? -1 : n > POOL_MAX ? POOL_MAX : n;
    if (g->hi)
        pool_start(g);
    return g->pool ? g->pool->n : 0;
}

ps1_gpu *ps1_gpu_create(ps1_irq_sink irq)
{
    ps1_gpu *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->irq = irq;
    g->threads = -1;
    for (int i = 0; i < SAT5_SIZE; i++)
        g->sat5[i] = (uint8_t)(clamp255(i - SAT5_BIAS) >> 3);
    ps1_gpu_reset(g);
    return g;
}

void ps1_gpu_destroy(ps1_gpu *g)
{
    if (g) {
        pool_stop(g);
        free(g->hi);
    }
    free(g);
}

/* ---- Save states (XBA-182) ------------------------------------------------------------------------------------ */

size_t ps1_gpu_state_size(ps1_gpu *g)
{
    (void)g;
    return GPU_STATE_SIZE;
}

void ps1_gpu_state_save(ps1_gpu *g, uint8_t *out)
{
    size_t n = sizeof g->irq;
    memcpy(out, g, GPU_STATE_SIZE);
    memset(out + offsetof(ps1_gpu, irq), 0, n); /* the host's sink, not state */
}

static int in_range(int v, int lo, int hi) { return v >= lo && v <= hi; }

/* The fields the GPU indexes or clips with, in the ranges its own writers keep them: the command words collected
 * (cmd[16]), the drawing area (the only clip of VRAM drawing), the offset and the polyline's last vertex (11-bit
 * coordinates plus the offset), the display start; a transfer in progress has pixels left. */
int ps1_gpu_state_valid(const uint8_t *in, size_t n)
{
    if (n != GPU_STATE_SIZE)
        return -1;
    int cmd_len, cmd_need, x1, y1, x2, y2, off_x, off_y, pl_x, pl_y, disp_x, disp_y, mode, rd_active;
    uint32_t wr_left, rd_left;
    PS1_STATE_GET(cmd_len, in, ps1_gpu, cmd_len);
    PS1_STATE_GET(cmd_need, in, ps1_gpu, cmd_need);
    PS1_STATE_GET(x1, in, ps1_gpu, area_x1);
    PS1_STATE_GET(y1, in, ps1_gpu, area_y1);
    PS1_STATE_GET(x2, in, ps1_gpu, area_x2);
    PS1_STATE_GET(y2, in, ps1_gpu, area_y2);
    PS1_STATE_GET(off_x, in, ps1_gpu, off_x);
    PS1_STATE_GET(off_y, in, ps1_gpu, off_y);
    PS1_STATE_GET(pl_x, in, ps1_gpu, pl_x);
    PS1_STATE_GET(pl_y, in, ps1_gpu, pl_y);
    PS1_STATE_GET(disp_x, in, ps1_gpu, disp_x);
    PS1_STATE_GET(disp_y, in, ps1_gpu, disp_y);
    PS1_STATE_GET(mode, in, ps1_gpu, mode);
    PS1_STATE_GET(wr_left, in, ps1_gpu, wr_left);
    PS1_STATE_GET(rd_active, in, ps1_gpu, rd_active);
    PS1_STATE_GET(rd_left, in, ps1_gpu, rd_left);
    /* cmd_len < 16 and cmd_need <= 16 keep the next word inside cmd[] (the longest command is 12 words) */
    if (!in_range(cmd_len, 0, 15) || !in_range(cmd_need, 0, 16))
        return -2;
    if (!in_range(x1, 0, VRAM_W - 1) || !in_range(x2, 0, VRAM_W - 1) || !in_range(y1, 0, VRAM_H - 1) ||
        !in_range(y2, 0, VRAM_H - 1))
        return -2;
    if (!in_range(off_x, -1024, 1023) || !in_range(off_y, -1024, 1023) || !in_range(pl_x, -2048, 2046) ||
        !in_range(pl_y, -2048, 2046))
        return -2;
    if (!in_range(disp_x, 0, VRAM_W - 1) || !in_range(disp_y, 0, VRAM_H - 1))
        return -2;
    if (!in_range(mode, MODE_CMD, MODE_WRITE) || (mode == MODE_WRITE && !wr_left) || (rd_active && !rd_left))
        return -2;
    return 0;
}

int ps1_gpu_state_load(ps1_gpu *g, const uint8_t *in, size_t n)
{
    if (n != GPU_STATE_SIZE)
        return -1;
    ps1_irq_sink irq = g->irq;
    memcpy(g, in, GPU_STATE_SIZE);
    g->irq = irq;
    g->clut_n = 0;
    if (g->hi) /* the upscale's shadow is derived: made again from the VRAM loaded */
        hi_rebuild(g);
    return 0;
}
