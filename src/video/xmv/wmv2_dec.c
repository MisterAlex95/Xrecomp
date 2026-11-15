/* WMV2 video decoder (XBA-70). See wmv2_dec.h; docs/runtime/xmv.md.
 *
 * Written for this project from the behaviour of the format. FFmpeg's wmv2 /
 * msmpeg4 decoder was the reference the output is compared against bit for
 * bit (tests/xmv_decode); the code tables come from it (wmv2_tables.c,
 * LGPL-2.1-or-later). This file holds no FFmpeg code.
 *
 * Layout. A macroblock (MB) is 16x16 luma and two 8x8 chroma blocks; its four
 * luma 8x8 blocks and two chroma blocks are numbered 0..5. The prediction
 * state (DC values, AC rows/columns, coded-block flags, motion vectors) lives
 * in grids with a border row on top and a border column on the left, which
 * hold the "nothing there" values (DC 1024, AC 0, not coded, MV 0): a block's
 * left and top neighbours are always in the grid. */
#include "wmv2_dec.h"
#include "wmv2_tables.h"

#include <stdlib.h>
#include <string.h>

/* ── Bit reader ────────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *buf;   /* big-endian order, 8 zero bytes of padding after it */
    size_t bits;          /* size in bits */
    size_t pos;
} Bits;

static inline uint32_t br_peek(const Bits *b, int n)   /* n in 1..25 */
{
    const uint8_t *p = b->buf + (b->pos >> 3);
    uint32_t w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return (w << (b->pos & 7)) >> (32 - n);
}
static inline void br_skip(Bits *b, int n) { b->pos += (size_t)n; }
static inline uint32_t br_get(Bits *b, int n)
{
    uint32_t v;
    if (n == 0)
        return 0;
    v = br_peek(b, n);
    b->pos += (size_t)n;
    return v;
}
static inline int br_bit(Bits *b) { return (int)br_get(b, 1); }
static inline long br_left(const Bits *b) { return (long)b->bits - (long)b->pos; }
/* 0, 10, 11 -> 0, 1, 2 */
static inline int br_012(Bits *b) { return br_bit(b) ? 1 + br_bit(b) : 0; }

/* ── Variable-length codes ─────────────────────────────────────────────── */

/* A multi-level lookup table: each level indexes the next `bits` bits.
 * len > 0: a symbol whose code ends `len` bits into this level;
 * len < 0: a sub-table at `val` indexed by the next -len bits; len 0: no code. */
typedef struct { int16_t val; int8_t len; } VlcEnt;
typedef struct { VlcEnt *t; int size, used, bits; } Vlc;

typedef struct { uint32_t code; int len; int sym; } VlcCode;

#define VLC_LEVEL_BITS 9

static int vlc_fill(Vlc *v, int off, int nb, const VlcCode *c, int n, int consumed)
{
    int i;
    for (i = 0; i < n; i++) {
        int rem = c[i].len - consumed;
        if (rem <= nb) {
            uint32_t idx = (c[i].code & ((1u << rem) - 1)) << (nb - rem);
            uint32_t k, cnt = 1u << (nb - rem);
            for (k = 0; k < cnt; k++) {
                v->t[off + idx + k].val = (int16_t)c[i].sym;
                v->t[off + idx + k].len = (int8_t)rem;
            }
        }
    }
    /* Longer codes: group them by their prefix in this level. */
    for (i = 0; i < n; i++) {
        int rem = c[i].len - consumed;
        uint32_t prefix;
        int j, m = 0, sub_bits = 0, sub;
        VlcCode *grp;
        if (rem <= nb)
            continue;
        prefix = (c[i].code >> (rem - nb)) & ((1u << nb) - 1);
        if (v->t[off + prefix].len < 0)
            continue;                         /* group done already */
        grp = (VlcCode *)malloc(sizeof(VlcCode) * (size_t)n);
        if (!grp)
            return -1;
        for (j = i; j < n; j++) {
            int r = c[j].len - consumed;
            if (r > nb && ((c[j].code >> (r - nb)) & ((1u << nb) - 1)) == prefix) {
                grp[m] = c[j];
                if (r - nb > sub_bits)
                    sub_bits = r - nb;
                m++;
            }
        }
        if (sub_bits > VLC_LEVEL_BITS)
            sub_bits = VLC_LEVEL_BITS;
        sub = v->used;
        v->used += 1 << sub_bits;
        if (v->used > v->size) {
            int ns = v->used * 2;
            VlcEnt *nt = (VlcEnt *)realloc(v->t, sizeof(VlcEnt) * (size_t)ns);
            if (!nt) { free(grp); return -1; }
            memset(nt + v->size, 0, sizeof(VlcEnt) * (size_t)(ns - v->size));
            v->t = nt;
            v->size = ns;
        }
        v->t[off + prefix].val = (int16_t)sub;
        v->t[off + prefix].len = (int8_t)-sub_bits;
        if (vlc_fill(v, sub, sub_bits, grp, m, consumed + nb) < 0) { free(grp); return -1; }
        free(grp);
    }
    return 0;
}

static int vlc_build(Vlc *v, const VlcCode *c, int n)
{
    v->bits = VLC_LEVEL_BITS;
    v->size = 4 << VLC_LEVEL_BITS;
    v->used = 1 << VLC_LEVEL_BITS;
    v->t = (VlcEnt *)calloc((size_t)v->size, sizeof(VlcEnt));
    if (!v->t)
        return -1;
    return vlc_fill(v, 0, v->bits, c, n, 0);
}

/* Pairs (code, len) of `width`-bit integers, symbol = index; len 0 = unused. */
static int vlc_build_pairs16(Vlc *v, const uint16_t *pairs, int n)
{
    VlcCode *c = (VlcCode *)malloc(sizeof(VlcCode) * (size_t)n);
    int i, m = 0, r;
    if (!c) return -1;
    for (i = 0; i < n; i++)
        if (pairs[2 * i + 1]) { c[m].code = pairs[2 * i]; c[m].len = pairs[2 * i + 1]; c[m].sym = i; m++; }
    r = vlc_build(v, c, m);
    free(c);
    return r;
}
static int vlc_build_pairs32(Vlc *v, const uint32_t *pairs, int n)
{
    VlcCode *c = (VlcCode *)malloc(sizeof(VlcCode) * (size_t)n);
    int i, m = 0, r;
    if (!c) return -1;
    for (i = 0; i < n; i++)
        if (pairs[2 * i + 1]) { c[m].code = pairs[2 * i]; c[m].len = (int)pairs[2 * i + 1]; c[m].sym = i; m++; }
    r = vlc_build(v, c, m);
    free(c);
    return r;
}
static int vlc_build_split(Vlc *v, const uint16_t *codes, const uint8_t *lens, int n)
{
    VlcCode *c = (VlcCode *)malloc(sizeof(VlcCode) * (size_t)n);
    int i, m = 0, r;
    if (!c) return -1;
    for (i = 0; i < n; i++)
        if (lens[i]) { c[m].code = codes[i]; c[m].len = lens[i]; c[m].sym = i; m++; }
    r = vlc_build(v, c, m);
    free(c);
    return r;
}

/* The symbol, or -1 for a bit pattern that is no code. */
static inline int vlc_get(Bits *b, const Vlc *v)
{
    int off = 0, nb = v->bits;
    for (;;) {
        VlcEnt e = v->t[off + (int)br_peek(b, nb)];
        if (e.len > 0) {
            br_skip(b, e.len);
            return e.val;
        }
        if (e.len == 0)
            return -1;
        br_skip(b, nb);
        off = e.val;
        nb = -e.len;
    }
}

/* ── Static tables (built once) ────────────────────────────────────────── */

typedef struct {
    Vlc vlc;
    int n, last;
    const int8_t *run, *level;
    int8_t max_level[2][65];   /* [last][run]: largest level coded with that run */
    int8_t max_run[2][65];     /* [last][level]: largest run coded with that level */
} RlTable;

typedef struct {
    int ready;
    RlTable rl[6];
    Vlc mv[2];
    const uint8_t *mvx[2], *mvy[2];
    Vlc mb_inter[4];
    Vlc mb_intra;
    Vlc dc[2][2];
} Wmv2Static;

static Wmv2Static g_tab;

static int rl_init(RlTable *rl, const uint16_t *vlc, const int8_t *run, const int8_t *level, int n, int last)
{
    int i, l;
    rl->n = n;
    rl->last = last;
    rl->run = run;
    rl->level = level;
    memset(rl->max_level, 0, sizeof rl->max_level);
    memset(rl->max_run, 0, sizeof rl->max_run);
    for (i = 0; i < n; i++) {
        l = i >= last;
        if (level[i] > rl->max_level[l][run[i]])
            rl->max_level[l][run[i]] = level[i];
        if (run[i] > rl->max_run[l][level[i]])
            rl->max_run[l][level[i]] = run[i];
    }
    return vlc_build_pairs16(&rl->vlc, vlc, n + 1);
}

static int wmv2_static_init(void)
{
    /* One-time, not thread-safe on the very first call: the player creates
     * its decoders from one thread. Idempotent afterwards. */
    Wmv2Static *t = &g_tab;
    int i, j, ok = 0;
    if (t->ready)
        return 0;
    ok |= rl_init(&t->rl[0], wmv2_rl0_vlc, wmv2_rl0_run, wmv2_rl0_level, 132, 85);
    ok |= rl_init(&t->rl[1], wmv2_rl1_vlc, wmv2_rl1_run, wmv2_rl1_level, 185, 119);
    ok |= rl_init(&t->rl[2], wmv2_rl2_vlc, wmv2_rl2_run, wmv2_rl2_level, 102, 67);
    ok |= rl_init(&t->rl[3], wmv2_rl3_vlc, wmv2_rl3_run, wmv2_rl3_level, 148, 81);
    ok |= rl_init(&t->rl[4], wmv2_rl4_vlc, wmv2_rl4_run, wmv2_rl4_level, 168, 99);
    ok |= rl_init(&t->rl[5], wmv2_rl5_vlc, wmv2_rl5_run, wmv2_rl5_level, 102, 58);
    ok |= vlc_build_split(&t->mv[0], wmv2_mv0_code, wmv2_mv0_bits, 1100);
    ok |= vlc_build_split(&t->mv[1], wmv2_mv1_code, wmv2_mv1_bits, 1100);
    t->mvx[0] = wmv2_mv0_x; t->mvy[0] = wmv2_mv0_y;
    t->mvx[1] = wmv2_mv1_x; t->mvy[1] = wmv2_mv1_y;
    ok |= vlc_build_pairs32(&t->mb_inter[0], wmv2_mb_inter0, 128);
    ok |= vlc_build_pairs32(&t->mb_inter[1], wmv2_mb_inter1, 128);
    ok |= vlc_build_pairs32(&t->mb_inter[2], wmv2_mb_inter2, 128);
    ok |= vlc_build_pairs32(&t->mb_inter[3], wmv2_mb_inter3, 128);
    ok |= vlc_build_pairs16(&t->mb_intra, wmv2_mb_intra, 64);
    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++)
            ok |= vlc_build_pairs32(&t->dc[i][j], wmv2_dc_vlc + (i * 2 + j) * 240, 120);
    if (ok)
        return -1;
    t->ready = 1;
    return 0;
}

/* ── Decoder state ─────────────────────────────────────────────────────── */

enum { SKIP_NONE = 0, SKIP_MPEG = 1, SKIP_ROW = 2, SKIP_COL = 3 };

struct Wmv2Dec {
    int width, height;          /* the picture */
    int mb_w, mb_h;             /* in macroblocks */
    int cw, ch;                 /* coded luma size (mb_w * 16, mb_h * 16) */

    uint8_t *pic[2][3];         /* two pictures, coded size, planes Y U V */
    int cur;                    /* pic[cur] is the current / last decoded picture */
    int have_ref;

    /* Sequence header. */
    int mspel_bit, loop_filter, abt_flag, j_type_bit, top_left_mv_flag, per_mb_rl_bit;
    int slice_height;

    /* Picture header. */
    int key, qscale, y_dc_scale, c_dc_scale;
    int rl_table_index, rl_chroma_table_index, dc_table_index, mv_table_index;
    int per_mb_rl_table, cbp_table_index, mspel, per_mb_abt, abt_type, skip_type;
    int no_rounding;
    int esc3_level_length, esc3_run_length;

    /* Macroblock state. */
    int mb_x, mb_y, first_slice_line, ac_pred, mb_intra, per_block_abt, hshift;
    int abt_type_table[6];
    int last_index[6];
    int16_t block[6][64];
    int16_t block2[6][64];      /* the second half of an 8x4 / 4x8 split */

    /* Prediction grids (border row and column). */
    int w8;                     /* luma 8x8 grid stride: 2 * mb_w + 1 */
    int wm;                     /* MB grid stride: mb_w + 1 */
    int16_t *dc_y, *dc_c[2];    /* DC (scaled), 1024 = none */
    int16_t *ac_y, *ac_c[2];    /* 16 per block: [1..7] left column, [9..15] top row */
    uint8_t *coded;             /* luma coded-block flags */
    int16_t *mv;                /* 2 per luma 8x8 block, no border: the ffmpeg-style b8 grid */
    uint8_t *skip;              /* per MB, P frames */
    int16_t *mv_base;

    Bits gb;
    uint8_t *swap;              /* the frame in big-endian order, padded */
    size_t swap_size;
};

static inline int clip_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline int mid3(int a, int b, int c)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) b = c;
    return a > b ? a : b;
}

/* Index of luma 8x8 block (bx, by) in the bordered grids. */
static inline int yidx(const Wmv2Dec *d, int bx, int by) { return (by + 1) * d->w8 + bx + 1; }
static inline int cidx(const Wmv2Dec *d, int mx, int my) { return (my + 1) * d->wm + mx + 1; }
/* Index of luma 8x8 block in the MV grid (stride w8, an unused last column). */
static inline int mvidx(const Wmv2Dec *d, int bx, int by) { return by * d->w8 + bx; }

Wmv2Dec *wmv2_dec_create(int width, int height)
{
    Wmv2Dec *d;
    int i, ysz, csz, gy, gc;
    if (width < 2 || height < 2 || width > 4096 || height > 4096)
        return NULL;
    if (wmv2_static_init() < 0)
        return NULL;
    d = (Wmv2Dec *)calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->width = width;
    d->height = height;
    d->mb_w = (width + 15) >> 4;
    d->mb_h = (height + 15) >> 4;
    d->cw = d->mb_w * 16;
    d->ch = d->mb_h * 16;
    ysz = d->cw * d->ch;
    csz = ysz / 4;
    for (i = 0; i < 2; i++) {
        d->pic[i][0] = (uint8_t *)malloc((size_t)ysz);
        d->pic[i][1] = (uint8_t *)malloc((size_t)csz);
        d->pic[i][2] = (uint8_t *)malloc((size_t)csz);
        if (!d->pic[i][0] || !d->pic[i][1] || !d->pic[i][2]) {
            wmv2_dec_destroy(d);
            return NULL;
        }
        memset(d->pic[i][0], 16, (size_t)ysz);       /* black until a key frame */
        memset(d->pic[i][1], 128, (size_t)csz);
        memset(d->pic[i][2], 128, (size_t)csz);
    }
    d->w8 = 2 * d->mb_w + 1;
    d->wm = d->mb_w + 1;
    gy = d->w8 * (2 * d->mb_h + 1);
    gc = d->wm * (d->mb_h + 1);
    d->dc_y = (int16_t *)malloc(sizeof(int16_t) * (size_t)gy);
    d->dc_c[0] = (int16_t *)malloc(sizeof(int16_t) * (size_t)gc);
    d->dc_c[1] = (int16_t *)malloc(sizeof(int16_t) * (size_t)gc);
    d->ac_y = (int16_t *)calloc((size_t)gy * 16, sizeof(int16_t));
    d->ac_c[0] = (int16_t *)calloc((size_t)gc * 16, sizeof(int16_t));
    d->ac_c[1] = (int16_t *)calloc((size_t)gc * 16, sizeof(int16_t));
    d->coded = (uint8_t *)calloc((size_t)gy, 1);
    /* MV grid: one row of slack before it (read, never used, at the top MB row). */
    d->mv_base = (int16_t *)calloc((size_t)(d->w8 * (2 * d->mb_h + 1) + 4) * 2, sizeof(int16_t));
    d->skip = (uint8_t *)calloc((size_t)(d->mb_w * d->mb_h), 1);
    if (!d->dc_y || !d->dc_c[0] || !d->dc_c[1] || !d->ac_y || !d->ac_c[0] || !d->ac_c[1] ||
        !d->coded || !d->mv_base || !d->skip) {
        wmv2_dec_destroy(d);
        return NULL;
    }
    d->mv = d->mv_base + (d->w8 + 2) * 2;
    for (i = 0; i < gy; i++) d->dc_y[i] = 1024;
    for (i = 0; i < gc; i++) d->dc_c[0][i] = d->dc_c[1][i] = 1024;
    d->slice_height = d->mb_h;
    return d;
}

void wmv2_dec_destroy(Wmv2Dec *d)
{
    int i;
    if (!d)
        return;
    for (i = 0; i < 2; i++) {
        free(d->pic[i][0]);
        free(d->pic[i][1]);
        free(d->pic[i][2]);
    }
    free(d->dc_y); free(d->dc_c[0]); free(d->dc_c[1]);
    free(d->ac_y); free(d->ac_c[0]); free(d->ac_c[1]);
    free(d->coded); free(d->mv_base); free(d->skip);
    free(d->swap);
    free(d);
}

static void set_header(Wmv2Dec *d, int mspel, int loop, int abt, int jt, int tlmv, int mbrl, int slices)
{
    d->mspel_bit = mspel;
    d->loop_filter = loop;
    d->abt_flag = abt;
    d->j_type_bit = jt;
    d->top_left_mv_flag = tlmv;
    d->per_mb_rl_bit = mbrl;
    /* No slice count: one slice (and never a division by zero). */
    d->slice_height = slices ? d->mb_h / slices : d->mb_h;
}

void wmv2_dec_set_xmv_header(Wmv2Dec *d, uint32_t w)
{
    set_header(d, w & 1, (w >> 1) & 1, (w >> 2) & 1, (w >> 3) & 1, (w >> 4) & 1, (w >> 5) & 1,
               (int)((w >> 6) & 7));
}

void wmv2_dec_set_wmv_extradata(Wmv2Dec *d, const uint8_t e[4])
{
    uint32_t w = ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) | e[3];
    set_header(d, (w >> 15) & 1, (w >> 14) & 1, (w >> 13) & 1, (w >> 12) & 1, (w >> 11) & 1,
               (w >> 10) & 1, (int)((w >> 7) & 7));
}

const uint8_t *wmv2_dec_plane(const Wmv2Dec *d, int plane, int *stride)
{
    if (plane < 0 || plane > 2)
        return NULL;
    if (stride)
        *stride = plane ? d->cw / 2 : d->cw;
    return d->pic[d->cur][plane];
}
int wmv2_dec_width(const Wmv2Dec *d) { return d->width; }
int wmv2_dec_height(const Wmv2Dec *d) { return d->height; }
int wmv2_dec_last_was_key(const Wmv2Dec *d) { return d->key; }

/* ── Inverse transforms ────────────────────────────────────────────────── */

/* The WMV2 8x8 IDCT: rows, then columns with extra precision. */
#define W0 2048
#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

static void idct_row(int16_t *b)
{
    int a0, a1, a2, a3, a4, a5, a6, a7, s1, s2;
    a1 = W1 * b[1] + W7 * b[7];
    a7 = W7 * b[1] - W1 * b[7];
    a5 = W5 * b[5] + W3 * b[3];
    a3 = W3 * b[5] - W5 * b[3];
    a2 = W2 * b[2] + W6 * b[6];
    a6 = W6 * b[2] - W2 * b[6];
    a0 = W0 * b[0] + W0 * b[4];
    a4 = W0 * b[0] - W0 * b[4];
    s1 = (int)(181u * (unsigned)(a1 - a5 + a7 - a3) + 128u) >> 8;
    s2 = (int)(181u * (unsigned)(a1 - a5 - a7 + a3) + 128u) >> 8;
    b[0] = (int16_t)((a0 + a2 + a1 + a5 + 128) >> 8);
    b[1] = (int16_t)((a4 + a6 + s1 + 128) >> 8);
    b[2] = (int16_t)((a4 - a6 + s2 + 128) >> 8);
    b[3] = (int16_t)((a0 - a2 + a7 + a3 + 128) >> 8);
    b[4] = (int16_t)((a0 - a2 - a7 - a3 + 128) >> 8);
    b[5] = (int16_t)((a4 - a6 - s2 + 128) >> 8);
    b[6] = (int16_t)((a4 + a6 - s1 + 128) >> 8);
    b[7] = (int16_t)((a0 + a2 - a1 - a5 + 128) >> 8);
}

static void idct_col(int16_t *b)
{
    int a0, a1, a2, a3, a4, a5, a6, a7, s1, s2;
    a1 = (W1 * b[8] + W7 * b[56] + 4) >> 3;
    a7 = (W7 * b[8] - W1 * b[56] + 4) >> 3;
    a5 = (W5 * b[40] + W3 * b[24] + 4) >> 3;
    a3 = (W3 * b[40] - W5 * b[24] + 4) >> 3;
    a2 = (W2 * b[16] + W6 * b[48] + 4) >> 3;
    a6 = (W6 * b[16] - W2 * b[48] + 4) >> 3;
    a0 = (W0 * b[0] + W0 * b[32]) >> 3;
    a4 = (W0 * b[0] - W0 * b[32]) >> 3;
    s1 = (int)(181u * (unsigned)(a1 - a5 + a7 - a3) + 128u) >> 8;
    s2 = (int)(181u * (unsigned)(a1 - a5 - a7 + a3) + 128u) >> 8;
    b[0]  = (int16_t)((a0 + a2 + a1 + a5 + 8192) >> 14);
    b[8]  = (int16_t)((a4 + a6 + s1 + 8192) >> 14);
    b[16] = (int16_t)((a4 - a6 + s2 + 8192) >> 14);
    b[24] = (int16_t)((a0 - a2 + a7 + a3 + 8192) >> 14);
    b[32] = (int16_t)((a0 - a2 - a7 - a3 + 8192) >> 14);
    b[40] = (int16_t)((a4 - a6 - s2 + 8192) >> 14);
    b[48] = (int16_t)((a4 + a6 - s1 + 8192) >> 14);
    b[56] = (int16_t)((a0 + a2 - a1 - a5 + 8192) >> 14);
}

static void idct8x8(int16_t *b)
{
    int i;
    for (i = 0; i < 64; i += 8) idct_row(b + i);
    for (i = 0; i < 8; i++) idct_col(b + i);
}

static void idct_put(uint8_t *dst, int stride, int16_t *b)
{
    int x, y;
    idct8x8(b);
    for (y = 0; y < 8; y++, dst += stride, b += 8)
        for (x = 0; x < 8; x++) dst[x] = (uint8_t)clip_u8(b[x]);
}

static void idct_add(uint8_t *dst, int stride, int16_t *b)
{
    int x, y;
    idct8x8(b);
    for (y = 0; y < 8; y++, dst += stride, b += 8)
        for (x = 0; x < 8; x++) dst[x] = (uint8_t)clip_u8(dst[x] + b[x]);
}

/* The adaptive block transform's 8-point and 4-point stages (the "simple"
 * integer IDCT's constants, as the format's 8x4 / 4x8 transforms use). */
#define S1 22725
#define S2 21407
#define S3 19266
#define S4 16383
#define S5 12873
#define S6 8867
#define S7 4520
#define S_ROW_SHIFT 11
#define S_COL_SHIFT 20

static void simple_row8(int16_t *r)
{
    int a0, a1, a2, a3, b0, b1, b2, b3;
    if (!(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) {
        int16_t v = (int16_t)(uint16_t)((unsigned)r[0] << 3);
        int i;
        for (i = 0; i < 8; i++) r[i] = v;
        return;
    }
    a0 = S4 * r[0] + (1 << (S_ROW_SHIFT - 1));
    a1 = a0; a2 = a0; a3 = a0;
    a0 += S2 * r[2]; a1 += S6 * r[2]; a2 -= S6 * r[2]; a3 -= S2 * r[2];
    b0 = S1 * r[1] + S3 * r[3];
    b1 = S3 * r[1] - S7 * r[3];
    b2 = S5 * r[1] - S1 * r[3];
    b3 = S7 * r[1] - S5 * r[3];
    a0 += S4 * r[4] + S6 * r[6];
    a1 += -S4 * r[4] - S2 * r[6];
    a2 += -S4 * r[4] + S2 * r[6];
    a3 += S4 * r[4] - S6 * r[6];
    b0 += S5 * r[5] + S7 * r[7];
    b1 += -S1 * r[5] - S5 * r[7];
    b2 += S7 * r[5] + S3 * r[7];
    b3 += S3 * r[5] - S1 * r[7];
    r[0] = (int16_t)((a0 + b0) >> S_ROW_SHIFT);
    r[7] = (int16_t)((a0 - b0) >> S_ROW_SHIFT);
    r[1] = (int16_t)((a1 + b1) >> S_ROW_SHIFT);
    r[6] = (int16_t)((a1 - b1) >> S_ROW_SHIFT);
    r[2] = (int16_t)((a2 + b2) >> S_ROW_SHIFT);
    r[5] = (int16_t)((a2 - b2) >> S_ROW_SHIFT);
    r[3] = (int16_t)((a3 + b3) >> S_ROW_SHIFT);
    r[4] = (int16_t)((a3 - b3) >> S_ROW_SHIFT);
}

static void simple_col8_add(uint8_t *dst, int stride, const int16_t *c)
{
    int a0, a1, a2, a3, b0, b1, b2, b3;
    a0 = S4 * (c[0] + ((1 << (S_COL_SHIFT - 1)) / S4));
    a1 = a0; a2 = a0; a3 = a0;
    a0 += S2 * c[16]; a1 += S6 * c[16]; a2 -= S6 * c[16]; a3 -= S2 * c[16];
    b0 = S1 * c[8] + S3 * c[24];
    b1 = S3 * c[8] - S7 * c[24];
    b2 = S5 * c[8] - S1 * c[24];
    b3 = S7 * c[8] - S5 * c[24];
    a0 += S4 * c[32]; a1 -= S4 * c[32]; a2 -= S4 * c[32]; a3 += S4 * c[32];
    b0 += S5 * c[40]; b1 -= S1 * c[40]; b2 += S7 * c[40]; b3 += S3 * c[40];
    a0 += S6 * c[48]; a1 -= S2 * c[48]; a2 += S2 * c[48]; a3 -= S6 * c[48];
    b0 += S7 * c[56]; b1 -= S5 * c[56]; b2 += S3 * c[56]; b3 -= S1 * c[56];
    dst[0 * stride] = (uint8_t)clip_u8(dst[0 * stride] + ((a0 + b0) >> S_COL_SHIFT));
    dst[1 * stride] = (uint8_t)clip_u8(dst[1 * stride] + ((a1 + b1) >> S_COL_SHIFT));
    dst[2 * stride] = (uint8_t)clip_u8(dst[2 * stride] + ((a2 + b2) >> S_COL_SHIFT));
    dst[3 * stride] = (uint8_t)clip_u8(dst[3 * stride] + ((a3 + b3) >> S_COL_SHIFT));
    dst[4 * stride] = (uint8_t)clip_u8(dst[4 * stride] + ((a3 - b3) >> S_COL_SHIFT));
    dst[5 * stride] = (uint8_t)clip_u8(dst[5 * stride] + ((a2 - b2) >> S_COL_SHIFT));
    dst[6 * stride] = (uint8_t)clip_u8(dst[6 * stride] + ((a1 - b1) >> S_COL_SHIFT));
    dst[7 * stride] = (uint8_t)clip_u8(dst[7 * stride] + ((a0 - b0) >> S_COL_SHIFT));
}

/* 4-point stages: C* with 12 fractional bits (columns), R* with 15 (rows). */
#define C4_1 3784   /* round(0.6532814824 * sqrt(2) * 4096) */
#define C4_2 1567   /* round(0.2705980501 * sqrt(2) * 4096) */
#define C4_3 2896   /* round(0.5 * sqrt(2) * 4096) */
#define C4_SHIFT 17
#define R4_1 30274
#define R4_2 12540
#define R4_3 23170
#define R4_SHIFT 11

static void col4_add(uint8_t *dst, int stride, const int16_t *c)
{
    int c0, c1, c2, c3, a0 = c[0], a1 = c[8], a2 = c[16], a3 = c[24];
    c0 = (a0 + a2) * C4_3 + (1 << (C4_SHIFT - 1));
    c2 = (a0 - a2) * C4_3 + (1 << (C4_SHIFT - 1));
    c1 = a1 * C4_1 + a3 * C4_2;
    c3 = a1 * C4_2 - a3 * C4_1;
    dst[0]          = (uint8_t)clip_u8(dst[0]          + ((c0 + c1) >> C4_SHIFT));
    dst[stride]     = (uint8_t)clip_u8(dst[stride]     + ((c2 + c3) >> C4_SHIFT));
    dst[2 * stride] = (uint8_t)clip_u8(dst[2 * stride] + ((c2 - c3) >> C4_SHIFT));
    dst[3 * stride] = (uint8_t)clip_u8(dst[3 * stride] + ((c0 - c1) >> C4_SHIFT));
}

static void row4(int16_t *r)
{
    unsigned c0, c1, c2, c3;
    int a0 = r[0], a1 = r[1], a2 = r[2], a3 = r[3];
    c0 = (unsigned)((a0 + a2) * R4_3 + (1 << (R4_SHIFT - 1)));
    c2 = (unsigned)((a0 - a2) * R4_3 + (1 << (R4_SHIFT - 1)));
    c1 = (unsigned)(a1 * R4_1 + a3 * R4_2);
    c3 = (unsigned)(a1 * R4_2 - a3 * R4_1);
    r[0] = (int16_t)((c0 + c1) >> R4_SHIFT);
    r[1] = (int16_t)((c2 + c3) >> R4_SHIFT);
    r[2] = (int16_t)((c2 - c3) >> R4_SHIFT);
    r[3] = (int16_t)((c0 - c1) >> R4_SHIFT);
}

/* 8 wide, 4 high: an 8-point transform on each of 4 rows, 4-point columns. */
static void idct84_add(uint8_t *dst, int stride, int16_t *b)
{
    int i;
    for (i = 0; i < 4; i++) simple_row8(b + i * 8);
    for (i = 0; i < 8; i++) col4_add(dst + i, stride, b + i);
}

/* 4 wide, 8 high. */
static void idct48_add(uint8_t *dst, int stride, int16_t *b)
{
    int i;
    for (i = 0; i < 8; i++) row4(b + i * 8);
    for (i = 0; i < 4; i++) simple_col8_add(dst + i, stride, b + i);
}

/* ── Motion compensation ───────────────────────────────────────────────── */

/* Copies a w x h area at (x, y) of a plane (pw x ph) into dst, repeating
 * the edge pixels for what lies outside. */
static void fetch_clamped(uint8_t *dst, int dstride, const uint8_t *src, int sstride,
                          int pw, int ph, int x, int y, int w, int h)
{
    int i, j;
    for (j = 0; j < h; j++) {
        int sy = y + j;
        const uint8_t *row;
        sy = sy < 0 ? 0 : sy >= ph ? ph - 1 : sy;
        row = src + (size_t)sy * sstride;
        for (i = 0; i < w; i++) {
            int sx = x + i;
            sx = sx < 0 ? 0 : sx >= pw ? pw - 1 : sx;
            dst[j * dstride + i] = row[sx];
        }
    }
}

/* Half-pel prediction of a w x h block (dxy: bit 0 half x, bit 1 half y).
 * src must have one extra column and row readable. */
static void hpel_put(uint8_t *dst, int dstride, const uint8_t *src, int sstride,
                     int w, int h, int dxy, int no_rnd)
{
    int x, y;
    for (y = 0; y < h; y++, dst += dstride, src += sstride) {
        for (x = 0; x < w; x++) {
            int v;
            switch (dxy) {
            case 0: v = src[x]; break;
            case 1: v = (src[x] + src[x + 1] + 1 - no_rnd) >> 1; break;
            case 2: v = (src[x] + src[x + sstride] + 1 - no_rnd) >> 1; break;
            default:
                v = (src[x] + src[x + 1] + src[x + sstride] + src[x + sstride + 1] + 2 - no_rnd) >> 2;
                break;
            }
            dst[x] = (uint8_t)v;
        }
    }
}

/* mspel: the format's 4-tap (-1, 9, 9, -1) half-pel filter. */
static void mspel_h(uint8_t *dst, int ds, const uint8_t *src, int ss, int h)
{
    int x, y;
    for (y = 0; y < h; y++, dst += ds, src += ss)
        for (x = 0; x < 8; x++)
            dst[x] = (uint8_t)clip_u8((9 * (src[x] + src[x + 1]) - (src[x - 1] + src[x + 2]) + 8) >> 4);
}
static void mspel_v(uint8_t *dst, int ds, const uint8_t *src, int ss, int w)
{
    int x, y;
    for (x = 0; x < w; x++)
        for (y = 0; y < 8; y++) {
            const uint8_t *s = src + x + y * ss;
            dst[y * ds + x] = (uint8_t)clip_u8((9 * (s[0] + s[ss]) - (s[-ss] + s[2 * ss]) + 8) >> 4);
        }
}
static void avg8(uint8_t *dst, int ds, const uint8_t *a, int as, const uint8_t *b, int bs)
{
    int x, y;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            dst[y * ds + x] = (uint8_t)((a[y * as + x] + b[y * bs + x] + 1) >> 1);
}

/* One 8x8 luma quarter of an mspel prediction; src has 1 pixel before and 2
 * after readable in both directions. */
static void mspel8(uint8_t *dst, int ds, const uint8_t *src, int ss, int mode)
{
    uint8_t half[64], hh[88], hv[64], hhv[64];
    int y;
    switch (mode) {
    case 0:
        for (y = 0; y < 8; y++) memcpy(dst + y * ds, src + y * ss, 8);
        break;
    case 1: mspel_h(half, 8, src, ss, 8); avg8(dst, ds, src, ss, half, 8); break;
    case 2: mspel_h(dst, ds, src, ss, 8); break;
    case 3: mspel_h(half, 8, src, ss, 8); avg8(dst, ds, src + 1, ss, half, 8); break;
    case 4: mspel_v(dst, ds, src, ss, 8); break;
    case 5:
        mspel_h(hh, 8, src - ss, ss, 11);
        mspel_v(hv, 8, src, ss, 8);
        mspel_v(hhv, 8, hh + 8, 8, 8);
        avg8(dst, ds, hv, 8, hhv, 8);
        break;
    case 6:
        mspel_h(hh, 8, src - ss, ss, 11);
        mspel_v(dst, ds, hh + 8, 8, 8);
        break;
    default:
        mspel_h(hh, 8, src - ss, ss, 11);
        mspel_v(hv, 8, src + 1, ss, 8);
        mspel_v(hhv, 8, hh + 8, 8, 8);
        avg8(dst, ds, hv, 8, hhv, 8);
        break;
    }
}

/* Predicts the current MB from the reference picture with vector (mx, my)
 * in half-pel units. */
static void motion(Wmv2Dec *d, int mx, int my)
{
    uint8_t *const *ref = d->pic[d->cur ^ 1];
    uint8_t *const *out = d->pic[d->cur];
    int ys = d->cw, cs = d->cw / 2;
    uint8_t *dy = out[0] + (size_t)d->mb_y * 16 * ys + d->mb_x * 16;
    uint8_t *du = out[1] + (size_t)d->mb_y * 8 * cs + d->mb_x * 8;
    uint8_t *dv = out[2] + (size_t)d->mb_y * 8 * cs + d->mb_x * 8;
    uint8_t tmp[24 * 24];
    int sx, sy, dxy, cx, cy, cdxy;

    if (d->mspel) {
        dxy = 2 * (((my & 1) << 1) | (mx & 1)) + d->hshift;
        sx = d->mb_x * 16 + (mx >> 1);
        sy = d->mb_y * 16 + (my >> 1);
        sx = sx < -16 ? -16 : sx > d->width ? d->width : sx;
        sy = sy < -16 ? -16 : sy > d->height ? d->height : sy;
        if (sx <= -16 || sx >= d->width) dxy &= ~3;
        if (sy <= -16 || sy >= d->height) dxy &= ~4;
        /* 1 pixel before, 2 after: 19 x 19 around the 16 x 16 block. */
        fetch_clamped(tmp, 24, ref[0], ys, d->cw, d->ch, sx - 1, sy - 1, 19, 19);
        {
            const uint8_t *p = tmp + 24 + 1;
            mspel8(dy, ys, p, 24, dxy);
            mspel8(dy + 8, ys, p + 8, 24, dxy);
            mspel8(dy + 8 * ys, ys, p + 8 * 24, 24, dxy);
            mspel8(dy + 8 + 8 * ys, ys, p + 8 + 8 * 24, 24, dxy);
        }
        cdxy = ((mx & 3) != 0) | (((my & 3) != 0) << 1);
        cx = d->mb_x * 8 + (mx >> 2);
        cy = d->mb_y * 8 + (my >> 2);
        cx = cx < -8 ? -8 : cx > (d->width >> 1) ? (d->width >> 1) : cx;
        if (cx == (d->width >> 1)) cdxy &= ~1;
        cy = cy < -8 ? -8 : cy > (d->height >> 1) ? (d->height >> 1) : cy;
        if (cy == (d->height >> 1)) cdxy &= ~2;
    } else {
        dxy = ((my & 1) << 1) | (mx & 1);
        sx = d->mb_x * 16 + (mx >> 1);
        sy = d->mb_y * 16 + (my >> 1);
        fetch_clamped(tmp, 24, ref[0], ys, d->cw, d->ch, sx, sy, 17, 17);
        hpel_put(dy, ys, tmp, 24, 16, 16, dxy, d->no_rounding);
        /* H.263 chroma: half the luma vector, any fraction rounds to a half. */
        cdxy = dxy | (my & 2) | ((mx & 2) >> 1);
        cx = sx >> 1;
        cy = sy >> 1;
    }
    fetch_clamped(tmp, 24, ref[1], cs, d->cw / 2, d->ch / 2, cx, cy, 9, 9);
    hpel_put(du, cs, tmp, 24, 8, 8, cdxy, d->no_rounding);
    fetch_clamped(tmp, 24, ref[2], cs, d->cw / 2, d->ch / 2, cx, cy, 9, 9);
    hpel_put(dv, cs, tmp, 24, 8, 8, cdxy, d->no_rounding);
}

/* ── The H.263 deblocking loop filter ──────────────────────────────────── */

static void lf_edge(uint8_t *src, int step, int along, int qscale)
{
    /* step: from one side of the edge to the other; along: along the edge. */
    int i, strength = wmv2_loop_strength[qscale];
    for (i = 0; i < 8; i++, src += along) {
        int p0 = src[-2 * step], p1 = src[-step], p2 = src[0], p3 = src[step];
        int d = (p0 - p3 + 4 * (p2 - p1)) / 8, d1, d2, ad1;
        if (d < -2 * strength) d1 = 0;
        else if (d < -strength) d1 = -2 * strength - d;
        else if (d < strength) d1 = d;
        else if (d < 2 * strength) d1 = 2 * strength - d;
        else d1 = 0;
        p1 += d1;
        p2 -= d1;
        if (p1 & 256) p1 = ~(p1 >> 31);
        if (p2 & 256) p2 = ~(p2 >> 31);
        src[-step] = (uint8_t)p1;
        src[0] = (uint8_t)p2;
        ad1 = iabs(d1) >> 1;
        d2 = (p0 - p3) / 4;
        d2 = d2 < -ad1 ? -ad1 : d2 > ad1 ? ad1 : d2;
        src[-2 * step] = (uint8_t)(p0 - d2);
        src[step] = (uint8_t)(p3 + d2);
    }
}
/* Across a horizontal edge (the rows above and below it). */
static void lf_v(uint8_t *src, int stride, int q) { lf_edge(src, stride, 1, q); }
/* Across a vertical edge. */
static void lf_h(uint8_t *src, int stride, int q) { lf_edge(src, 1, stride, q); }

static void loop_filter(Wmv2Dec *d)
{
    int ys = d->cw, cs = d->cw / 2;
    uint8_t **out = d->pic[d->cur];
    uint8_t *dy = out[0] + (size_t)d->mb_y * 16 * ys + d->mb_x * 16;
    uint8_t *du = out[1] + (size_t)d->mb_y * 8 * cs + d->mb_x * 8;
    uint8_t *dv = out[2] + (size_t)d->mb_y * 8 * cs + d->mb_x * 8;
    int xy = d->mb_y * d->mb_w + d->mb_x;
    int q = d->qscale, qp_c;
    /* qscale never changes within a WMV2 picture: a neighbour's is q unless
     * it was skipped. Chroma uses the same value (no chroma table here). */
    if (!d->skip[xy]) {
        qp_c = q;
        lf_v(dy + 8 * ys, ys, qp_c);
        lf_v(dy + 8 * ys + 8, ys, qp_c);
    } else
        qp_c = 0;
    if (d->mb_y) {
        int qp_tt = d->skip[xy - d->mb_w] ? 0 : q, qp_tc = qp_c ? qp_c : qp_tt, qp_dt;
        if (qp_tc) {
            lf_v(dy, ys, qp_tc);
            lf_v(dy + 8, ys, qp_tc);
            lf_v(du, cs, qp_tc);
            lf_v(dv, cs, qp_tc);
        }
        if (qp_tt)
            lf_h(dy - 8 * ys + 8, ys, qp_tt);
        if (d->mb_x) {
            if (qp_tt || d->skip[xy - 1 - d->mb_w])
                qp_dt = qp_tt;
            else
                qp_dt = q;
            if (qp_dt) {
                lf_h(dy - 8 * ys, ys, qp_dt);
                lf_h(du - 8 * cs, cs, qp_dt);
                lf_h(dv - 8 * cs, cs, qp_dt);
            }
        }
    }
    if (qp_c) {
        lf_h(dy + 8, ys, qp_c);
        if (d->mb_y + 1 == d->mb_h)
            lf_h(dy + 8 * ys + 8, ys, qp_c);
    }
    if (d->mb_x) {
        int qp_lc = (qp_c || d->skip[xy - 1]) ? qp_c : q;
        if (qp_lc) {
            lf_h(dy, ys, qp_lc);
            if (d->mb_y + 1 == d->mb_h) {
                lf_h(dy + 8 * ys, ys, qp_lc);
                lf_h(du, cs, qp_lc);
                lf_h(dv, cs, qp_lc);
            }
        }
    }
}

/* ── Prediction ────────────────────────────────────────────────────────── */

/* Block n of the current MB: its index in the luma or chroma grids. */
static inline int blk_idx(const Wmv2Dec *d, int n)
{
    if (n < 4)
        return yidx(d, 2 * d->mb_x + (n & 1), 2 * d->mb_y + (n >> 1));
    return cidx(d, d->mb_x, d->mb_y);
}

/* DC prediction: returns the predicted (quantised) DC, *dir 0 = from the
 * left, 1 = from the top; *slot gets where to store the block's DC. */
static int pred_dc(Wmv2Dec *d, int n, int16_t **slot, int *dir)
{
    int16_t *g = n < 4 ? d->dc_y : d->dc_c[n - 4];
    int wrap = n < 4 ? d->w8 : d->wm;
    int i = blk_idx(d, n);
    int scale = n < 4 ? d->y_dc_scale : d->c_dc_scale;
    int a = g[i - 1], b = g[i - 1 - wrap], c = g[i - wrap];
    a = (a + (scale >> 1)) / scale;
    b = (b + (scale >> 1)) / scale;
    c = (c + (scale >> 1)) / scale;
    *slot = &g[i];
    if (iabs(a - b) < iabs(b - c)) {
        *dir = 1;
        return c;
    }
    *dir = 0;
    return a;
}

/* AC prediction (raw quantised levels) and the store of this block's first
 * row and column for the blocks after it. */
static void pred_ac(Wmv2Dec *d, int16_t *block, int n, int dir)
{
    int16_t *g = n < 4 ? d->ac_y : d->ac_c[n - 4];
    int wrap = n < 4 ? d->w8 : d->wm;
    int16_t *ac = g + (size_t)blk_idx(d, n) * 16;
    int i;
    if (d->ac_pred) {
        if (dir == 0) {
            const int16_t *l = ac - 16;
            for (i = 1; i < 8; i++) block[i << 3] = (int16_t)(block[i << 3] + l[i]);
        } else {
            const int16_t *t = ac - 16 * wrap;
            for (i = 1; i < 8; i++) block[i] = (int16_t)(block[i] + t[i + 8]);
        }
    }
    for (i = 1; i < 8; i++) ac[i] = block[i << 3];
    for (i = 1; i < 8; i++) ac[8 + i] = block[i];
}

static int decode_dc(Wmv2Dec *d, int n, int *dir)
{
    int level = vlc_get(&d->gb, &g_tab.dc[d->dc_table_index][n >= 4]);
    int pred;
    int16_t *slot;
    if (level < 0)
        return -100000;
    if (level == WMV2_DC_MAX) {
        level = (int)br_get(&d->gb, 8);
        if (br_bit(&d->gb)) level = -level;
    } else if (level) {
        if (br_bit(&d->gb)) level = -level;
    }
    pred = pred_dc(d, n, &slot, dir);
    level += pred;
    *slot = (int16_t)(level * (n < 4 ? d->y_dc_scale : d->c_dc_scale));
    return level;
}

/* One run/level code: returns 0 with *run, *level (raw), *last, 1 for the
 * escape code, -1 for an invalid code. */
static inline int rl_code(Wmv2Dec *d, const RlTable *rl, int *run, int *level, int *last)
{
    int s = vlc_get(&d->gb, &rl->vlc);
    if (s < 0)
        return -1;
    if (s == rl->n)
        return 1;
    *run = rl->run[s];
    *level = rl->level[s];
    *last = s >= rl->last;
    return 0;
}

/* Decodes the coefficients of block n into block[] (zeroed by the caller).
 * Intra blocks get raw quantised levels (dequantised later, after AC
 * prediction); inter blocks dequantised levels. scan: the inter or adaptive
 * block transform scan (NULL: the intra scan the prediction picks). */
static int decode_block(Wmv2Dec *d, int16_t *block, int n, int coded, const uint8_t *scan)
{
    const RlTable *rl;
    int i, qmul, qadd, dir = 0;
    if (d->mb_intra) {
        int level = decode_dc(d, n, &dir);
        if (level == -100000)
            return -1;
        if (n < 4) {
            rl = &g_tab.rl[d->rl_table_index];
            if (level > 256 * d->y_dc_scale) return -1;
        } else {
            rl = &g_tab.rl[3 + d->rl_chroma_table_index];
            if (level > 256 * d->c_dc_scale) return -1;
        }
        block[0] = (int16_t)level;
        qmul = 1;
        qadd = 0;
        i = 0;
        if (!coded)
            goto not_coded;
        scan = d->ac_pred ? (dir == 0 ? wmv2_scan + 192 : wmv2_scan + 128) : wmv2_scan + 64;
    } else {
        qmul = d->qscale << 1;
        qadd = (d->qscale - 1) | 1;
        i = -1;
        rl = &g_tab.rl[3 + d->rl_table_index];
        if (!coded) {
            d->last_index[n] = -1;
            return 0;
        }
        if (!scan)
            scan = wmv2_scan;
    }

    for (;;) {
        int run, level, last, r = rl_code(d, rl, &run, &level, &last), val;
        if (r < 0)
            return -1;
        if (r == 0) {
            i += run + 1 + (last ? 192 : 0);
            val = level * qmul + qadd;
            if (br_bit(&d->gb)) val = -val;
        } else if (br_bit(&d->gb)) {
            /* Escape 1: the level is offset by the largest one for its run. */
            if (rl_code(d, rl, &run, &level, &last) != 0)
                return -1;
            i += run + 1 + (last ? 192 : 0);
            val = (level + rl->max_level[last][run]) * qmul + qadd;
            if (br_bit(&d->gb)) val = -val;
        } else if (br_bit(&d->gb)) {
            /* Escape 2: the run is offset by the largest one for its level. */
            if (rl_code(d, rl, &run, &level, &last) != 0)
                return -1;
            i += run + 1 + (last ? 192 : 0) + rl->max_run[last][level] + 1;
            val = level * qmul + qadd;
            if (br_bit(&d->gb)) val = -val;
        } else {
            /* Escape 3: fixed-length run and level, their lengths sent once
             * per picture. */
            int sign;
            last = br_bit(&d->gb);
            if (!d->esc3_level_length) {
                int ll;
                if (d->qscale < 8) {
                    ll = (int)br_get(&d->gb, 3);
                    if (ll == 0)
                        ll = 8 + br_bit(&d->gb);
                } else {
                    ll = 2;
                    while (ll < 8 && br_peek(&d->gb, 1) == 0) {
                        ll++;
                        br_skip(&d->gb, 1);
                    }
                    if (ll < 8)
                        br_skip(&d->gb, 1);
                }
                d->esc3_level_length = ll;
                d->esc3_run_length = (int)br_get(&d->gb, 2) + 3;
            }
            run = (int)br_get(&d->gb, d->esc3_run_length);
            sign = br_bit(&d->gb);
            level = (int)br_get(&d->gb, d->esc3_level_length);
            if (sign) level = -level;
            val = level > 0 ? level * qmul + qadd : level * qmul - qadd;
            i += run + 1 + (last ? 192 : 0);
        }
        if (br_left(&d->gb) < 0)
            return -1;
        if (i > 62) {
            i -= 192;
            if (i & ~63) {
                /* Past the block's end: the coefficient is dropped (as the
                 * reference decoder does when not checking strictly). */
                i = 63;
                break;
            }
            block[scan[i]] = (int16_t)val;
            break;
        }
        block[scan[i]] = (int16_t)val;
    }
not_coded:
    if (d->mb_intra) {
        pred_ac(d, block, n, dir);
        if (d->ac_pred)
            i = 63;
    }
    if (i > 0)
        i = 63;
    d->last_index[n] = i;
    return 0;
}

/* ── Macroblocks ───────────────────────────────────────────────────────── */

static void clean_intra_entries(Wmv2Dec *d)
{
    int i = yidx(d, 2 * d->mb_x, 2 * d->mb_y), c = cidx(d, d->mb_x, d->mb_y);
    d->dc_y[i] = d->dc_y[i + 1] = d->dc_y[i + d->w8] = d->dc_y[i + 1 + d->w8] = 1024;
    memset(d->ac_y + (size_t)i * 16, 0, 32 * sizeof(int16_t));
    memset(d->ac_y + (size_t)(i + d->w8) * 16, 0, 32 * sizeof(int16_t));
    d->coded[i] = d->coded[i + 1] = d->coded[i + d->w8] = d->coded[i + 1 + d->w8] = 0;
    d->dc_c[0][c] = d->dc_c[1][c] = 1024;
    memset(d->ac_c[0] + (size_t)c * 16, 0, 16 * sizeof(int16_t));
    memset(d->ac_c[1] + (size_t)c * 16, 0, 16 * sizeof(int16_t));
}

static void set_mv(Wmv2Dec *d, int mx, int my)
{
    int i = mvidx(d, 2 * d->mb_x, 2 * d->mb_y);
    int16_t *m = d->mv;
    m[2 * i] = m[2 * (i + 1)] = m[2 * (i + d->w8)] = m[2 * (i + 1 + d->w8)] = (int16_t)mx;
    m[2 * i + 1] = m[2 * (i + 1) + 1] = m[2 * (i + d->w8) + 1] = m[2 * (i + 1 + d->w8) + 1] = (int16_t)my;
}

static void pred_motion(Wmv2Dec *d, int *px, int *py)
{
    int i = mvidx(d, 2 * d->mb_x, 2 * d->mb_y), diff, type;
    const int16_t *a = d->mv + 2 * (i - 1);
    const int16_t *b = d->mv + 2 * (i - d->w8);
    const int16_t *c = d->mv + 2 * (i + 2 - d->w8);
    if (d->mb_x && !d->first_slice_line && !d->mspel && d->top_left_mv_flag) {
        int dx = iabs(a[0] - b[0]), dy = iabs(a[1] - b[1]);
        diff = dx > dy ? dx : dy;
    } else
        diff = 0;
    type = diff >= 8 ? br_bit(&d->gb) : 2;
    if (type == 0 || (type == 2 && d->first_slice_line)) {
        *px = a[0]; *py = a[1];
    } else if (type == 1) {
        *px = b[0]; *py = b[1];
    } else {
        *px = mid3(a[0], b[0], c[0]);
        *py = mid3(a[1], b[1], c[1]);
    }
}

static void decode_motion(Wmv2Dec *d, int *mxp, int *myp)
{
    int code = vlc_get(&d->gb, &g_tab.mv[d->mv_table_index]), mx, my;
    if (code == WMV2_MV_ESCAPE || code < 0) {
        mx = (int)br_get(&d->gb, 6);
        my = (int)br_get(&d->gb, 6);
    } else {
        mx = g_tab.mvx[d->mv_table_index][code];
        my = g_tab.mvy[d->mv_table_index][code];
    }
    mx += *mxp - 32;
    my += *myp - 32;
    if (mx <= -64) mx += 64; else if (mx >= 64) mx -= 64;
    if (my <= -64) my += 64; else if (my >= 64) my -= 64;
    *mxp = mx;
    *myp = my;
    d->hshift = (((mx | my) & 1) && d->mspel) ? br_bit(&d->gb) : 0;
}

static int decode_inter_block(Wmv2Dec *d, int n, int coded)
{
    static const int sub_cbp_table[3] = { 2, 3, 1 };
    if (!coded) {
        d->last_index[n] = -1;
        return 0;
    }
    if (d->per_block_abt)
        d->abt_type = br_012(&d->gb);
    d->abt_type_table[n] = d->abt_type;
    if (d->abt_type) {
        const uint8_t *scan = d->abt_type == 1 ? wmv2_scan_abt_a : wmv2_scan_abt_b;
        int sub = sub_cbp_table[br_012(&d->gb)];
        if ((sub & 1) && decode_block(d, d->block[n], n, 1, scan) < 0)
            return -1;
        if ((sub & 2) && decode_block(d, d->block2[n], n, 1, scan) < 0)
            return -1;
        d->last_index[n] = 63;
        return 0;
    }
    return decode_block(d, d->block[n], n, 1, wmv2_scan);
}

static void add_block(Wmv2Dec *d, int n, uint8_t *dst, int stride)
{
    if (d->last_index[n] < 0)
        return;
    switch (d->abt_type_table[n]) {
    case 0:
        idct_add(dst, stride, d->block[n]);
        break;
    case 1:
        idct84_add(dst, stride, d->block[n]);
        idct84_add(dst + 4 * stride, stride, d->block2[n]);
        memset(d->block2[n], 0, sizeof d->block2[n]);
        break;
    default:
        idct48_add(dst, stride, d->block[n]);
        idct48_add(dst + 4, stride, d->block2[n]);
        memset(d->block2[n], 0, sizeof d->block2[n]);
        break;
    }
}

static void dequant_intra(Wmv2Dec *d, int16_t *b, int n)
{
    int i, qmul = d->qscale << 1, qadd = (d->qscale - 1) | 1;
    b[0] = (int16_t)(b[0] * (n < 4 ? d->y_dc_scale : d->c_dc_scale));
    for (i = 1; i < 64; i++) {
        int l = b[i];
        if (l)
            b[i] = (int16_t)(l < 0 ? l * qmul - qadd : l * qmul + qadd);
    }
}

static int decode_mb(Wmv2Dec *d)
{
    int cbp, code, i;
    int ys = d->cw, cs = d->cw / 2;
    uint8_t **out = d->pic[d->cur];
    uint8_t *dy = out[0] + (size_t)d->mb_y * 16 * ys + d->mb_x * 16;
    uint8_t *dst[6];
    int mbxy = d->mb_y * d->mb_w + d->mb_x;

    dst[0] = dy; dst[1] = dy + 8; dst[2] = dy + 8 * ys; dst[3] = dy + 8 * ys + 8;
    dst[4] = out[1] + (size_t)d->mb_y * 8 * cs + d->mb_x * 8;
    dst[5] = out[2] + (size_t)d->mb_y * 8 * cs + d->mb_x * 8;

    if (!d->key) {
        if (d->skip[mbxy]) {
            d->mb_intra = 0;
            d->hshift = 0;
            set_mv(d, 0, 0);
            clean_intra_entries(d);
            motion(d, 0, 0);
            return 0;
        }
        if (br_left(&d->gb) <= 0)
            return -1;
        code = vlc_get(&d->gb, &g_tab.mb_inter[d->cbp_table_index]);
        if (code < 0)
            return -1;
        d->mb_intra = (~code & 0x40) >> 6;
        cbp = code & 0x3f;
    } else {
        d->mb_intra = 1;
        if (br_left(&d->gb) <= 0)
            return -1;
        code = vlc_get(&d->gb, &g_tab.mb_intra);
        if (code < 0)
            return -1;
        cbp = 0;
        for (i = 0; i < 6; i++) {
            int val = (code >> (5 - i)) & 1;
            if (i < 4) {
                int bi = yidx(d, 2 * d->mb_x + (i & 1), 2 * d->mb_y + (i >> 1));
                int a = d->coded[bi - 1], b = d->coded[bi - 1 - d->w8], c = d->coded[bi - d->w8];
                val ^= (b == c) ? a : c;
                d->coded[bi] = (uint8_t)val;
            }
            cbp |= val << (5 - i);
        }
    }

    memset(d->block, 0, sizeof d->block);
    if (!d->mb_intra) {
        int mx, my;
        pred_motion(d, &mx, &my);
        if (cbp) {
            if (d->per_mb_rl_table) {
                d->rl_table_index = br_012(&d->gb);
                d->rl_chroma_table_index = d->rl_table_index;
            }
            if (d->abt_flag && d->per_mb_abt) {
                d->per_block_abt = br_bit(&d->gb);
                if (!d->per_block_abt)
                    d->abt_type = br_012(&d->gb);
            } else
                d->per_block_abt = 0;
        }
        decode_motion(d, &mx, &my);
        for (i = 0; i < 6; i++)
            if (decode_inter_block(d, i, (cbp >> (5 - i)) & 1) < 0)
                return -1;
        set_mv(d, mx, my);
        clean_intra_entries(d);
        motion(d, mx, my);
        for (i = 0; i < 6; i++)
            add_block(d, i, dst[i], i < 4 ? ys : cs);
    } else {
        d->ac_pred = br_bit(&d->gb);
        if (d->per_mb_rl_table && cbp) {
            d->rl_table_index = br_012(&d->gb);
            d->rl_chroma_table_index = d->rl_table_index;
        }
        for (i = 0; i < 6; i++)
            if (decode_block(d, d->block[i], i, (cbp >> (5 - i)) & 1, NULL) < 0)
                return -1;
        set_mv(d, 0, 0);
        for (i = 0; i < 6; i++) {
            dequant_intra(d, d->block[i], i);
            idct_put(dst[i], i < 4 ? ys : cs, d->block[i]);
        }
    }
    return 0;
}

/* ── Pictures ──────────────────────────────────────────────────────────── */

static int parse_mb_skip(Wmv2Dec *d)
{
    int x, y, coded = 0;
    d->skip_type = (int)br_get(&d->gb, 2);
    switch (d->skip_type) {
    case SKIP_NONE:
        memset(d->skip, 0, (size_t)(d->mb_w * d->mb_h));
        break;
    case SKIP_MPEG:
        if (br_left(&d->gb) < (long)d->mb_w * d->mb_h)
            return -1;
        for (y = 0; y < d->mb_h; y++)
            for (x = 0; x < d->mb_w; x++)
                d->skip[y * d->mb_w + x] = (uint8_t)br_bit(&d->gb);
        break;
    case SKIP_ROW:
        for (y = 0; y < d->mb_h; y++) {
            if (br_left(&d->gb) < 1)
                return -1;
            if (br_bit(&d->gb)) {
                for (x = 0; x < d->mb_w; x++) d->skip[y * d->mb_w + x] = 1;
            } else {
                for (x = 0; x < d->mb_w; x++) d->skip[y * d->mb_w + x] = (uint8_t)br_bit(&d->gb);
            }
        }
        break;
    default:
        for (x = 0; x < d->mb_w; x++) {
            if (br_left(&d->gb) < 1)
                return -1;
            if (br_bit(&d->gb)) {
                for (y = 0; y < d->mb_h; y++) d->skip[y * d->mb_w + x] = 1;
            } else {
                for (y = 0; y < d->mb_h; y++) d->skip[y * d->mb_w + x] = (uint8_t)br_bit(&d->gb);
            }
        }
        break;
    }
    for (x = 0; x < d->mb_w * d->mb_h; x++)
        coded += !d->skip[x];
    if (coded > br_left(&d->gb))
        return -1;
    return 0;
}

/* A P frame whose skip map says "every row (or column) skipped". */
static int all_skipped(Wmv2Dec *d)
{
    Bits g = d->gb;
    int type, run;
    if (br_peek(&g, 1) == 0)
        return 0;
    type = (int)br_get(&g, 2);
    run = type == SKIP_COL ? d->mb_w : d->mb_h;
    while (run > 0) {
        int n = run < 25 ? run : 25;
        if (br_get(&g, n) + 1 != (1u << n))
            break;
        run -= n;
    }
    return run == 0;
}

static void load_bits(Wmv2Dec *d, const uint8_t *data, size_t size, unsigned flags)
{
    size_t i, need = size + 8;
    if (need > d->swap_size) {
        uint8_t *n = (uint8_t *)realloc(d->swap, need);
        if (!n) { d->gb.buf = NULL; return; }
        d->swap = n;
        d->swap_size = need;
    }
    if (flags & WMV2_LE_WORDS) {
        for (i = 0; i + 4 <= size; i += 4) {
            d->swap[i] = data[i + 3];
            d->swap[i + 1] = data[i + 2];
            d->swap[i + 2] = data[i + 1];
            d->swap[i + 3] = data[i];
        }
        for (; i < size; i++)
            d->swap[i] = 0;                 /* a partial last word: not sent */
    } else
        memcpy(d->swap, data, size);
    memset(d->swap + size, 0, 8);
    d->gb.buf = d->swap;
    d->gb.bits = size * 8;
    d->gb.pos = 0;
}

/* After damaged data at the current MB: the rest of the picture is the
 * reference's (the MB in error included). */
static void conceal(Wmv2Dec *d)
{
    uint8_t **out = d->pic[d->cur], **ref = d->pic[d->cur ^ 1];
    int ys = d->cw, cs = d->cw / 2, x0 = d->mb_x, x, y, r;
    for (y = d->mb_y; y < d->mb_h; y++, x0 = 0) {
        for (x = x0; x < d->mb_w; x++) {
            for (r = 0; r < 16; r++) {
                size_t o = (size_t)(y * 16 + r) * ys + x * 16;
                memcpy(out[0] + o, ref[0] + o, 16);
            }
            for (r = 0; r < 8; r++) {
                size_t o = (size_t)(y * 8 + r) * cs + x * 8;
                memcpy(out[1] + o, ref[1] + o, 8);
                memcpy(out[2] + o, ref[2] + o, 8);
            }
        }
    }
}

int wmv2_dec_decode(Wmv2Dec *d, const uint8_t *data, size_t size, unsigned flags)
{
    int key, q, ret = WMV2_OK;
    if (!d || !data || size == 0 || size > (1u << 26))
        return WMV2_ERROR;
    load_bits(d, data, size, flags);
    if (!d->gb.buf)
        return WMV2_ERROR;

    /* Picture header. */
    key = !br_bit(&d->gb);
    if (key)
        br_skip(&d->gb, 7);
    q = (int)br_get(&d->gb, 5);
    if (q <= 0)
        return WMV2_ERROR;
    if (!key && all_skipped(d))
        return WMV2_SKIPPED;
    if (!key && !d->have_ref)
        return WMV2_ERROR;

    if (key) {
        int j_type = d->j_type_bit ? br_bit(&d->gb) : 0;
        if (j_type)
            return WMV2_UNSUPPORTED;
        d->per_mb_rl_table = d->per_mb_rl_bit ? br_bit(&d->gb) : 0;
        if (!d->per_mb_rl_table) {
            d->rl_chroma_table_index = br_012(&d->gb);
            d->rl_table_index = br_012(&d->gb);
        }
        d->dc_table_index = br_bit(&d->gb);
        if (br_left(&d->gb) * 8 < (long)d->mb_w * d->mb_h)
            return WMV2_ERROR;
    }

    /* From here on the picture is written: the new one goes into the other
     * buffer, the current one becomes the reference. */
    d->key = key;
    d->qscale = q;
    d->y_dc_scale = wmv2_y_dc_scale[q];
    d->c_dc_scale = wmv2_c_dc_scale[q];
    d->cur ^= 1;

    if (key) {
        memset(d->skip, 0, (size_t)(d->mb_w * d->mb_h));
        d->no_rounding = 1;
    } else {
        static const uint8_t cbp_map[3][3] = { { 0, 2, 1 }, { 1, 0, 2 }, { 2, 1, 0 } };
        if (parse_mb_skip(d) < 0) { ret = WMV2_ERROR; goto keep_ref; }
        d->cbp_table_index = cbp_map[(q > 10) + (q > 20)][br_012(&d->gb)];
        d->mspel = d->mspel_bit ? br_bit(&d->gb) : 0;
        if (d->abt_flag) {
            d->per_mb_abt = br_bit(&d->gb) ^ 1;
            if (!d->per_mb_abt)
                d->abt_type = br_012(&d->gb);
        }
        d->per_mb_rl_table = d->per_mb_rl_bit ? br_bit(&d->gb) : 0;
        if (!d->per_mb_rl_table) {
            d->rl_table_index = br_012(&d->gb);
            d->rl_chroma_table_index = d->rl_table_index;
        }
        if (br_left(&d->gb) < 2) { ret = WMV2_ERROR; goto keep_ref; }
        d->dc_table_index = br_bit(&d->gb);
        d->mv_table_index = br_bit(&d->gb);
        d->no_rounding ^= 1;
    }
    d->esc3_level_length = 0;
    d->esc3_run_length = 0;
    memset(d->abt_type_table, 0, sizeof d->abt_type_table);
    memset(d->block2, 0, sizeof d->block2);

    /* Slices: rows in groups of slice_height; only the MV prediction sees
     * their boundaries. */
    for (d->mb_y = 0; d->mb_y < d->mb_h; d->mb_y++) {
        int slice_top = d->slice_height > 0 ? (d->mb_y / d->slice_height) * d->slice_height : 0;
        for (d->mb_x = 0; d->mb_x < d->mb_w; d->mb_x++) {
            d->first_slice_line = d->mb_y == slice_top;
            if (decode_mb(d) < 0) {
                ret = WMV2_ERROR;
                conceal(d);
                goto done;
            }
            if (d->loop_filter)
                loop_filter(d);
        }
    }
done:
    d->have_ref = 1;
    return ret;
keep_ref:
    /* Nothing of the new picture was decoded: show the reference again. */
    d->cur ^= 1;
    return ret;
}
