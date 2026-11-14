/* n64_rdp.c -- the Nintendo 64 Reality Display Processor (Plane N-5, XBA-173): DPC registers, command lists fetched
 * from RDRAM or from the RSP's DMEM (XBUS), and our own software rasterizer drawing into RDRAM, behind the RDP part of
 * n64_devices.h. Behaviour follows the n64brew wiki RDP pages and libdragon's rdpq documentation; ideas from ares
 * (ISC) and cen64 (BSD-3); the structure and the code are our own (nothing from angrylion, GLideN64, mupen64plus or
 * paraLLEl-RDP).
 *
 * Front end: START / END / CURRENT / STATUS (xbus, freeze, flush, start pending), CLOCK / BUFBUSY / PIPEBUSY / TMEM
 * counters approximated from the work done (cycles per pixel by cycle type, per TMEM word loaded). A write to END
 * queues CURRENT..END; n64_rdp_run fetches the 64-bit words, collects whole commands (a command may straddle two
 * buffers) and executes them. SYNC_FULL raises the DP interrupt; the other syncs are no-ops (we never overlap).
 *
 * Rasterizer (all primitives go through one edge walker; rectangles are walked as two vertical edges):
 *   - edges exactly as the triangle coefficients define them: YH / YM / YL in s11.2 sub-scanlines, XH and XM taken at
 *     the scan line of YH, XL at YM, slopes stepped by DxDy / 4 per sub-scanline; left- or right-major (lft); spans
 *     clipped by the scissor at quarter-pixel precision, odd / even lines dropped in interlace (field) mode;
 *   - coverage: 8 samples per pixel (4 sub-scanlines x 2 columns, odd rows offset by 1/4 pixel) for the pixels on the
 *     span ends only; inner pixels are known to be fully covered. Pixels with no sample covered are not drawn;
 *   - attributes (shade RGBA, S / T / W, Z) are planes: value at a pixel = edge value of its scan line (stepped by
 *     DaDe) + DaDx * (pixel x - major edge x), sampled at the pixel's top-left corner, stepped by DaDx along the span;
 *   - one span routine per (texture, shade, Z, 2-cycle, 32-bit) combination, picked once per primitive; inside, the
 *     rest of the state (blender, dither, filters...) are plain branches on per-primitive constants. The combiner of
 *     each cycle is resolved when the mode changes into: constant (computed once), pass-through of one input, a
 *     product of two inputs, or the generic (A - B) * C + D on input pointers. A blender that cannot change the pixel
 *     is skipped. Fill and copy modes have their own loops.
 * Pixel pipeline (1 / 2 cycle): texture (perspective divide, shift, SL / TL offset, clamp / mirror / mask, point or
 * 3-point bilinear with mid-texel, YUV convert, TLUT, LOD tile selection with detail / sharpen), combiner (2 cycles,
 * every input incl. noise, LOD fraction, K4 / K5, key centre / scale), chroma key (approximated: see key_alpha), alpha
 * compare (blend alpha or noise threshold), coverage * alpha and alpha from coverage, Z test (opaque /
 * interpenetrating / transparent / decal with the delta Z precision) on the 18-bit Z stored as 14-bit floating point +
 * 4-bit dz, blender (2 cycles, fog, memory colour and coverage, force blend or the divided blend), dither (magic square,
 * Bayer, noise) into 5:5:5:1, coverage stored per cvg_dest (clamp / wrap / zap / save) in the 16-bit pixel's low bit +
 * two hidden bits (kept here in a side array: RDRAM's 9th bits are not in n64_rcp_bus) or in the top bits of a 32-bit
 * pixel's alpha. Things approximated are listed at their code.
 *
 * Threads: optional scanline bands (n64_rdp.h). Every renderer executes every command (so each keeps its own TMEM and
 * state), draws only the rows (y / 4) % n == band, and the pixels in a row are drawn in command order: the output does
 * not depend on the thread count. The per-pixel noise is a hash of (x, y, primitive count), not a running generator,
 * for the same reason. */
#include "n64_devices.h"
#include "n64_rdp.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#define RDP_THREADS 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define ALWAYS_INLINE static inline
#endif

/* ---- State --------------------------------------------------------------------------------------------------------- */
enum { CYC_1 = 0, CYC_2 = 1, CYC_COPY = 2, CYC_FILL = 3 };
enum { FMT_RGBA = 0, FMT_YUV = 1, FMT_CI = 2, FMT_IA = 3, FMT_I = 4 };
enum { A_R, A_G, A_B, A_A, A_S, A_T, A_W, A_Z, A_N };
enum { ZM_OPAQUE = 0, ZM_INTER = 1, ZM_TRANS = 2, ZM_DECAL = 3 };
enum { CM_GENERIC, CM_CONST, CM_PASS, CM_MUL };

/* DPC_STATUS read bits. */
#define ST_XBUS 0x001u
#define ST_FREEZE 0x002u
#define ST_FLUSH 0x004u
#define ST_GCLK 0x008u
#define ST_PIPE_BUSY 0x020u
#define ST_CMD_BUSY 0x040u
#define ST_CBUF_READY 0x080u
#define ST_DMA_BUSY 0x100u
#define ST_START_PENDING 0x400u

typedef struct {
    uint8_t fmt, size, pal, ct, mt, cs, ms, mask_s, mask_t, shift_s, shift_t;
    uint16_t line; /* bytes per TMEM row */
    uint16_t tmem; /* byte address */
    uint16_t sl, tl, sh, th; /* 10.2 */
} rdp_tile;

typedef struct {
    uint8_t cycle, persp, detail, sharpen, lod_en, tlut, tlut_ia, bilinear, mid_texel, bilerp[2], convert_one, key;
    uint8_t rgb_dither, alpha_dither;
    uint8_t m1a[2], m1b[2], m2a[2], m2b[2];
    uint8_t force_blend, alpha_cvg_sel, cvg_x_alpha, z_mode, cvg_dest, color_on_cvg, image_read, z_update, z_compare;
    uint8_t aa, z_prim, dither_alpha, alpha_compare;
} rdp_modes;

/* Everything a command can change: each renderer (thread) has its own copy. */
typedef struct {
    rdp_modes om;
    uint8_t cc[2][8]; /* per cycle: RGB sub A, sub B, mul, add; alpha sub A, sub B, mul, add */
    uint32_t fill_color;
    int32_t prim[4], env[4], blend[4], fog[4];
    int32_t prim_lod_frac, min_level;
    int max_level; /* of the last triangle: texture rectangles use it too */
    int32_t k[6];
    int32_t key_center[3], key_scale[3], key_width[3];
    uint16_t prim_z, prim_dz;
    int sc_x0, sc_y0, sc_x1, sc_y1, sc_field, sc_odd; /* 10.2 */
    int ci_fmt, ci_size, ci_width;
    uint32_t ci_addr, zi_addr;
    int ti_fmt, ti_size, ti_width;
    uint32_t ti_addr;
    rdp_tile tile[8];
    uint8_t tmem[4096];
} rdp_state;

/* Combiner inputs of the pixel being drawn (the combiner reads them through pointers). */
typedef struct {
    int32_t comb[4], tex0[4], tex1[4], shade[4], prim[4], env[4], keyc[4], keys[4];
    int32_t one, zero, noise, lodfrac, primlod, k4, k5;
} rdp_cin;

/* A primitive ready for the edge walker. */
typedef struct {
    int lft, tile, level, rect;
    int32_t yh, ym, yl;                /* s11.2 sub-scanlines */
    int32_t xh, xm, xl, dxh, dxm, dxl; /* s15.16 */
    int32_t a0[A_N], dadx[A_N], dade[A_N], dady[A_N];
    uint32_t primz18;
    int dzc; /* the primitive's delta Z, compressed (log2) */
} rdp_prim;

/* One scan line of a primitive. */
typedef struct {
    int y, x0, x1;     /* pixels x0..x1 inclusive */
    int fx0, fx1;      /* [fx0, fx1): fully covered */
    int valid;         /* sub-scanlines (bit j) inside the primitive and the scissor */
    int xl8[4], xr8[4]; /* their spans, 1/8 pixel */
    int32_t a[A_N];    /* attributes at x0 */
} rdp_row;

typedef struct rdp_render rdp_render;
typedef void (*span_fn)(rdp_render *r, const rdp_prim *p, const rdp_row *row);

struct rdp_render {
    n64_rdp *dp;
    uint8_t *ram;
    uint32_t ram_size;
    uint8_t *hidden; /* 2 hidden bits per 16-bit RDRAM word */
    int band, nbands;
    rdp_state s;

    /* Derived from the state when it changes (prepare). */
    int dirty;
    rdp_cin in;
    const int32_t *cp[2][4][4]; /* [cycle][channel r g b a][operand a b c d] */
    uint8_t ccm[2][2];          /* [cycle][rgb, alpha]: CM_* */
    int32_t ccconst[2][4];
    const int32_t *blp[2], *blm[2], *bla[2];
    uint8_t blb[2];
    int32_t bcomb[4], bc0[4]; /* blender inputs: combiner output, cycle-0 blender output */
    int32_t bpa;              /* blender pixel alpha */
    int32_t memc[4];          /* memory colour */
    int32_t memcvg;
    int use_tex, use_tex1, use_shade, use_noise, use_lod, use_z, blend_pass;
    span_fn span;
    uint32_t prim_count; /* noise seed: primitives drawn so far */
};

#define BATCH_MAX (1u << 16)

struct n64_rdp {
    n64_rcp_bus bus;
    const uint8_t *dmem;
    uint32_t start, end, current, status;
    uint32_t clock, bufbusy, pipebusy, tmemc;
    int start_pending;
    uint64_t pend[24];
    int npend;
    uint8_t *hidden;
    rdp_render *rd[N64_RDP_MAX_THREADS];
    int nthreads;

    /* Front-end view of the state, for the costs and the batch hazards. */
    int fe_cycle, fe_ci_size, fe_ci_width, fe_zon;
    uint32_t fe_ci_addr, fe_zi_addr, fe_ti_addr;
    int fe_ti_size, fe_ti_width, fe_sc_y1;

    /* Batch (threads > 1). */
    uint64_t *batch;
    uint32_t nbatch;
    uint32_t haz_lo, haz_hi;
#ifdef RDP_THREADS
    pthread_t th[N64_RDP_MAX_THREADS];
    pthread_mutex_t mu;
    pthread_cond_t cv_go, cv_done;
    unsigned gen;
    int running, quit, threads_up;
#endif
};

/* ---- Small helpers ------------------------------------------------------------------------------------------------- */
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }
static inline int32_t clamp32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}
/* The combiner / shade clamp on 9 bits: 0x100..0x17F saturate to 255, 0x180..0x1FF (negative) to 0. */
static inline int clamp9(int v)
{
    v &= 0x1FF;
    return (v & 0x100) ? ((v & 0x80) ? 0 : 255) : v;
}
static inline int sext9(int v) { return ((v & 0x1FF) ^ 0x100) - 0x100; }
static inline int expand5(int v) { return (v << 3) | (v >> 2); }

static inline uint8_t ram8(const rdp_render *r, uint32_t a)
{
    a &= 0xFFFFFF;
    return a < r->ram_size ? r->ram[a] : 0;
}

static uint32_t hash3(uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ z * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

/* Z: 18-bit depth <-> 14-bit stored (3-bit exponent = leading ones, 11-bit mantissa). */
static const uint8_t z_shift[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
static const uint32_t z_base[8] = { 0x00000, 0x20000, 0x30000, 0x38000, 0x3C000, 0x3E000, 0x3F000, 0x3F800 };

static inline uint32_t z_decompress(uint32_t z14)
{
    uint32_t e = (z14 >> 11) & 7;
    return z_base[e] + ((z14 & 0x7FF) << z_shift[e]);
}
static inline uint32_t z_compress(uint32_t z18)
{
    uint32_t e = 0;
    while (e < 7 && ((z18 >> (17 - e)) & 1))
        e++;
    return e << 11 | (((z18 - z_base[e]) >> z_shift[e]) & 0x7FF);
}
static inline int dz_compress(uint32_t dz)
{
    int c = 0;
    while (c < 15 && (dz >> (c + 1)))
        c++;
    return c;
}

/* Dither matrices of the hardware, indexed [(y & 3) * 4 + (x & 3)]. */
static const uint8_t magic_matrix[16] = { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 };
static const uint8_t bayer_matrix[16] = { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 };

static inline int row_mine(const rdp_render *r, int y) { return r->nbands == 1 || ((y >> 2) % r->nbands) == r->band; }

/* ---- Textures ------------------------------------------------------------------------------------------------------ */
static inline void tlut_entry(const rdp_render *r, int idx, int32_t *o)
{
    const uint8_t *e = r->s.tmem + 0x800 + ((idx << 3) & 0x7FF);
    uint16_t v = (uint16_t)(e[0] << 8 | e[1]);
    if (r->s.om.tlut_ia) {
        o[0] = o[1] = o[2] = v >> 8;
        o[3] = v & 0xFF;
    } else {
        o[0] = expand5(v >> 11);
        o[1] = expand5((v >> 6) & 31);
        o[2] = expand5((v >> 1) & 31);
        o[3] = (v & 1) ? 255 : 0;
    }
}

/* One texel of tile t at integer coordinates (already wrapped) into o[4] (8-bit channels). YUV texels come out as
 * (U, V, Y, Y) for the convert step. */
ALWAYS_INLINE void fetch_texel(const rdp_render *r, const rdp_tile *t, int s, int tt, int32_t *o)
{
    const uint8_t *tm = r->s.tmem;
    const uint32_t base = (uint32_t)(t->tmem + tt * t->line);
    const uint32_t x = (tt & 1) ? 4 : 0;
    const int tlut = r->s.om.tlut;
    const uint32_t am = tlut ? 0x7FF : 0xFFF;
    switch (t->size) {
    case 0: {
        int n = (tm[((base + (uint32_t)(s >> 1)) ^ x) & am] >> ((s & 1) ? 0 : 4)) & 15;
        if (tlut) {
            tlut_entry(r, t->pal << 4 | n, o);
            return;
        }
        if (t->fmt == FMT_IA) {
            int i = n >> 1;
            o[0] = o[1] = o[2] = i << 5 | i << 2 | i >> 1;
            o[3] = (n & 1) ? 255 : 0;
        } else if (t->fmt == FMT_CI) {
            o[0] = o[1] = o[2] = o[3] = t->pal << 4 | n;
        } else {
            o[0] = o[1] = o[2] = o[3] = n * 17;
        }
        return;
    }
    case 1: {
        int n = tm[((base + (uint32_t)s) ^ x) & am];
        if (tlut) {
            tlut_entry(r, n, o);
            return;
        }
        if (t->fmt == FMT_IA) {
            o[0] = o[1] = o[2] = (n >> 4) * 17;
            o[3] = (n & 15) * 17;
        } else {
            o[0] = o[1] = o[2] = o[3] = n;
        }
        return;
    }
    case 2: {
        if (t->fmt == FMT_YUV) {
            uint32_t ay = ((base + (uint32_t)s) ^ x) & 0x7FF, auv = ((base + (uint32_t)(s & ~1)) ^ x) & 0x7FF;
            o[0] = tm[auv];
            o[1] = tm[(auv + 1) & 0x7FF];
            o[2] = o[3] = tm[0x800 + ay];
            return;
        }
        uint32_t a = ((base + (uint32_t)s * 2) ^ x) & am & ~1u;
        uint16_t v = (uint16_t)(tm[a] << 8 | tm[a + 1]);
        if (tlut) {
            tlut_entry(r, v >> 8, o);
            return;
        }
        if (t->fmt == FMT_IA) {
            o[0] = o[1] = o[2] = v >> 8;
            o[3] = v & 0xFF;
        } else if (t->fmt == FMT_I || t->fmt == FMT_CI) {
            o[0] = o[1] = o[2] = o[3] = v >> 8;
        } else {
            o[0] = expand5(v >> 11);
            o[1] = expand5((v >> 6) & 31);
            o[2] = expand5((v >> 1) & 31);
            o[3] = (v & 1) ? 255 : 0;
        }
        return;
    }
    default: { /* 32-bit: R G in the low half, B A in the high half */
        uint32_t a = ((base + (uint32_t)s * 2) ^ x) & 0x7FE;
        if (tlut) {
            tlut_entry(r, tm[a], o);
            return;
        }
        o[0] = tm[a];
        o[1] = tm[a + 1];
        o[2] = tm[a + 0x800];
        o[3] = tm[a + 0x801];
        return;
    }
    }
}

/* Raw texel for copy mode, as a 16-bit pixel (and the 32-bit / 8-bit forms for those colour images). */
static uint32_t copy_texel(const rdp_render *r, const rdp_tile *t, int s, int tt, int out_size)
{
    const uint8_t *tm = r->s.tmem;
    const uint32_t base = (uint32_t)(t->tmem + tt * t->line);
    const uint32_t x = (tt & 1) ? 4 : 0;
    const int tlut = r->s.om.tlut;
    const uint32_t am = tlut ? 0x7FF : 0xFFF;
    uint32_t idx;
    switch (t->size) {
    case 0:
        idx = (tm[((base + (uint32_t)(s >> 1)) ^ x) & am] >> ((s & 1) ? 0 : 4)) & 15;
        if (!tlut || out_size == 1)
            return out_size == 1 ? idx : (idx * 17) << 8 | idx * 17;
        idx |= (uint32_t)t->pal << 4;
        break;
    case 1:
        idx = tm[((base + (uint32_t)s) ^ x) & am];
        if (!tlut || out_size == 1)
            return out_size == 1 ? idx : idx << 8 | idx;
        break;
    case 2: {
        uint32_t a = ((base + (uint32_t)s * 2) ^ x) & am & ~1u;
        uint32_t v = (uint32_t)(tm[a] << 8 | tm[a + 1]);
        if (out_size == 1)
            return v >> 8;
        if (tlut) {
            idx = v >> 8;
            break;
        }
        if (out_size == 3)
            return (uint32_t)expand5((int)(v >> 11)) << 24 | (uint32_t)expand5((int)(v >> 6) & 31) << 16 |
                   (uint32_t)expand5((int)(v >> 1) & 31) << 8 | ((v & 1) ? 0xFF : 0);
        return v;
    }
    default: {
        uint32_t a = ((base + (uint32_t)s * 2) ^ x) & 0x7FE;
        uint32_t v = (uint32_t)tm[a] << 24 | (uint32_t)tm[a + 1] << 16 | (uint32_t)tm[a + 0x800] << 8 | tm[a + 0x801];
        if (out_size == 3)
            return v;
        if (out_size == 1)
            return v >> 24;
        return (v >> 16 & 0xF800) | (v >> 13 & 0x7C0) | (v >> 10 & 0x3E) | ((v & 0xFF) ? 1 : 0);
    }
    }
    {
        const uint8_t *e = tm + 0x800 + ((idx << 3) & 0x7FF);
        uint32_t v = (uint32_t)(e[0] << 8 | e[1]);
        if (out_size == 3)
            return (uint32_t)expand5((int)(v >> 11)) << 24 | (uint32_t)expand5((int)(v >> 6) & 31) << 16 |
                   (uint32_t)expand5((int)(v >> 1) & 31) << 8 | ((v & 1) ? 0xFF : 0);
        return v;
    }
}

/* One texture coordinate (s10.5) of tile t through shift, offset, clamp, mirror and mask: integer coordinates of the
 * texel and its +1 neighbour, and the 5-bit fraction. */
ALWAYS_INLINE void tc_axis(int32_t c, int shift, int lo, int hi, int clamp_en, int mask, int mirror, int *i0, int *i1,
                           int *frac)
{
    if (shift)
        c = shift < 11 ? c >> shift : (int32_t)((uint32_t)c << (16 - shift));
    c -= lo << 3;
    int i = c >> 5, f = c & 31, j = i + 1;
    if (clamp_en || !mask) {
        int mx = imax((hi >> 2) - (lo >> 2), 0);
        if (c < 0) {
            i = j = 0;
            f = 0;
        } else if (i >= mx) {
            i = j = mx;
            f = 0;
        } else if (j > mx) {
            j = mx;
        }
    }
    if (mask) {
        int m = (1 << mask) - 1;
        if (mirror) {
            if ((i >> mask) & 1)
                i = ~i;
            if ((j >> mask) & 1)
                j = ~j;
        }
        i &= m;
        j &= m;
    }
    *i0 = i;
    *i1 = j;
    *frac = f;
}

static inline void tile_coords(const rdp_tile *t, int32_t s, int32_t tt, int *s0, int *s1, int *fs, int *t0, int *t1,
                               int *ft)
{
    tc_axis(s, t->shift_s, t->sl, t->sh, t->cs, imin(t->mask_s, 10), t->ms, s0, s1, fs);
    tc_axis(tt, t->shift_t, t->tl, t->th, t->ct, imin(t->mask_t, 10), t->mt, t0, t1, ft);
}

/* YUV -> RGB with K0..K3 on (U, V, Y) = channels (0, 1, 2); the K4 / K5 part is the combiner's (TEX - K4) * K5 + TEX.
 * Approximation: U and V centred on 128, products rounded, 9-bit results. */
static void yuv_convert(const rdp_render *r, const int32_t *in, int32_t *o)
{
    const int32_t *k = r->s.k;
    int u = in[0] - 128, v = in[1] - 128, y = in[2];
    o[0] = sext9(y + ((k[0] * v + 0x80) >> 8));
    o[1] = sext9(y + ((k[1] * u + k[2] * v + 0x80) >> 8));
    o[2] = sext9(y + ((k[3] * u + 0x80) >> 8));
    o[3] = in[3];
}

/* Sample tile tn at (s, t) (s10.5) with the filter of cycle cyc. */
static void sample_tile(const rdp_render *r, int tn, int32_t s, int32_t t, int cyc, int32_t *o)
{
    const rdp_tile *tl = &r->s.tile[tn & 7];
    const rdp_modes *om = &r->s.om;
    int s0, s1, fs, t0, t1, ft;
    tile_coords(tl, s, t, &s0, &s1, &fs, &t0, &t1, &ft);
    if (!om->bilerp[cyc]) { /* the filter converts instead of interpolating */
        int32_t raw[4];
        fetch_texel(r, tl, s0, t0, raw);
        yuv_convert(r, raw, o);
        return;
    }
    if (!om->bilinear) {
        fetch_texel(r, tl, s0, t0, o);
        return;
    }
    int32_t a[4], b[4], c[4];
    if (om->mid_texel && fs == 16 && ft == 16) {
        int32_t d[4];
        fetch_texel(r, tl, s0, t0, a);
        fetch_texel(r, tl, s1, t0, b);
        fetch_texel(r, tl, s0, t1, c);
        fetch_texel(r, tl, s1, t1, d);
        for (int i = 0; i < 4; i++)
            o[i] = (a[i] + b[i] + c[i] + d[i] + 2) >> 2;
        return;
    }
    fetch_texel(r, tl, s1, t0, b);
    fetch_texel(r, tl, s0, t1, c);
    if (fs + ft < 32) { /* upper-left triangle of the texel square */
        fetch_texel(r, tl, s0, t0, a);
        for (int i = 0; i < 4; i++)
            o[i] = a[i] + ((fs * (b[i] - a[i]) + ft * (c[i] - a[i]) + 16) >> 5);
    } else {
        fetch_texel(r, tl, s1, t1, a);
        for (int i = 0; i < 4; i++)
            o[i] = a[i] + (((32 - fs) * (c[i] - a[i]) + (32 - ft) * (b[i] - a[i]) + 16) >> 5);
    }
}

static inline int32_t sat16(int64_t v) { return v > 0x7FFF ? 0x7FFF : v < -0x8000 ? -0x8000 : (int32_t)v; }

/* Texture coordinates (s10.5) from the S / T / W attributes. */
ALWAYS_INLINE void tex_st(int persp, int32_t as, int32_t at, int32_t aw, int32_t *s, int32_t *t)
{
    if (persp) {
        if (aw <= 0) { /* behind the eye: the divider saturates */
            *s = as < 0 ? -0x8000 : 0x7FFF;
            *t = at < 0 ? -0x8000 : 0x7FFF;
            return;
        }
        *s = sat16(((int64_t)as << 15) / aw);
        *t = sat16(((int64_t)at << 15) / aw);
    } else {
        *s = sat16(as >> 16);
        *t = sat16(at >> 16);
    }
}

/* Texture stage of one pixel: TEX0, TEX1 and the LOD fraction into r->in. */
static void tex_stage(rdp_render *r, const rdp_prim *p, const int32_t *a)
{
    const rdp_modes *om = &r->s.om;
    const int persp = om->persp && !p->rect;
    int32_t s, t;
    tex_st(persp, a[A_S], a[A_T], a[A_W], &s, &t);
    int t0 = p->tile, t1 = p->tile + 1;
    if (r->use_lod) {
        int32_t s1, tt1, s2, tt2;
        tex_st(persp, a[A_S] + p->dadx[A_S], a[A_T] + p->dadx[A_T], a[A_W] + p->dadx[A_W], &s1, &tt1);
        tex_st(persp, a[A_S] + p->dady[A_S], a[A_T] + p->dady[A_T], a[A_W] + p->dady[A_W], &s2, &tt2);
        int lod = imax(imax(abs(s1 - s), abs(tt1 - t)), imax(abs(s2 - s), abs(tt2 - t)));
        if (lod > 0x7FFF)
            lod = 0x7FFF;
        if (om->detail || om->sharpen)
            lod = imax(lod, r->s.min_level); /* min_level is 0.5 fixed point, as is lod / 32 at 5 bits */
        int lt = 0, frac;
        int magnify = lod < 32;
        if (magnify) {
            frac = (om->detail || om->sharpen) ? (lod << 3) & 0xFF : 0;
            if (om->sharpen)
                frac -= 0x100; /* negative: extrapolates away from TEX1 */
        } else {
            int texels = lod >> 5;
            while (texels >> (lt + 1))
                lt++;
            frac = ((lod << 3) >> lt) & 0xFF;
            if (lt >= p->level) {
                lt = p->level;
                frac = 0xFF;
            }
        }
        if (om->lod_en) {
            if (om->detail) {
                t0 = magnify ? p->tile : p->tile + lt + 1;
                t1 = t0 + 1;
            } else {
                t0 = p->tile + lt;
                t1 = lt < p->level ? t0 + 1 : t0;
            }
        }
        r->in.lodfrac = frac;
    }
    sample_tile(r, t0, s, t, 0, r->in.tex0);
    if (om->cycle == CYC_2) {
        if (!r->use_tex1)
            ; /* TEX1 is not read by the combiner */
        else if (om->convert_one)
            yuv_convert(r, r->in.tex0, r->in.tex1);
        else
            sample_tile(r, t1, s, t, 1, r->in.tex1);
    } else {
        memcpy(r->in.tex1, r->in.tex0, sizeof r->in.tex1); /* approximation: TEX1 = TEX0 in 1-cycle mode */
    }
}

/* ---- Combiner and blender resolution ------------------------------------------------------------------------------- */
static int cc_dynamic(const rdp_render *r, const int32_t *p)
{
    const rdp_cin *in = &r->in;
    return (p >= in->comb && p < in->comb + 4) || (p >= in->tex0 && p < in->tex0 + 4) ||
           (p >= in->tex1 && p < in->tex1 + 4) || (p >= in->shade && p < in->shade + 4) || p == &in->noise ||
           p == &in->lodfrac;
}

static const int32_t *cc_rgb_src(rdp_cin *in, int op, int sel, int ch)
{
    switch (op) {
    case 0: /* sub A */
        switch (sel) {
        case 0: return &in->comb[ch];
        case 1: return &in->tex0[ch];
        case 2: return &in->tex1[ch];
        case 3: return &in->prim[ch];
        case 4: return &in->shade[ch];
        case 5: return &in->env[ch];
        case 6: return &in->one;
        case 7: return &in->noise;
        default: return &in->zero;
        }
    case 1: /* sub B */
        switch (sel) {
        case 0: return &in->comb[ch];
        case 1: return &in->tex0[ch];
        case 2: return &in->tex1[ch];
        case 3: return &in->prim[ch];
        case 4: return &in->shade[ch];
        case 5: return &in->env[ch];
        case 6: return &in->keyc[ch];
        case 7: return &in->k4;
        default: return &in->zero;
        }
    case 2: /* mul */
        switch (sel) {
        case 0: return &in->comb[ch];
        case 1: return &in->tex0[ch];
        case 2: return &in->tex1[ch];
        case 3: return &in->prim[ch];
        case 4: return &in->shade[ch];
        case 5: return &in->env[ch];
        case 6: return &in->keys[ch];
        case 7: return &in->comb[3];
        case 8: return &in->tex0[3];
        case 9: return &in->tex1[3];
        case 10: return &in->prim[3];
        case 11: return &in->shade[3];
        case 12: return &in->env[3];
        case 13: return &in->lodfrac;
        case 14: return &in->primlod;
        case 15: return &in->k5;
        default: return &in->zero;
        }
    default: /* add */
        switch (sel) {
        case 0: return &in->comb[ch];
        case 1: return &in->tex0[ch];
        case 2: return &in->tex1[ch];
        case 3: return &in->prim[ch];
        case 4: return &in->shade[ch];
        case 5: return &in->env[ch];
        case 6: return &in->one;
        default: return &in->zero;
        }
    }
}

static const int32_t *cc_alpha_src(rdp_cin *in, int op, int sel)
{
    if (op == 2) {
        switch (sel) {
        case 0: return &in->lodfrac;
        case 1: return &in->tex0[3];
        case 2: return &in->tex1[3];
        case 3: return &in->prim[3];
        case 4: return &in->shade[3];
        case 5: return &in->env[3];
        case 6: return &in->primlod;
        default: return &in->zero;
        }
    }
    switch (sel) {
    case 0: return &in->comb[3];
    case 1: return &in->tex0[3];
    case 2: return &in->tex1[3];
    case 3: return &in->prim[3];
    case 4: return &in->shade[3];
    case 5: return &in->env[3];
    case 6: return &in->one;
    default: return &in->zero;
    }
}

ALWAYS_INLINE int cc_eval(const int32_t *const *op)
{
    return ((*op[0] - *op[1]) * *op[2] + *op[3] * 256 + 0x80) >> 8;
}

static void span_select(rdp_render *r);

/* Re-derives the combiner pointers, fast paths and flags after a state change. */
static void prepare(rdp_render *r)
{
    rdp_state *s = &r->s;
    rdp_modes *om = &s->om;
    rdp_cin *in = &r->in;
    in->one = 256;
    in->zero = 0;
    in->primlod = s->prim_lod_frac;
    in->k4 = s->k[4];
    in->k5 = s->k[5];
    for (int c = 0; c < 4; c++) {
        in->prim[c] = s->prim[c];
        in->env[c] = s->env[c];
    }
    for (int c = 0; c < 3; c++) {
        in->keyc[c] = s->key_center[c];
        in->keys[c] = s->key_scale[c];
    }
    in->keyc[3] = in->keys[3] = 0;

    r->use_tex = r->use_tex1 = r->use_shade = r->use_noise = 0;
    int lodfrac_used = 0;
    const int first = om->cycle == CYC_2 ? 0 : 1;
    for (int cyc = 0; cyc < 2; cyc++) {
        const uint8_t *cc = s->cc[cyc];
        for (int ch = 0; ch < 4; ch++)
            for (int op = 0; op < 4; op++)
                r->cp[cyc][ch][op] = ch < 3 ? cc_rgb_src(in, op, cc[op], ch) : cc_alpha_src(in, op, cc[4 + op]);
        for (int g = 0; g < 2; g++) {
            const int32_t *const *o = r->cp[cyc][g ? 3 : 0];
            int dyn = 0;
            for (int ch = g ? 3 : 0; ch < (g ? 4 : 3); ch++)
                for (int op = 0; op < 4; op++) {
                    const int32_t *p = r->cp[cyc][ch][op];
                    if (cc_dynamic(r, p))
                        dyn = 1;
                    if (cyc >= first) {
                        if ((p >= in->tex0 && p < in->tex0 + 4) || (p >= in->tex1 && p < in->tex1 + 4))
                            r->use_tex = 1;
                        if (p >= in->tex1 && p < in->tex1 + 4)
                            r->use_tex1 = 1;
                        if (p >= in->shade && p < in->shade + 4)
                            r->use_shade = 1;
                        if (p == &in->noise)
                            r->use_noise = 1;
                        if (p == &in->lodfrac)
                            lodfrac_used = 1;
                    }
                }
            if (!dyn) {
                r->ccm[cyc][g] = CM_CONST;
                for (int ch = g ? 3 : 0; ch < (g ? 4 : 3); ch++)
                    r->ccconst[cyc][ch] = cc_eval(r->cp[cyc][ch]);
            } else if (o[2] == &in->zero || o[0] == o[1]) {
                r->ccm[cyc][g] = CM_PASS;
            } else if (o[1] == &in->zero && o[3] == &in->zero) {
                r->ccm[cyc][g] = CM_MUL;
            } else {
                r->ccm[cyc][g] = CM_GENERIC;
            }
            /* CM_PASS / CM_MUL must hold for every channel of the group (the RGB selectors are shared). */
        }
    }

    /* Blender: in 1-cycle mode the cycle-0 settings blend; in 2-cycle mode cycle 1's pixel input is cycle 0's output. */
    for (int cyc = 0; cyc < 2; cyc++) {
        const int32_t *pix = (om->cycle == CYC_2 && cyc == 1) ? r->bc0 : r->bcomb;
        const int32_t *srcs[4] = { pix, r->memc, s->blend, s->fog };
        r->blp[cyc] = srcs[om->m1a[cyc]];
        r->blm[cyc] = srcs[om->m2a[cyc]];
        const int32_t *as[4] = { &r->bpa, &s->fog[3], &in->shade[3], &in->zero };
        r->bla[cyc] = as[om->m1b[cyc]];
        r->blb[cyc] = om->m2b[cyc];
        if (om->m1b[cyc] == 2 && (om->cycle == CYC_2 || cyc == 0))
            r->use_shade = 1;
    }
    /* A blender that gives back its pixel input: P = M = pixel with B = 1 - A, or A = 1 / B = 0 on the pixel. */
    {
        int c = om->cycle == CYC_2 ? 1 : 0;
        int pass = om->m1a[c] == 0 && om->m2a[c] == 0 && om->m2b[c] == 0; /* P*a + P*(32-a), any A */
        if (om->cycle == CYC_2)
            pass = pass && om->m1a[0] == 0 && om->m2a[0] == 0 && om->m2b[0] == 0;
        r->blend_pass = pass;
    }
    r->use_z = om->z_compare || om->z_update;
    r->use_lod = om->lod_en || lodfrac_used;
    if (om->alpha_compare && om->dither_alpha)
        r->use_noise = 1;
    if (om->rgb_dither == 2 || om->alpha_dither == 2)
        r->use_noise = 1;
    if (r->use_lod && r->use_tex == 0 && om->lod_en)
        r->use_tex = 1;
    if (lodfrac_used)
        r->use_tex = 1; /* the LOD fraction comes from the texture coordinates */
    span_select(r);
    r->dirty = 0;
}

/* ---- Pixel pipeline ------------------------------------------------------------------------------------------------ */
ALWAYS_INLINE void combine_cycle(const rdp_render *r, int cyc, int32_t *out)
{
    switch (r->ccm[cyc][0]) {
    case CM_CONST:
        out[0] = r->ccconst[cyc][0];
        out[1] = r->ccconst[cyc][1];
        out[2] = r->ccconst[cyc][2];
        break;
    case CM_PASS:
        out[0] = *r->cp[cyc][0][3];
        out[1] = *r->cp[cyc][1][3];
        out[2] = *r->cp[cyc][2][3];
        break;
    case CM_MUL:
        for (int c = 0; c < 3; c++)
            out[c] = (*r->cp[cyc][c][0] * *r->cp[cyc][c][2] + 0x80) >> 8;
        break;
    default:
        for (int c = 0; c < 3; c++)
            out[c] = cc_eval(r->cp[cyc][c]);
    }
    switch (r->ccm[cyc][1]) {
    case CM_CONST: out[3] = r->ccconst[cyc][3]; break;
    case CM_PASS: out[3] = *r->cp[cyc][3][3]; break;
    case CM_MUL: out[3] = (*r->cp[cyc][3][0] * *r->cp[cyc][3][2] + 0x80) >> 8; break;
    default: out[3] = cc_eval(r->cp[cyc][3]);
    }
}

/* Chroma key (approximation): a channel matches when |colour - centre| * scale / 256 <= width (4.8 fixed point, so
 * width / 16 in 8-bit units); a pixel whose three channels match gets alpha 0, others keep 255. */
static int key_alpha(const rdp_render *r, const int32_t *c)
{
    for (int i = 0; i < 3; i++)
        if (((abs(c[i] - r->s.key_center[i]) * r->s.key_scale[i]) >> 8) > (r->s.key_width[i] >> 4))
            return 255;
    return 0;
}

ALWAYS_INLINE void blend_cycle(const rdp_render *r, int cyc, int divide, int32_t *out)
{
    const int32_t *P = r->blp[cyc], *M = r->blm[cyc];
    int a = *r->bla[cyc] >> 3, b;
    switch (r->blb[cyc]) {
    case 0: b = ~a & 31; break;
    case 1: b = r->memcvg << 2; break;
    case 2: b = 31; break;
    default: b = 0; break;
    }
    if (divide) {
        int den = (a & ~3) + (b & ~3) + 4;
        for (int c = 0; c < 3; c++)
            out[c] = imin((P[c] * a + M[c] * (b + 1)) / den, 255);
    } else {
        for (int c = 0; c < 3; c++)
            out[c] = imin((P[c] * a + M[c] * (b + 1)) >> 5, 255);
    }
}

static int z_test(int mode, uint32_t sz, uint32_t oz, int dzc_new, int dzc_mem, int overflow)
{
    int max_z = oz == 0x3FFFF;
    int32_t dzmax = (int32_t)(1u << (dzc_new > dzc_mem ? dzc_new : dzc_mem)) << 3;
    int infront = sz < oz;
    int nearer = (int32_t)sz - dzmax <= (int32_t)oz;
    int farther = (int32_t)sz + dzmax >= (int32_t)oz;
    switch (mode) {
    case ZM_OPAQUE:
    case ZM_INTER: /* approximation: interpenetrating tests like opaque (no blend of the intersection) */
        return max_z || (overflow ? infront : nearer);
    case ZM_TRANS: return infront || max_z;
    default: return farther && nearer && !max_z;
    }
}

#define F_TEX 1u
#define F_SHADE 2u
#define F_Z 4u
#define F_2C 8u
#define F_32 16u

static inline int row_cvg(const rdp_row *row, int x)
{
    int x8 = x * 8, c = 0;
    for (int j = 0; j < 4; j++) {
        if (!(row->valid >> j & 1))
            continue;
        int o = x8 + ((j & 1) ? 2 : 0);
        c += (o >= row->xl8[j] && o < row->xr8[j]) + (o + 4 >= row->xl8[j] && o + 4 < row->xr8[j]);
    }
    return c;
}

/* The span routine of 1- and 2-cycle modes; F is a constant in each instance. */
ALWAYS_INLINE void span_std(rdp_render *r, const rdp_prim *p, const rdp_row *row, const unsigned F)
{
    const rdp_state *s = &r->s;
    const rdp_modes M = s->om; /* locals: the RDRAM stores below cannot alias them */
    rdp_cin *in = &r->in;
    uint8_t *restrict ram = r->ram;
    uint8_t *restrict hid = r->hidden;
    const uint32_t size = r->ram_size, ci_addr = s->ci_addr, zi_addr = s->zi_addr;
    const int use_noise = r->use_noise, blend_pass = r->blend_pass, thr_blend = s->blend[3];
    const uint32_t seed = r->prim_count;
    const int dzc = p->dzc;
    const uint32_t primz18 = p->primz18;
    int32_t dadx[A_N];
    memcpy(dadx, p->dadx, sizeof dadx);
    /* A combiner without per-pixel inputs in 1-cycle mode: its output once for the span. */
    const int cc_const = !(F & F_2C) && r->ccm[1][0] == CM_CONST && r->ccm[1][1] == CM_CONST;
    int32_t ccol[4], ccomb[4];
    for (int c = 0; c < 4; c++) {
        ccol[c] = clamp9(r->ccconst[1][c]);
        ccomb[c] = sext9(r->ccconst[1][c]);
    }
    const int y = row->y;
    const int ci8 = !(F & F_32) && s->ci_size == 1;
    const uint8_t *dith = M.rgb_dither == 0 ? magic_matrix : M.rgb_dither == 1 ? bayer_matrix : NULL;
    const int dither_on = !(F & F_32) && M.rgb_dither != 3;
    const uint32_t row_off = (uint32_t)y * (uint32_t)s->ci_width;
    int32_t a[A_N];
    memcpy(a, row->a, sizeof a);
    memset(in->comb, 0, sizeof in->comb); /* COMBINED before the first pixel: 0, so rows do not depend on each other */
    if (!(F & F_SHADE))
        memset(in->shade, 0, sizeof in->shade);
    for (int x = row->x0; x <= row->x1; x++) {
        int cvg = (x >= row->fx0 && x < row->fx1) ? 8 : row_cvg(row, x);
        if (!cvg)
            goto next;
        uint32_t paddr = (ci_addr + ((row_off + (uint32_t)x) << ((F & F_32) ? 2 : ci8 ? 0 : 1))) & 0xFFFFFF;
        if (paddr + ((F & F_32) ? 3 : 1) >= size)
            goto next;
        uint32_t h = 0;
        if (use_noise) {
            h = hash3((uint32_t)x, (uint32_t)y, seed);
            in->noise = (int32_t)((h & 7) << 6 | 0x20);
        }
        /* Memory colour and coverage. */
        int memcvg = 7;
        if (M.image_read) {
            if (F & F_32) {
                r->memc[0] = ram[paddr];
                r->memc[1] = ram[paddr + 1];
                r->memc[2] = ram[paddr + 2];
                memcvg = ram[paddr + 3] >> 5;
            } else if (ci8) {
                r->memc[0] = r->memc[1] = r->memc[2] = ram[paddr];
            } else {
                uint16_t v = n64_be16(ram + paddr);
                r->memc[0] = expand5(v >> 11);
                r->memc[1] = expand5((v >> 6) & 31);
                r->memc[2] = expand5((v >> 1) & 31);
                memcvg = (v & 1) << 2 | hid[paddr >> 1];
            }
        }
        /* Z. */
        uint32_t z18 = 0, zaddr = 0;
        int zok = 0;
        if (F & F_Z) {
            z18 = M.z_prim ? primz18 : a[A_Z] < 0 ? 0 : (uint32_t)a[A_Z] >> 13;
            zaddr = (zi_addr + ((row_off + (uint32_t)x) << 1)) & 0xFFFFFF;
            zok = zaddr + 1 < size;
            if (M.z_compare && zok) {
                uint16_t zm = n64_be16(ram + zaddr);
                int dzm = (zm & 3) << 2 | hid[zaddr >> 1];
                if (!z_test(M.z_mode, z18, z_decompress(zm >> 2), dzc, dzm, cvg + memcvg >= 8))
                    goto next;
            }
        }
        if (F & F_SHADE) {
            in->shade[0] = clamp9(a[A_R] >> 16);
            in->shade[1] = clamp9(a[A_G] >> 16);
            in->shade[2] = clamp9(a[A_B] >> 16);
            in->shade[3] = clamp9(a[A_A] >> 16);
        }
        if (F & F_TEX)
            tex_stage(r, p, a);
        /* Combiner. */
        int32_t col[4];
        if (F & F_2C) {
            int32_t c0[4];
            combine_cycle(r, 0, c0);
            for (int c = 0; c < 4; c++)
                in->comb[c] = sext9(c0[c]);
            combine_cycle(r, 1, col);
            for (int c = 0; c < 4; c++)
                col[c] = clamp9(col[c]);
        } else if (cc_const) {
            memcpy(col, ccol, sizeof col);
            memcpy(in->comb, ccomb, sizeof ccomb);
        } else {
            combine_cycle(r, 1, col);
            for (int c = 0; c < 4; c++) {
                in->comb[c] = sext9(col[c]);
                col[c] = clamp9(col[c]);
            }
        }
        if (M.key)
            col[3] = key_alpha(r, col);
        /* Coverage and alpha. */
        int pcvg = cvg;
        if (M.cvg_x_alpha)
            pcvg = (cvg * (col[3] + 1)) >> 8;
        int alpha = M.alpha_cvg_sel ? imin(pcvg << 5, 255) : col[3];
        if (M.alpha_compare) {
            int thr = M.dither_alpha ? (int)(h >> 8 & 0xFF) : thr_blend;
            if (alpha < thr)
                goto next;
        }
        int overflow = pcvg + memcvg >= 8;
        /* Blender. */
        const int32_t *out;
        int32_t bo[3];
        if (blend_pass) {
            out = col;
        } else {
            r->bcomb[0] = col[0];
            r->bcomb[1] = col[1];
            r->bcomb[2] = col[2];
            r->bpa = alpha;
            r->memcvg = memcvg;
            int fc = 0;
            if (F & F_2C) {
                blend_cycle(r, 0, 0, r->bc0);
                fc = 1;
            }
            if (M.force_blend || overflow) {
                blend_cycle(r, fc, !M.force_blend, bo);
                out = bo;
            } else {
                out = r->blp[fc];
            }
        }
        /* Coverage to store. */
        int ncvg;
        switch (M.cvg_dest) {
        case 0: ncvg = overflow ? 7 : pcvg + memcvg; break;
        case 1: ncvg = (pcvg + memcvg) & 7; break;
        case 2: ncvg = 7; break;
        default: ncvg = memcvg; break;
        }
        if (M.color_on_cvg && !overflow) { /* coverage only */
            if (F & F_32) {
                ram[paddr + 3] = (uint8_t)((ram[paddr + 3] & 0x1F) | ncvg << 5);
            } else if (!ci8) {
                ram[paddr + 1] = (uint8_t)((ram[paddr + 1] & 0xFE) | ncvg >> 2);
                hid[paddr >> 1] = (uint8_t)(ncvg & 3);
            }
        } else if (F & F_32) {
            ram[paddr] = (uint8_t)out[0];
            ram[paddr + 1] = (uint8_t)out[1];
            ram[paddr + 2] = (uint8_t)out[2];
            ram[paddr + 3] = (uint8_t)(ncvg << 5);
        } else if (ci8) {
            ram[paddr] = (uint8_t)out[0];
        } else {
            int cr = out[0], cg = out[1], cb = out[2];
            if (dither_on) {
                int dv = dith ? dith[(y & 3) * 4 + (x & 3)] : (int)(h >> 3 & 7);
                if ((cr & 7) > dv)
                    cr = imin(cr + 8, 255);
                if ((cg & 7) > dv)
                    cg = imin(cg + 8, 255);
                if ((cb & 7) > dv)
                    cb = imin(cb + 8, 255);
            }
            n64_wbe16(ram + paddr, (uint16_t)((cr >> 3) << 11 | (cg >> 3) << 6 | (cb >> 3) << 1 | ncvg >> 2));
            hid[paddr >> 1] = (uint8_t)(ncvg & 3);
        }
        if ((F & F_Z) && M.z_update && zok) {
            n64_wbe16(ram + zaddr, (uint16_t)(z_compress(z18) << 2 | (uint32_t)(dzc >> 2)));
            hid[zaddr >> 1] = (uint8_t)(dzc & 3);
        }
    next:
        if (F & F_SHADE) {
            a[A_R] += dadx[A_R];
            a[A_G] += dadx[A_G];
            a[A_B] += dadx[A_B];
            a[A_A] += dadx[A_A];
        }
        if (F & F_TEX) {
            a[A_S] += dadx[A_S];
            a[A_T] += dadx[A_T];
            a[A_W] += dadx[A_W];
        }
        if (F & F_Z)
            a[A_Z] += dadx[A_Z];
    }
}

#define SPAN_INST(f)                                                                                                     \
    static void span_##f(rdp_render *r, const rdp_prim *p, const rdp_row *row) { span_std(r, p, row, f); }
SPAN_INST(0) SPAN_INST(1) SPAN_INST(2) SPAN_INST(3) SPAN_INST(4) SPAN_INST(5) SPAN_INST(6) SPAN_INST(7)
SPAN_INST(8) SPAN_INST(9) SPAN_INST(10) SPAN_INST(11) SPAN_INST(12) SPAN_INST(13) SPAN_INST(14) SPAN_INST(15)
SPAN_INST(16) SPAN_INST(17) SPAN_INST(18) SPAN_INST(19) SPAN_INST(20) SPAN_INST(21) SPAN_INST(22) SPAN_INST(23)
SPAN_INST(24) SPAN_INST(25) SPAN_INST(26) SPAN_INST(27) SPAN_INST(28) SPAN_INST(29) SPAN_INST(30) SPAN_INST(31)
static const span_fn span_table[32] = {
    span_0,  span_1,  span_2,  span_3,  span_4,  span_5,  span_6,  span_7,  span_8,  span_9,  span_10,
    span_11, span_12, span_13, span_14, span_15, span_16, span_17, span_18, span_19, span_20, span_21,
    span_22, span_23, span_24, span_25, span_26, span_27, span_28, span_29, span_30, span_31,
};

/* Fill mode: the fill colour over the span (no coverage, no pipeline). */
static void fill_span(rdp_render *r, int y, int x0, int x1);
static void span_fill(rdp_render *r, const rdp_prim *p, const rdp_row *row)
{
    (void)p;
    fill_span(r, row->y, row->x0, row->x1);
}
static void span_none(rdp_render *r, const rdp_prim *p, const rdp_row *row)
{
    (void)r;
    (void)p;
    (void)row; /* copy-mode triangles: not drawn (approximation; the hardware draws garbage-ish texels) */
}

static void span_select(rdp_render *r)
{
    const rdp_modes *om = &r->s.om;
    if (om->cycle == CYC_FILL) {
        r->span = span_fill;
        return;
    }
    if (om->cycle == CYC_COPY) {
        r->span = span_none;
        return;
    }
    unsigned f = 0;
    if (r->use_tex)
        f |= F_TEX;
    if (r->use_shade)
        f |= F_SHADE;
    if (r->use_z)
        f |= F_Z;
    if (om->cycle == CYC_2)
        f |= F_2C;
    if (r->s.ci_size == 3)
        f |= F_32;
    r->span = span_table[f];
}

/* ---- Edge walker --------------------------------------------------------------------------------------------------- */
static inline int32_t xclamp(int64_t v) { return v > (1 << 30) ? (1 << 30) : v < -(1 << 30) ? -(1 << 30) : (int32_t)v; }

static void raster(rdp_render *r, const rdp_prim *p)
{
    const rdp_state *s = &r->s;
    const int ytop = imax(p->yh, s->sc_y0), ybot = imin(p->yl, s->sc_y1); /* sub-scanlines [ytop, ybot) */
    if (ytop >= ybot)
        return;
    if (r->dirty)
        prepare(r);
    r->prim_count++;
    const int k0 = p->yh & ~3, y0 = k0 >> 2;
    const int32_t ih = p->dxh >> 2, im = p->dxm >> 2, il = p->dxl >> 2;
    const int64_t clx0 = (int64_t)s->sc_x0 << 14, clx1 = (int64_t)s->sc_x1 << 14;
    for (int y = ytop >> 2; y <= (ybot - 1) >> 2; y++) {
        if (!row_mine(r, y) || (s->sc_field && (y & 1) != s->sc_odd))
            continue;
        rdp_row row;
        row.y = y;
        row.valid = 0;
        int minl = INT_MAX, maxr = INT_MIN, maxl = INT_MIN, minr = INT_MAX, nfull = 0;
        for (int j = 0; j < 4; j++) {
            int k = y * 4 + j;
            if (k < ytop || k >= ybot)
                continue;
            int64_t xmaj = (int64_t)p->xh + (int64_t)ih * (k - k0);
            int64_t xmin = k < p->ym ? (int64_t)p->xm + (int64_t)im * (k - k0) : (int64_t)p->xl + (int64_t)il * (k - p->ym);
            int64_t xa = p->lft ? xmaj : xmin, xb = p->lft ? xmin : xmaj;
            if (xa < clx0)
                xa = clx0;
            if (xb > clx1)
                xb = clx1;
            row.valid |= 1 << j;
            if (xa >= xb) {
                row.xl8[j] = row.xr8[j] = 0;
                continue;
            }
            int l8 = (int)(xclamp(xa) >> 13), r8 = (int)(xclamp(xb) >> 13);
            row.xl8[j] = l8;
            row.xr8[j] = r8;
            if (l8 >= r8)
                continue;
            minl = imin(minl, l8);
            maxr = imax(maxr, r8);
            maxl = imax(maxl, l8);
            minr = imin(minr, r8);
            nfull++;
        }
        if (minl >= maxr)
            continue;
        row.x0 = minl >> 3;
        row.x1 = (maxr - 1) >> 3;
        if (nfull == 4) {
            row.fx0 = (maxl + 7) >> 3;
            row.fx1 = minr >> 3;
        } else {
            row.fx0 = row.fx1 = 0;
        }
        const int64_t xm = (int64_t)p->xh + (int64_t)ih * (y * 4 - k0);
        const int64_t dx = ((int64_t)row.x0 << 16) - xm;
        for (int i = 0; i < A_N; i++)
            row.a[i] = (int32_t)((uint32_t)p->a0[i] + (uint32_t)p->dade[i] * (uint32_t)(y - y0) +
                                 (uint32_t)(((int64_t)p->dadx[i] * dx) >> 16));
        r->span(r, p, &row);
    }
}

/* ---- Fill and copy modes ------------------------------------------------------------------------------------------- */
static void fill_span(rdp_render *r, int y, int x0, int x1)
{
    const rdp_state *s = &r->s;
    const uint32_t fc = s->fill_color, size = r->ram_size;
    uint8_t *ram = r->ram;
    const uint32_t row = (uint32_t)y * (uint32_t)s->ci_width;
    switch (s->ci_size) {
    case 3:
        for (int x = x0; x <= x1; x++) {
            uint32_t a = (s->ci_addr + ((row + (uint32_t)x) << 2)) & 0xFFFFFF;
            if (a + 3 < size)
                n64_wbe32(ram + a, fc);
        }
        break;
    case 2: {
        uint32_t a = (s->ci_addr + ((row + (uint32_t)x0) << 1)) & 0xFFFFFF;
        uint32_t e = a + (uint32_t)(x1 - x0) * 2;
        if (e + 1 < size && e <= 0xFFFFFF && x1 >= x0) {
            for (; a <= e; a += 2) {
                uint16_t v = (uint16_t)((a & 2) ? fc : fc >> 16);
                ram[a] = (uint8_t)(v >> 8);
                ram[a + 1] = (uint8_t)v;
                r->hidden[a >> 1] = (v & 1) ? 3 : 0;
            }
        } else {
            for (int x = x0; x <= x1; x++) {
                a = (s->ci_addr + ((row + (uint32_t)x) << 1)) & 0xFFFFFF;
                if (a + 1 >= size)
                    continue;
                uint16_t v = (uint16_t)((a & 2) ? fc : fc >> 16);
                n64_wbe16(ram + a, v);
                r->hidden[a >> 1] = (v & 1) ? 3 : 0;
            }
        }
        break;
    }
    default:
        for (int x = x0; x <= x1; x++) {
            uint32_t a = (s->ci_addr + row + (uint32_t)x) & 0xFFFFFF;
            if (a < size)
                ram[a] = (uint8_t)(fc >> ((3 - (a & 3)) * 8));
        }
        break;
    }
}

/* Scissor bounds in whole pixels: x in [*x0, *x1), y in [*y0, *y1). */
static void scissor_px(const rdp_state *s, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = (s->sc_x0 + 3) >> 2;
    *y0 = (s->sc_y0 + 3) >> 2;
    *x1 = (s->sc_x1 + 3) >> 2;
    *y1 = (s->sc_y1 + 3) >> 2;
}

static void fill_rect(rdp_render *r, int xh, int yh, int xl, int yl)
{
    int sx0, sy0, sx1, sy1;
    scissor_px(&r->s, &sx0, &sy0, &sx1, &sy1);
    int x0 = imax(xh >> 2, sx0), x1 = imin(xl >> 2, sx1 - 1);
    int y0 = imax(yh >> 2, sy0), y1 = imin(yl >> 2, sy1 - 1);
    if (x0 > x1)
        return;
    for (int y = y0; y <= y1; y++) {
        if (!row_mine(r, y) || (r->s.sc_field && (y & 1) != r->s.sc_odd))
            continue;
        fill_span(r, y, x0, x1);
    }
}

static void copy_rect(rdp_render *r, int xh, int yh, int xl, int yl, int tn, int32_t S, int32_t T, int32_t dsdx,
                      int32_t dtdy, int flip)
{
    rdp_state *s = &r->s;
    const rdp_tile *t = &s->tile[tn];
    int sx0, sy0, sx1, sy1;
    scissor_px(s, &sx0, &sy0, &sx1, &sy1);
    const int bx = xh >> 2, by = yh >> 2;
    int x0 = imax(bx, sx0), x1 = imin(xl >> 2, sx1 - 1);
    int y0 = imax(by, sy0), y1 = imin(yl >> 2, sy1 - 1);
    if (x0 > x1)
        return;
    const int osz = s->ci_size;
    const int acmp = s->om.alpha_compare;
    uint8_t *ram = r->ram;
    for (int y = y0; y <= y1; y++) {
        if (!row_mine(r, y) || (s->sc_field && (y & 1) != s->sc_odd))
            continue;
        const uint32_t row = (uint32_t)y * (uint32_t)s->ci_width;
        /* s10.10 coordinates: copy mode steps dsdx / 4 per pixel (4.0 = one texel per pixel). */
        int32_t fy = (flip ? dsdx : dtdy) * (y - by);
        for (int x = x0; x <= x1; x++) {
            int32_t fx = (flip ? dtdy : dsdx) * (x - bx) / 4;
            int32_t ss = (S * 32 + (flip ? fy : fx)) >> 5, tt = (T * 32 + (flip ? fx : fy)) >> 5;
            int s0, s1, fs, t0, t1, ft;
            tile_coords(t, ss, tt, &s0, &s1, &fs, &t0, &t1, &ft);
            uint32_t v = copy_texel(r, t, s0, t0, osz);
            if (osz == 3) {
                uint32_t a = (s->ci_addr + ((row + (uint32_t)x) << 2)) & 0xFFFFFF;
                if (acmp && !(v & 0xFF))
                    continue;
                if (a + 3 < r->ram_size)
                    n64_wbe32(ram + a, v);
            } else if (osz == 2) {
                uint32_t a = (s->ci_addr + ((row + (uint32_t)x) << 1)) & 0xFFFFFF;
                if (acmp && !(v & 1))
                    continue;
                if (a + 1 < r->ram_size) {
                    n64_wbe16(ram + a, (uint16_t)v);
                    r->hidden[a >> 1] = (v & 1) ? 3 : 0;
                }
            } else {
                uint32_t a = (s->ci_addr + row + (uint32_t)x) & 0xFFFFFF;
                if (a < r->ram_size)
                    ram[a] = (uint8_t)v;
            }
        }
    }
}

/* ---- TMEM loads ---------------------------------------------------------------------------------------------------- */
static void set_tile_size(rdp_tile *t, uint64_t w)
{
    t->sl = (uint16_t)((w >> 44) & 0xFFF);
    t->tl = (uint16_t)((w >> 32) & 0xFFF);
    t->sh = (uint16_t)((w >> 12) & 0xFFF);
    t->th = (uint16_t)(w & 0xFFF);
}

static void load_tile(rdp_render *r, uint64_t w)
{
    rdp_state *s = &r->s;
    rdp_tile *t = &s->tile[(w >> 24) & 7];
    set_tile_size(t, w);
    int s0 = t->sl >> 2, t0 = t->tl >> 2, s1 = t->sh >> 2, t1 = t->th >> 2;
    if (s1 < s0 || t1 < t0)
        return;
    const int n = s1 - s0 + 1;
    uint8_t *tm = s->tmem;
    for (int i = 0; i <= t1 - t0; i++) {
        uint32_t src = s->ti_addr + ((((uint32_t)(t0 + i) * (uint32_t)s->ti_width + (uint32_t)s0) << s->ti_size) >> 1);
        uint32_t dst = t->tmem + (uint32_t)(i * t->line), x = (i & 1) ? 4 : 0;
        if (s->ti_size == 3) {
            for (int k = 0; k < n; k++) {
                uint32_t o = (dst + (uint32_t)k * 2) ^ x;
                tm[o & 0x7FF] = ram8(r, src + (uint32_t)k * 4);
                tm[(o + 1) & 0x7FF] = ram8(r, src + (uint32_t)k * 4 + 1);
                tm[0x800 | (o & 0x7FF)] = ram8(r, src + (uint32_t)k * 4 + 2);
                tm[0x800 | ((o + 1) & 0x7FF)] = ram8(r, src + (uint32_t)k * 4 + 3);
            }
        } else if (t->fmt == FMT_YUV && s->ti_size == 2) { /* U Y0 V Y1 -> U V low, Y0 Y1 high */
            for (int k = 0; k + 1 < n + 1; k += 2) {
                uint32_t o = (dst + (uint32_t)k) ^ x;
                tm[o & 0x7FF] = ram8(r, src + (uint32_t)k * 2);
                tm[0x800 | (o & 0x7FF)] = ram8(r, src + (uint32_t)k * 2 + 1);
                tm[(o + 1) & 0x7FF] = ram8(r, src + (uint32_t)k * 2 + 2);
                tm[0x800 | ((o + 1) & 0x7FF)] = ram8(r, src + (uint32_t)k * 2 + 3);
            }
        } else {
            uint32_t bytes = ((uint32_t)n << s->ti_size) >> 1;
            if (s->ti_size == 0)
                bytes = ((uint32_t)n + 1) >> 1;
            for (uint32_t b = 0; b < bytes; b++)
                tm[((dst + b) ^ x) & 0xFFF] = ram8(r, src + b);
        }
    }
}

static void load_block(rdp_render *r, uint64_t w)
{
    rdp_state *s = &r->s;
    rdp_tile *t = &s->tile[(w >> 24) & 7];
    const uint32_t sl = (w >> 44) & 0xFFF, tl = (w >> 32) & 0xFFF, sh = (w >> 12) & 0xFFF, dxt = w & 0xFFF;
    t->sl = (uint16_t)sl;
    t->tl = (uint16_t)tl;
    t->sh = (uint16_t)sh;
    t->th = (uint16_t)dxt;
    if (sh < sl)
        return;
    const uint32_t n = sh - sl + 1;
    const uint32_t src = s->ti_addr + (((tl * (uint32_t)s->ti_width + sl) << s->ti_size) >> 1);
    uint8_t *tm = s->tmem;
    uint32_t acc = 0;
    if (s->ti_size == 3) { /* 4 texels per TMEM word of each half */
        for (uint32_t wd = 0; wd < (n + 3) / 4; wd++) {
            uint32_t x = (acc >> 11) & 1 ? 4 : 0;
            acc += dxt;
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t o = (t->tmem + wd * 8 + k * 2) ^ x, sa = src + (wd * 4 + k) * 4;
                tm[o & 0x7FF] = ram8(r, sa);
                tm[(o + 1) & 0x7FF] = ram8(r, sa + 1);
                tm[0x800 | (o & 0x7FF)] = ram8(r, sa + 2);
                tm[0x800 | ((o + 1) & 0x7FF)] = ram8(r, sa + 3);
            }
        }
        return;
    }
    const uint32_t bytes = s->ti_size == 0 ? (n + 1) >> 1 : (n << s->ti_size) >> 1;
    for (uint32_t wd = 0; wd < (bytes + 7) / 8; wd++) {
        uint32_t x = (acc >> 11) & 1 ? 4 : 0;
        acc += dxt;
        for (uint32_t b = 0; b < 8; b++)
            tm[((t->tmem + wd * 8 + b) ^ x) & 0xFFF] = ram8(r, src + wd * 8 + b);
    }
}

static void load_tlut(rdp_render *r, uint64_t w)
{
    rdp_state *s = &r->s;
    rdp_tile *t = &s->tile[(w >> 24) & 7];
    set_tile_size(t, w);
    int s0 = t->sl >> 2, s1 = t->sh >> 2;
    if (s1 < s0)
        return;
    uint32_t src = s->ti_addr + (((uint32_t)(t->tl >> 2) * (uint32_t)s->ti_width + (uint32_t)s0) << 1);
    for (int i = 0; i <= s1 - s0; i++) {
        uint8_t hi = ram8(r, src + (uint32_t)i * 2), lo = ram8(r, src + (uint32_t)i * 2 + 1);
        for (int c = 0; c < 4; c++) {
            uint32_t o = (t->tmem + (uint32_t)i * 8 + (uint32_t)c * 2) & 0xFFF;
            s->tmem[o] = hi;
            s->tmem[o + 1] = lo;
        }
    }
}

/* ---- Commands ------------------------------------------------------------------------------------------------------ */
static int cmd_len(int id)
{
    if (id >= 0x08 && id <= 0x0F)
        return 4 + ((id & 4) ? 8 : 0) + ((id & 2) ? 8 : 0) + ((id & 1) ? 2 : 0);
    if (id == 0x24 || id == 0x25)
        return 2;
    return 1;
}

static inline int32_t hi16x(uint64_t w, int i) { return (int32_t)((w >> (48 - 16 * i)) & 0xFFFF); }
static inline int32_t fx(uint64_t wi, uint64_t wf, int i) { return (int32_t)((uint32_t)hi16x(wi, i) << 16 | (uint32_t)hi16x(wf, i)); }

static void triangle(rdp_render *r, const uint64_t *w)
{
    const int id = (int)(w[0] >> 56) & 0x3F;
    rdp_prim p;
    memset(&p, 0, sizeof p);
    p.lft = (int)(w[0] >> 55) & 1;
    p.level = (int)(w[0] >> 51) & 7;
    p.tile = (int)(w[0] >> 48) & 7;
    p.yl = sext((uint32_t)(w[0] >> 32) & 0x3FFF, 14);
    p.ym = sext((uint32_t)(w[0] >> 16) & 0x3FFF, 14);
    p.yh = sext((uint32_t)w[0] & 0x3FFF, 14);
    p.xl = (int32_t)(w[1] >> 32);
    p.dxl = (int32_t)w[1];
    p.xh = (int32_t)(w[2] >> 32);
    p.dxh = (int32_t)w[2];
    p.xm = (int32_t)(w[3] >> 32);
    p.dxm = (int32_t)w[3];
    int i = 4;
    if (id & 4) {
        const uint64_t *c = w + i;
        for (int k = 0; k < 4; k++) {
            p.a0[A_R + k] = fx(c[0], c[2], k);
            p.dadx[A_R + k] = fx(c[1], c[3], k);
            p.dade[A_R + k] = fx(c[4], c[6], k);
            p.dady[A_R + k] = fx(c[5], c[7], k);
        }
        i += 8;
    }
    if (id & 2) {
        const uint64_t *c = w + i;
        for (int k = 0; k < 3; k++) {
            p.a0[A_S + k] = fx(c[0], c[2], k);
            p.dadx[A_S + k] = fx(c[1], c[3], k);
            p.dade[A_S + k] = fx(c[4], c[6], k);
            p.dady[A_S + k] = fx(c[5], c[7], k);
        }
        i += 8;
    }
    uint32_t dz = 0;
    if (id & 1) {
        p.a0[A_Z] = (int32_t)(w[i] >> 32);
        p.dadx[A_Z] = (int32_t)w[i];
        p.dade[A_Z] = (int32_t)(w[i + 1] >> 32);
        p.dady[A_Z] = (int32_t)w[i + 1];
        int64_t d = (llabs((int64_t)p.dadx[A_Z]) + llabs((int64_t)p.dady[A_Z])) >> 16;
        dz = d > 0xFFFF ? 0xFFFF : (uint32_t)d;
    }
    rdp_state *s = &r->s;
    s->max_level = p.level;
    p.primz18 = (uint32_t)(s->prim_z & 0x7FFF) << 3;
    p.dzc = dz_compress(s->om.z_prim ? s->prim_dz : dz);
    raster(r, &p);
}

/* Fill / texture rectangles in 1- and 2-cycle modes: two vertical edges through the same walker. */
static void rect_std(rdp_render *r, int xh, int yh, int xl, int yl, int tn, int32_t S, int32_t T, int32_t dsdx,
                     int32_t dtdy, int flip, int textured)
{
    rdp_prim p;
    memset(&p, 0, sizeof p);
    p.lft = 1;
    p.rect = 1;
    p.tile = tn;
    p.level = r->s.max_level;
    p.yh = yh;
    p.ym = yl;
    p.yl = yl;
    p.xh = xh << 14;
    p.xm = p.xl = xl << 14;
    if (textured) {
        p.a0[A_S] = S * 65536;
        p.a0[A_T] = T * 65536;
        if (flip) {
            p.dade[A_S] = p.dady[A_S] = dsdx * 2048;
            p.dadx[A_T] = dtdy * 2048;
        } else {
            p.dadx[A_S] = dsdx * 2048;
            p.dade[A_T] = p.dady[A_T] = dtdy * 2048;
        }
    }
    p.primz18 = (uint32_t)(r->s.prim_z & 0x7FFF) << 3;
    p.dzc = dz_compress(r->s.prim_dz);
    raster(r, &p);
}

static void set_other_modes(rdp_render *r, uint64_t w)
{
    rdp_modes *m = &r->s.om;
    m->cycle = (uint8_t)((w >> 52) & 3);
    m->persp = (w >> 51) & 1;
    m->detail = (w >> 50) & 1;
    m->sharpen = (w >> 49) & 1;
    m->lod_en = (w >> 48) & 1;
    m->tlut = (w >> 47) & 1;
    m->tlut_ia = (w >> 46) & 1;
    m->bilinear = (w >> 45) & 1;
    m->mid_texel = (w >> 44) & 1;
    m->bilerp[0] = (w >> 43) & 1;
    m->bilerp[1] = (w >> 42) & 1;
    m->convert_one = (w >> 41) & 1;
    m->key = (w >> 40) & 1;
    m->rgb_dither = (w >> 38) & 3;
    m->alpha_dither = (w >> 36) & 3;
    m->m1a[0] = (w >> 30) & 3;
    m->m1a[1] = (w >> 28) & 3;
    m->m1b[0] = (w >> 26) & 3;
    m->m1b[1] = (w >> 24) & 3;
    m->m2a[0] = (w >> 22) & 3;
    m->m2a[1] = (w >> 20) & 3;
    m->m2b[0] = (w >> 18) & 3;
    m->m2b[1] = (w >> 16) & 3;
    m->force_blend = (w >> 14) & 1;
    m->alpha_cvg_sel = (w >> 13) & 1;
    m->cvg_x_alpha = (w >> 12) & 1;
    m->z_mode = (w >> 10) & 3;
    m->cvg_dest = (w >> 8) & 3;
    m->color_on_cvg = (w >> 7) & 1;
    m->image_read = (w >> 6) & 1;
    m->z_update = (w >> 5) & 1;
    m->z_compare = (w >> 4) & 1;
    m->aa = (w >> 3) & 1;
    m->z_prim = (w >> 2) & 1;
    m->dither_alpha = (w >> 1) & 1;
    m->alpha_compare = w & 1;
}

static void set_rgba(int32_t *c, uint32_t v)
{
    c[0] = (int32_t)(v >> 24);
    c[1] = (v >> 16) & 0xFF;
    c[2] = (v >> 8) & 0xFF;
    c[3] = v & 0xFF;
}

static void exec_cmd(rdp_render *r, const uint64_t *w)
{
    rdp_state *s = &r->s;
    const uint64_t w0 = w[0];
    const int id = (int)(w0 >> 56) & 0x3F;
    switch (id) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        if (r->dirty)
            prepare(r);
        triangle(r, w);
        break;
    case 0x24:
    case 0x25: { /* texture rectangle (flip) */
        int xl = (int)(w0 >> 44) & 0xFFF, yl = (int)(w0 >> 32) & 0xFFF, tn = (int)(w0 >> 24) & 7;
        int xh = (int)(w0 >> 12) & 0xFFF, yh = (int)w0 & 0xFFF;
        int32_t S = (int16_t)(w[1] >> 48), T = (int16_t)(w[1] >> 32), dsdx = (int16_t)(w[1] >> 16), dtdy = (int16_t)w[1];
        if (s->om.cycle == CYC_COPY)
            copy_rect(r, xh, yh, xl, yl, tn, S, T, dsdx, dtdy, id == 0x25);
        else if (s->om.cycle == CYC_FILL)
            fill_rect(r, xh, yh, xl, yl);
        else
            rect_std(r, xh, yh, xl, yl, tn, S, T, dsdx, dtdy, id == 0x25, 1);
        break;
    }
    case 0x36: { /* fill rectangle */
        int xl = (int)(w0 >> 44) & 0xFFF, yl = (int)(w0 >> 32) & 0xFFF, xh = (int)(w0 >> 12) & 0xFFF, yh = (int)w0 & 0xFFF;
        if (s->om.cycle == CYC_FILL || s->om.cycle == CYC_COPY)
            fill_rect(r, xh, yh, xl, yl);
        else
            rect_std(r, xh, yh, xl, yl, 0, 0, 0, 0, 0, 0, 0);
        break;
    }
    case 0x2A: /* set key GB */
        s->key_width[1] = (int32_t)(w0 >> 44) & 0xFFF;
        s->key_width[2] = (int32_t)(w0 >> 32) & 0xFFF;
        s->key_center[1] = (int32_t)(w0 >> 24) & 0xFF;
        s->key_scale[1] = (int32_t)(w0 >> 16) & 0xFF;
        s->key_center[2] = (int32_t)(w0 >> 8) & 0xFF;
        s->key_scale[2] = (int32_t)w0 & 0xFF;
        r->dirty = 1;
        break;
    case 0x2B: /* set key R */
        s->key_width[0] = (int32_t)(w0 >> 16) & 0xFFF;
        s->key_center[0] = (int32_t)(w0 >> 8) & 0xFF;
        s->key_scale[0] = (int32_t)w0 & 0xFF;
        r->dirty = 1;
        break;
    case 0x2C: /* set convert */
        for (int i = 0; i < 6; i++)
            s->k[i] = sext((uint32_t)(w0 >> (45 - 9 * i)) & 0x1FF, 9);
        r->dirty = 1;
        break;
    case 0x2D: /* set scissor */
        s->sc_x0 = (int)(w0 >> 44) & 0xFFF;
        s->sc_y0 = (int)(w0 >> 32) & 0xFFF;
        s->sc_field = (int)(w0 >> 25) & 1;
        s->sc_odd = (int)(w0 >> 24) & 1;
        s->sc_x1 = (int)(w0 >> 12) & 0xFFF;
        s->sc_y1 = (int)w0 & 0xFFF;
        break;
    case 0x2E: /* set prim depth */
        s->prim_z = (uint16_t)(w0 >> 16);
        s->prim_dz = (uint16_t)w0;
        break;
    case 0x2F:
        set_other_modes(r, w0);
        r->dirty = 1;
        break;
    case 0x30: load_tlut(r, w0); break;
    case 0x32: set_tile_size(&s->tile[(w0 >> 24) & 7], w0); break;
    case 0x33: load_block(r, w0); break;
    case 0x34: load_tile(r, w0); break;
    case 0x35: { /* set tile */
        rdp_tile *t = &s->tile[(w0 >> 24) & 7];
        t->fmt = (uint8_t)((w0 >> 53) & 7);
        t->size = (uint8_t)((w0 >> 51) & 3);
        t->line = (uint16_t)(((w0 >> 41) & 0x1FF) * 8);
        t->tmem = (uint16_t)(((w0 >> 32) & 0x1FF) * 8);
        t->pal = (uint8_t)((w0 >> 20) & 15);
        t->ct = (w0 >> 19) & 1;
        t->mt = (w0 >> 18) & 1;
        t->mask_t = (uint8_t)((w0 >> 14) & 15);
        t->shift_t = (uint8_t)((w0 >> 10) & 15);
        t->cs = (w0 >> 9) & 1;
        t->ms = (w0 >> 8) & 1;
        t->mask_s = (uint8_t)((w0 >> 4) & 15);
        t->shift_s = (uint8_t)(w0 & 15);
        break;
    }
    case 0x37: s->fill_color = (uint32_t)w0; break;
    case 0x38: set_rgba(s->fog, (uint32_t)w0); break;
    case 0x39: set_rgba(s->blend, (uint32_t)w0); break;
    case 0x3A:
        s->min_level = (int32_t)(w0 >> 40) & 0x1F;
        s->prim_lod_frac = (int32_t)(w0 >> 32) & 0xFF;
        set_rgba(s->prim, (uint32_t)w0);
        r->dirty = 1;
        break;
    case 0x3B:
        set_rgba(s->env, (uint32_t)w0);
        r->dirty = 1;
        break;
    case 0x3C: { /* set combine */
        uint8_t *c0 = s->cc[0], *c1 = s->cc[1];
        c0[0] = (w0 >> 52) & 15;
        c0[2] = (w0 >> 47) & 31;
        c0[4] = (w0 >> 44) & 7;
        c0[6] = (w0 >> 41) & 7;
        c1[0] = (w0 >> 37) & 15;
        c1[2] = (w0 >> 32) & 31;
        c0[1] = (w0 >> 28) & 15;
        c1[1] = (w0 >> 24) & 15;
        c1[4] = (w0 >> 21) & 7;
        c1[6] = (w0 >> 18) & 7;
        c0[3] = (w0 >> 15) & 7;
        c0[5] = (w0 >> 12) & 7;
        c0[7] = (w0 >> 9) & 7;
        c1[3] = (w0 >> 6) & 7;
        c1[5] = (w0 >> 3) & 7;
        c1[7] = w0 & 7;
        r->dirty = 1;
        break;
    }
    case 0x3D:
        s->ti_fmt = (int)(w0 >> 53) & 7;
        s->ti_size = (int)(w0 >> 51) & 3;
        s->ti_width = (int)((w0 >> 32) & 0x3FF) + 1;
        s->ti_addr = (uint32_t)w0 & 0xFFFFFF;
        break;
    case 0x3E: s->zi_addr = (uint32_t)w0 & 0xFFFFFF; break;
    case 0x3F:
        s->ci_fmt = (int)(w0 >> 53) & 7;
        s->ci_size = (int)(w0 >> 51) & 3;
        s->ci_width = (int)((w0 >> 32) & 0x3FF) + 1;
        s->ci_addr = (uint32_t)w0 & 0xFFFFFF;
        r->dirty = 1; /* the span routine depends on the pixel size */
        break;
    default: /* no-op, syncs, undefined */
        break;
    }
}

/* ---- Renderers and threads ----------------------------------------------------------------------------------------- */
static void state_reset(rdp_state *s)
{
    memset(s, 0, sizeof *s);
    s->ci_size = 2;
    s->ci_width = 320;
    s->ti_width = 1;
    s->sc_x1 = s->sc_y1 = 0xFFF;
    s->om.bilerp[0] = s->om.bilerp[1] = 1;
    for (int c = 0; c < 2; c++) {
        s->cc[c][2] = 31; /* RGB mul zero */
        s->cc[c][3] = 7;  /* RGB add zero */
        s->cc[c][6] = 7;
        s->cc[c][7] = 7;
    }
}

static rdp_render *render_new(n64_rdp *d)
{
    rdp_render *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->dp = d;
    r->ram = d->bus.rdram;
    r->ram_size = d->bus.rdram_size;
    r->hidden = d->hidden;
    r->nbands = 1;
    state_reset(&r->s);
    r->dirty = 1;
    return r;
}

static void run_batch(rdp_render *r, const uint64_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n;) {
        int len = cmd_len((int)(w[i] >> 56) & 0x3F);
        exec_cmd(r, w + i);
        i += (uint32_t)len;
    }
}

#ifdef RDP_THREADS
typedef struct {
    n64_rdp *d;
    int id;
} worker_arg;

static void *worker(void *arg)
{
    n64_rdp *d = ((worker_arg *)arg)->d;
    int id = ((worker_arg *)arg)->id;
    free(arg);
    unsigned seen = 0;
    pthread_mutex_lock(&d->mu);
    for (;;) {
        while (!d->quit && d->gen == seen)
            pthread_cond_wait(&d->cv_go, &d->mu);
        if (d->quit)
            break;
        seen = d->gen;
        pthread_mutex_unlock(&d->mu);
        run_batch(d->rd[id], d->batch, d->nbatch);
        pthread_mutex_lock(&d->mu);
        if (--d->running == 0)
            pthread_cond_signal(&d->cv_done);
    }
    pthread_mutex_unlock(&d->mu);
    return NULL;
}

static void threads_stop(n64_rdp *d)
{
    if (!d->threads_up)
        return;
    pthread_mutex_lock(&d->mu);
    d->quit = 1;
    pthread_cond_broadcast(&d->cv_go);
    pthread_mutex_unlock(&d->mu);
    for (int i = 1; i < d->nthreads; i++)
        pthread_join(d->th[i], NULL);
    d->quit = 0;
    d->threads_up = 0;
}
#endif

void n64_rdp_flush(n64_rdp *d)
{
    if (!d->nbatch)
        return;
#ifdef RDP_THREADS
    if (d->threads_up) {
        pthread_mutex_lock(&d->mu);
        d->running = d->nthreads - 1;
        d->gen++;
        pthread_cond_broadcast(&d->cv_go);
        pthread_mutex_unlock(&d->mu);
        run_batch(d->rd[0], d->batch, d->nbatch);
        pthread_mutex_lock(&d->mu);
        while (d->running)
            pthread_cond_wait(&d->cv_done, &d->mu);
        pthread_mutex_unlock(&d->mu);
    } else
#endif
    {
        for (int i = 0; i < d->nthreads; i++)
            run_batch(d->rd[i], d->batch, d->nbatch);
    }
    d->nbatch = 0;
    d->haz_lo = UINT32_MAX;
    d->haz_hi = 0;
}

int n64_rdp_set_threads(n64_rdp *d, int n)
{
    if (n < 1)
        n = 1;
    if (n > N64_RDP_MAX_THREADS)
        n = N64_RDP_MAX_THREADS;
    n64_rdp_flush(d);
#ifdef RDP_THREADS
    threads_stop(d);
#else
    n = 1;
#endif
    for (int i = 1; i < N64_RDP_MAX_THREADS; i++) {
        if (i < n && !d->rd[i])
            d->rd[i] = render_new(d);
        if (i < n && !d->rd[i])
            n = i;
    }
    for (int i = 0; i < n; i++) {
        if (i)
            d->rd[i]->s = d->rd[0]->s;
        d->rd[i]->band = i;
        d->rd[i]->nbands = n;
        d->rd[i]->prim_count = d->rd[0]->prim_count;
        d->rd[i]->dirty = 1;
    }
    d->nthreads = n;
    if (n > 1 && !d->batch) {
        d->batch = malloc(BATCH_MAX * sizeof *d->batch);
        if (!d->batch) {
            for (int i = 0; i < n; i++) {
                d->rd[i]->band = 0;
                d->rd[i]->nbands = 1;
            }
            d->nthreads = 1;
            return 1;
        }
    }
#ifdef RDP_THREADS
    if (n > 1) {
        d->threads_up = 1;
        for (int i = 1; i < n; i++) {
            worker_arg *a = malloc(sizeof *a);
            if (a) {
                a->d = d;
                a->id = i;
            }
            if (!a || pthread_create(&d->th[i], NULL, worker, a) != 0) {
                free(a);
                /* fall back to running the bands one after the other on this thread */
                d->nthreads = i;
                threads_stop(d);
                d->nthreads = n;
                break;
            }
        }
    }
#endif
    return d->nthreads;
}

/* ---- Front end ----------------------------------------------------------------------------------------------------- */
static void haz_add(n64_rdp *d, uint32_t lo, uint32_t hi)
{
    if (lo < d->haz_lo)
        d->haz_lo = lo;
    if (hi > d->haz_hi)
        d->haz_hi = hi;
}

/* Approximate RDP cycles of a command, and front-end bookkeeping. */
static uint32_t cmd_cost(n64_rdp *d, const uint64_t *w, int id)
{
    const uint64_t w0 = w[0];
    const uint32_t per = d->fe_cycle == CYC_2 ? 2 : 1;
    if (id >= 0x08 && id <= 0x0F) {
        int yh = sext((uint32_t)w0 & 0x3FFF, 14), yl = sext((uint32_t)(w0 >> 32) & 0x3FFF, 14);
        int64_t xh = (int32_t)(w[2] >> 32), xm = (int32_t)(w[3] >> 32), xl = (int32_t)(w[1] >> 32);
        int64_t wd = (llabs(xm - xh) + llabs(xl - xh)) >> 17;
        int64_t rows = imax(yl - yh, 0) >> 2;
        int64_t c = rows * (wd + 1) * per + 24;
        return c > 0xFFFFFF ? 0xFFFFFF : (uint32_t)c;
    }
    if (id == 0x24 || id == 0x25 || id == 0x36) {
        int xl = (int)(w0 >> 44) & 0xFFF, yl = (int)(w0 >> 32) & 0xFFF, xh = (int)(w0 >> 12) & 0xFFF, yh = (int)w0 & 0xFFF;
        uint32_t px = (uint32_t)(imax((xl >> 2) - (xh >> 2), 0) + 1) * (uint32_t)(imax((yl >> 2) - (yh >> 2), 0) + 1);
        if (d->fe_cycle == CYC_FILL)
            return px / (d->fe_ci_size == 3 ? 2 : 4) + 8;
        if (d->fe_cycle == CYC_COPY)
            return px / 4 + 8;
        return px * per + 8;
    }
    return 1;
}

static void dispatch(n64_rdp *d, const uint64_t *w, int len)
{
    const uint64_t w0 = w[0];
    const int id = (int)(w0 >> 56) & 0x3F;
    uint32_t cost = cmd_cost(d, w, id);
    d->clock = (d->clock + cost) & 0xFFFFFF;
    d->bufbusy = (d->bufbusy + cost) & 0xFFFFFF;
    const int draw = (id >= 0x08 && id <= 0x0F) || id == 0x24 || id == 0x25 || id == 0x36;
    if (draw) {
        d->pipebusy = (d->pipebusy + cost) & 0xFFFFFF;
        d->status |= ST_PIPE_BUSY | ST_GCLK;
    }
    if (id == 0x30 || id == 0x33 || id == 0x34) {
        uint32_t sh = (uint32_t)(w0 >> 12) & 0xFFF, sl = (uint32_t)(w0 >> 44) & 0xFFF;
        d->tmemc = (d->tmemc + ((sh - sl) >> 2) + 4) & 0xFFFFFF;
    }

    const int threaded = d->nthreads > 1;
    switch (id) {
    case 0x2F: d->fe_cycle = (int)(w0 >> 52) & 3; d->fe_zon = (w0 >> 4 & 3) != 0; break;
    case 0x2D: d->fe_sc_y1 = (int)w0 & 0xFFF; break;
    case 0x3D:
        d->fe_ti_size = (int)(w0 >> 51) & 3;
        d->fe_ti_width = (int)((w0 >> 32) & 0x3FF) + 1;
        d->fe_ti_addr = (uint32_t)w0 & 0xFFFFFF;
        break;
    case 0x3E:
        if (threaded && ((uint32_t)w0 & 0xFFFFFF) != d->fe_zi_addr)
            n64_rdp_flush(d);
        d->fe_zi_addr = (uint32_t)w0 & 0xFFFFFF;
        break;
    case 0x3F: {
        uint32_t a = (uint32_t)w0 & 0xFFFFFF;
        int sz = (int)(w0 >> 51) & 3, wd = (int)((w0 >> 32) & 0x3FF) + 1;
        if (threaded && (a != d->fe_ci_addr || sz != d->fe_ci_size || wd != d->fe_ci_width))
            n64_rdp_flush(d);
        d->fe_ci_addr = a;
        d->fe_ci_size = sz;
        d->fe_ci_width = wd;
        break;
    }
    case 0x30: case 0x33: case 0x34:
        if (threaded && d->nbatch) { /* a load from pixels drawn earlier in the batch waits for them */
            uint32_t lo = d->fe_ti_addr, hi = lo + (uint32_t)d->fe_ti_width * 4u * 1024u;
            if (lo < d->haz_hi && hi > d->haz_lo)
                n64_rdp_flush(d);
        }
        break;
    default: break;
    }
    if (threaded && draw) {
        uint32_t stride = ((uint32_t)d->fe_ci_width << d->fe_ci_size) >> 1;
        uint32_t rows = (uint32_t)(d->fe_sc_y1 >> 2) + 1;
        haz_add(d, d->fe_ci_addr, d->fe_ci_addr + stride * rows);
        if (d->fe_zon)
            haz_add(d, d->fe_zi_addr, d->fe_zi_addr + (uint32_t)d->fe_ci_width * 2u * rows);
    }

    if (!threaded) {
        exec_cmd(d->rd[0], w);
    } else {
        if (d->nbatch + (uint32_t)len > BATCH_MAX)
            n64_rdp_flush(d);
        memcpy(d->batch + d->nbatch, w, (size_t)len * sizeof *w);
        d->nbatch += (uint32_t)len;
    }
    if (id == 0x29) { /* SYNC_FULL: everything drawn, DP interrupt */
        n64_rdp_flush(d);
        d->status &= ~(ST_PIPE_BUSY | ST_GCLK);
        if (d->bus.raise)
            d->bus.raise(d->bus.ctx, N64_MI_DP);
    }
}

static uint64_t fetch64(const n64_rdp *d, uint32_t a)
{
    uint8_t b[8];
    if (d->status & ST_XBUS) {
        if (!d->dmem)
            return 0;
        for (int i = 0; i < 8; i++)
            b[i] = d->dmem[(a + (uint32_t)i) & 0xFFF];
    } else {
        for (int i = 0; i < 8; i++) {
            uint32_t x = (a + (uint32_t)i) & 0xFFFFFF;
            b[i] = x < d->bus.rdram_size ? d->bus.rdram[x] : 0;
        }
    }
    return (uint64_t)n64_be32(b) << 32 | n64_be32(b + 4);
}

n64_rdp *n64_rdp_create(n64_rcp_bus bus)
{
    n64_rdp *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->bus = bus;
    d->hidden = calloc(bus.rdram_size / 2 + 1, 1);
    d->nthreads = 1;
    d->rd[0] = d->hidden ? render_new(d) : NULL;
    if (!d->rd[0]) {
        free(d->hidden);
        free(d);
        return NULL;
    }
#ifdef RDP_THREADS
    pthread_mutex_init(&d->mu, NULL);
    pthread_cond_init(&d->cv_go, NULL);
    pthread_cond_init(&d->cv_done, NULL);
#endif
    n64_rdp_reset(d);
    const char *env = getenv("XBR_N64_RDP_THREADS");
    if (env && atoi(env) > 1)
        n64_rdp_set_threads(d, atoi(env));
    return d;
}

void n64_rdp_destroy(n64_rdp *d)
{
    if (!d)
        return;
#ifdef RDP_THREADS
    threads_stop(d);
    pthread_mutex_destroy(&d->mu);
    pthread_cond_destroy(&d->cv_go);
    pthread_cond_destroy(&d->cv_done);
#endif
    for (int i = 0; i < N64_RDP_MAX_THREADS; i++)
        free(d->rd[i]);
    free(d->batch);
    free(d->hidden);
    free(d);
}

void n64_rdp_reset(n64_rdp *d)
{
    d->nbatch = 0;
    d->start = d->end = d->current = 0;
    d->status = 0;
    d->clock = d->bufbusy = d->pipebusy = d->tmemc = 0;
    d->start_pending = 0;
    d->npend = 0;
    d->fe_cycle = 0;
    d->fe_ci_size = 2;
    d->fe_ci_width = 320;
    d->fe_ci_addr = d->fe_zi_addr = d->fe_ti_addr = 0;
    d->fe_ti_size = 0;
    d->fe_ti_width = 1;
    d->fe_sc_y1 = 0xFFF;
    d->fe_zon = 0;
    d->haz_lo = UINT32_MAX;
    d->haz_hi = 0;
    memset(d->hidden, 0, d->bus.rdram_size / 2 + 1);
    for (int i = 0; i < N64_RDP_MAX_THREADS; i++)
        if (d->rd[i]) {
            state_reset(&d->rd[i]->s);
            d->rd[i]->prim_count = 0;
            d->rd[i]->dirty = 1;
        }
}

void n64_rdp_attach_dmem(n64_rdp *d, const uint8_t *dmem) { d->dmem = dmem; }

uint32_t n64_rdp_read_reg(n64_rdp *d, uint32_t reg)
{
    switch (reg & 7) {
    case 0: return d->start;
    case 1: return d->end;
    case 2: return d->current;
    case 3: {
        uint32_t st = d->status | ST_CBUF_READY;
        if (d->current != d->end || d->npend)
            st |= ST_CMD_BUSY | ST_DMA_BUSY;
        if (d->start_pending)
            st |= ST_START_PENDING;
        return st;
    }
    case 4: return d->clock;
    case 5: return d->bufbusy;
    case 6: return d->pipebusy;
    default: return d->tmemc;
    }
}

void n64_rdp_write_reg(n64_rdp *d, uint32_t reg, uint32_t v)
{
    switch (reg & 7) {
    case 0:
        if (!d->start_pending) {
            d->start = v & 0xFFFFF8;
            d->start_pending = 1;
        }
        break;
    case 1:
        d->end = v & 0xFFFFF8;
        if (d->start_pending) {
            d->current = d->start;
            d->start_pending = 0;
            d->npend = 0; /* a new buffer drops a partial command */
        }
        break;
    case 3:
        if (v & 0x001) d->status &= ~ST_XBUS;
        if (v & 0x002) d->status |= ST_XBUS;
        if (v & 0x004) d->status &= ~ST_FREEZE;
        if (v & 0x008) d->status |= ST_FREEZE;
        if (v & 0x010) d->status &= ~ST_FLUSH;
        if (v & 0x020) d->status |= ST_FLUSH;
        if (v & 0x040) d->tmemc = 0;
        if (v & 0x080) d->pipebusy = 0;
        if (v & 0x100) d->bufbusy = 0;
        if (v & 0x200) d->clock = 0;
        break;
    default: /* CURRENT and the counters are read-only */
        break;
    }
}

void n64_rdp_run(n64_rdp *d)
{
    if (d->status & ST_FREEZE)
        return;
    while (d->current < d->end) {
        d->pend[d->npend++] = fetch64(d, d->current);
        d->current += 8;
        int need = cmd_len((int)(d->pend[0] >> 56) & 0x3F);
        if (d->npend >= need) {
            dispatch(d, d->pend, need);
            d->npend = 0;
        }
    }
}
