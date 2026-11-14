/* GTE, the geometry transformation engine (COP2), P-6 XBA-164. Written from nocash's psx-spx description of the
 * hardware: every command, the 44-bit MAC accumulators checked after each addition, the saturation rules and FLAG
 * bits, the UNR reciprocal used by RTPS / RTPT, and the register quirks (sign extension on read, H read as signed,
 * IRGB / ORGB, LZCS / LZCR, the SXY FIFO through SXYP, FLAG bit 31).
 *
 * Command word (COP2 imm25): bits 0-5 opcode, 10 lm (IR saturates at 0 instead of -8000h), 13-14 MVMVA translation
 * vector, 15-16 MVMVA vector, 17-18 MVMVA matrix, 19 sf (results shifted right by 12). */
#include "ps1_internal.h"

#include <string.h>

/* FLAG bits. */
#define F_MAC1P (1u << 30)
#define F_MAC1N (1u << 27)
#define F_IR1 (1u << 24)
#define F_COLR (1u << 21)
#define F_SZ (1u << 18)
#define F_DIV (1u << 17)
#define F_MAC0P (1u << 16)
#define F_MAC0N (1u << 15)
#define F_SX (1u << 14)
#define F_SY (1u << 13)
#define F_IR0 (1u << 12)
#define F_ERRORS 0x7F87E000u /* bits 30-23 and 18-13 feed bit 31 */

/* The UNR table entry i (0..256): max(0, (40000h / (i + 100h) + 1) / 2 - 101h). Computed, not stored. */
static inline uint32_t unr(uint32_t i)
{
    int v = (int)((0x40000u / (i + 0x100u) + 1) / 2) - 0x101;
    return v < 0 ? 0u : (uint32_t)v;
}

void ps1_gte_reset(ps1_gte *g)
{
    memset(g, 0, sizeof *g);
}

/* ---- Accumulator helpers ---- */

/* Checks a MAC1..3 intermediate against 44 bits (flags) and wraps it to 44 bits. */
static inline int64_t chk44(ps1_gte *g, int i, int64_t v)
{
    if (v > 0x7FFFFFFFFFFLL)
        g->flag |= F_MAC1P >> (i - 1);
    else if (v < -0x80000000000LL)
        g->flag |= F_MAC1N >> (i - 1);
    return (int64_t)((uint64_t)v << 20) >> 20;
}

/* MACi = value >> shift (after the 44-bit check). Returns the stored MAC. */
static inline int32_t set_mac(ps1_gte *g, int i, int64_t v, int shift)
{
    chk44(g, i, v);
    g->mac[i] = (int32_t)(v >> shift);
    return g->mac[i];
}

static inline int64_t chk_mac0(ps1_gte *g, int64_t v)
{
    if (v > 0x7FFFFFFFLL)
        g->flag |= F_MAC0P;
    else if (v < -0x80000000LL)
        g->flag |= F_MAC0N;
    return v;
}

static inline void set_mac0(ps1_gte *g, int64_t v)
{
    chk_mac0(g, v);
    g->mac[0] = (int32_t)v;
}

/* IRi = saturate(v) to -8000h (0 with lm) .. 7FFFh. */
static inline void set_ir(ps1_gte *g, int i, int32_t v, int lm)
{
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) {
        v = lo;
        g->flag |= F_IR1 >> (i - 1);
    } else if (v > 0x7FFF) {
        v = 0x7FFF;
        g->flag |= F_IR1 >> (i - 1);
    }
    g->ir[i] = (int16_t)v;
}

static inline void set_mac_ir(ps1_gte *g, int i, int64_t v, int shift, int lm)
{
    set_ir(g, i, set_mac(g, i, v, shift), lm);
}

static inline uint8_t sat_color(ps1_gte *g, int i, int32_t v)
{
    if (v < 0) {
        g->flag |= F_COLR >> i;
        return 0;
    }
    if (v > 0xFF) {
        g->flag |= F_COLR >> i;
        return 0xFF;
    }
    return (uint8_t)v;
}

/* Color FIFO push of [MAC1, MAC2, MAC3] / 16 with RGBC's code byte. */
static void push_rgb(ps1_gte *g)
{
    memcpy(g->rgb_fifo[0], g->rgb_fifo[1], 4);
    memcpy(g->rgb_fifo[1], g->rgb_fifo[2], 4);
    g->rgb_fifo[2][0] = sat_color(g, 0, g->mac[1] >> 4);
    g->rgb_fifo[2][1] = sat_color(g, 1, g->mac[2] >> 4);
    g->rgb_fifo[2][2] = sat_color(g, 2, g->mac[3] >> 4);
    g->rgb_fifo[2][3] = g->rgbc[3];
}

static void push_sz(ps1_gte *g, int64_t v)
{
    if (v < 0) {
        v = 0;
        g->flag |= F_SZ;
    } else if (v > 0xFFFF) {
        v = 0xFFFF;
        g->flag |= F_SZ;
    }
    g->sz[0] = g->sz[1];
    g->sz[1] = g->sz[2];
    g->sz[2] = g->sz[3];
    g->sz[3] = (uint16_t)v;
}

static void push_sxy(ps1_gte *g, int32_t x, int32_t y)
{
    if (x < -0x400) {
        x = -0x400;
        g->flag |= F_SX;
    } else if (x > 0x3FF) {
        x = 0x3FF;
        g->flag |= F_SX;
    }
    if (y < -0x400) {
        y = -0x400;
        g->flag |= F_SY;
    } else if (y > 0x3FF) {
        y = 0x3FF;
        g->flag |= F_SY;
    }
    memcpy(g->sxy[0], g->sxy[1], sizeof g->sxy[0]);
    memcpy(g->sxy[1], g->sxy[2], sizeof g->sxy[0]);
    g->sxy[2][0] = (int16_t)x;
    g->sxy[2][1] = (int16_t)y;
}

static inline void set_ir0(ps1_gte *g, int64_t v)
{
    if (v < 0) {
        v = 0;
        g->flag |= F_IR0;
    } else if (v > 0x1000) {
        v = 0x1000;
        g->flag |= F_IR0;
    }
    g->ir[0] = (int16_t)v;
}

/* The UNR reciprocal: (H * 20000h / SZ3 + 1) / 2, clamped to 1FFFFh (overflow flag when H >= SZ3 * 2). */
static uint32_t gte_divide(ps1_gte *g)
{
    uint32_t h = g->h, sz3 = g->sz[3];
    if (h < sz3 * 2) {
        int z = 0;
        while (z < 16 && !((sz3 << z) & 0x8000))
            z++;
        uint64_t n = (uint64_t)h << z;
        uint32_t d = sz3 << z;
        uint32_t u = unr((d - 0x7FC0) >> 7) + 0x101;
        d = (0x2000080u - d * u) >> 8;
        d = (0x0000080u + d * u) >> 8;
        uint64_t r = (n * d + 0x8000) >> 16;
        return r > 0x1FFFF ? 0x1FFFF : (uint32_t)r;
    }
    g->flag |= F_DIV;
    return 0x1FFFF;
}

/* ---- Matrix / vector products ---- */

/* MAC / IR = (T << 12 + M * V) >> shift; T may be NULL (zero). */
static void mat_vec(ps1_gte *g, const int16_t m[3][3], const int16_t v[3], const int32_t *t, int shift, int lm)
{
    for (int i = 0; i < 3; i++) {
        int64_t a = t ? (int64_t)t[i] << 12 : 0;
        a = chk44(g, i + 1, a + (int64_t)m[i][0] * v[0]);
        a = chk44(g, i + 1, a + (int64_t)m[i][1] * v[1]);
        a = a + (int64_t)m[i][2] * v[2];
        set_mac_ir(g, i + 1, a, shift, lm);
    }
}

static void rtp(ps1_gte *g, int n, int shift, int lm, int last)
{
    const int16_t *v = g->v[n];
    int64_t mac3 = 0;
    for (int i = 0; i < 3; i++) {
        int64_t a = (int64_t)g->tr[i] << 12;
        a = chk44(g, i + 1, a + (int64_t)g->rt[i][0] * v[0]);
        a = chk44(g, i + 1, a + (int64_t)g->rt[i][1] * v[1]);
        a = a + (int64_t)g->rt[i][2] * v[2];
        int32_t mac = set_mac(g, i + 1, a, shift);
        if (i < 2) {
            set_ir(g, i + 1, mac, lm);
        } else {
            mac3 = chk44(g, 3, a);
            /* IR3 saturates on MAC3, but its flag comes from MAC3 >> 12 whatever sf is. */
            int32_t lo = lm ? 0 : -0x8000, s = mac;
            int64_t z = mac3 >> 12;
            if (z < -0x8000 || z > 0x7FFF)
                g->flag |= F_IR1 >> 2;
            if (s < lo)
                s = lo;
            else if (s > 0x7FFF)
                s = 0x7FFF;
            g->ir[3] = (int16_t)s;
        }
    }
    push_sz(g, mac3 >> 12); /* from the full accumulator, not the 32-bit MAC3 */
    int64_t div = gte_divide(g);
    int64_t x = chk_mac0(g, div * g->ir[1] + g->ofx);
    int64_t y = chk_mac0(g, div * g->ir[2] + g->ofy);
    push_sxy(g, (int32_t)(x >> 16), (int32_t)(y >> 16));
    if (last) {
        int64_t dq = div * g->dqa + g->dqb;
        set_mac0(g, dq);
        set_ir0(g, dq >> 12);
    }
}

/* [IR] = ((FC << 12) - inMAC) >> sf (saturated, lm = 0); [MAC] = (IR * IR0 + inMAC) >> sf; color FIFO; [IR] (lm). */
static void interpolate(ps1_gte *g, int64_t m1, int64_t m2, int64_t m3, int shift, int lm)
{
    int64_t in[3] = {m1, m2, m3};
    for (int i = 0; i < 3; i++)
        set_mac_ir(g, i + 1, ((int64_t)g->fc[i] << 12) - in[i], shift, 0);
    for (int i = 0; i < 3; i++)
        set_mac(g, i + 1, (int64_t)g->ir[i + 1] * g->ir[0] + in[i], shift);
    push_rgb(g);
    for (int i = 0; i < 3; i++)
        set_ir(g, i + 1, g->mac[i + 1], lm);
}

/* Light: [IR] = LLM * V >> sf; [IR] = (BK << 12 + LCM * IR) >> sf. */
static void light(ps1_gte *g, int n, int shift, int lm)
{
    mat_vec(g, g->llm, g->v[n], NULL, shift, lm);
    int16_t ir[3] = {g->ir[1], g->ir[2], g->ir[3]};
    mat_vec(g, g->lcm, ir, g->bk, shift, lm);
}

static void color_mul(ps1_gte *g, const uint8_t rgb[3], int64_t out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = ((int64_t)rgb[i] * g->ir[i + 1]) << 4;
}

static void ncd(ps1_gte *g, int n, int shift, int lm)
{
    int64_t c[3];
    light(g, n, shift, lm);
    color_mul(g, g->rgbc, c);
    interpolate(g, c[0], c[1], c[2], shift, lm);
}

static void ncc(ps1_gte *g, int n, int shift, int lm)
{
    int64_t c[3];
    light(g, n, shift, lm);
    color_mul(g, g->rgbc, c);
    for (int i = 0; i < 3; i++)
        set_mac(g, i + 1, c[i], shift);
    push_rgb(g);
    for (int i = 0; i < 3; i++)
        set_ir(g, i + 1, g->mac[i + 1], lm);
}

static void nc(ps1_gte *g, int n, int shift, int lm)
{
    light(g, n, shift, lm);
    push_rgb(g);
}

static void mvmva(ps1_gte *g, uint32_t cmd, int shift, int lm)
{
    int mx = (int)((cmd >> 17) & 3), vx = (int)((cmd >> 15) & 3), tx = (int)((cmd >> 13) & 3);
    int16_t m[3][3], v[3];
    int32_t zero[3] = {0, 0, 0};
    const int32_t *t;
    switch (mx) {
    case 0: memcpy(m, g->rt, sizeof m); break;
    case 1: memcpy(m, g->llm, sizeof m); break;
    case 2: memcpy(m, g->lcm, sizeof m); break;
    default: /* garbage matrix */
        m[0][0] = (int16_t)-(g->rgbc[0] << 4);
        m[0][1] = (int16_t)(g->rgbc[0] << 4);
        m[0][2] = g->ir[0];
        m[1][0] = m[1][1] = m[1][2] = g->rt[0][2];
        m[2][0] = m[2][1] = m[2][2] = g->rt[1][1];
        break;
    }
    if (vx < 3)
        memcpy(v, g->v[vx], sizeof v);
    else
        memcpy(v, &g->ir[1], sizeof v);
    switch (tx) {
    case 0: t = g->tr; break;
    case 1: t = g->bk; break;
    case 2: t = g->fc; break;
    default: t = zero; break;
    }
    if (tx != 2) {
        mat_vec(g, (const int16_t(*)[3])m, v, t, shift, lm);
        return;
    }
    /* FC as the translation is bugged: the first column's sum only sets flags, the result is the other two. */
    for (int i = 0; i < 3; i++) {
        int64_t a = chk44(g, i + 1, ((int64_t)t[i] << 12) + (int64_t)m[i][0] * v[0]);
        int32_t part = (int32_t)(a >> shift);
        if (part < -0x8000 || part > 0x7FFF)
            g->flag |= F_IR1 >> i;
        int64_t b = chk44(g, i + 1, (int64_t)m[i][1] * v[1]);
        b = b + (int64_t)m[i][2] * v[2];
        set_mac_ir(g, i + 1, b, shift, lm);
    }
}

int ps1_gte_command(ps1_gte *g, uint32_t cmd)
{
    int shift = (cmd & (1u << 19)) ? 12 : 0;
    int lm = (cmd >> 10) & 1;
    int cost;
    g->flag = 0;
    switch (cmd & 0x3F) {
    case 0x01: /* RTPS */
        rtp(g, 0, shift, lm, 1);
        cost = 15;
        break;
    case 0x30: /* RTPT */
        rtp(g, 0, shift, lm, 0);
        rtp(g, 1, shift, lm, 0);
        rtp(g, 2, shift, lm, 1);
        cost = 23;
        break;
    case 0x06: { /* NCLIP */
        int64_t x0 = g->sxy[0][0], y0 = g->sxy[0][1], x1 = g->sxy[1][0], y1 = g->sxy[1][1];
        int64_t x2 = g->sxy[2][0], y2 = g->sxy[2][1];
        set_mac0(g, x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
        cost = 8;
        break;
    }
    case 0x0C: { /* OP: cross product of the rotation diagonal and IR */
        int64_t d1 = g->rt[0][0], d2 = g->rt[1][1], d3 = g->rt[2][2];
        int64_t i1 = g->ir[1], i2 = g->ir[2], i3 = g->ir[3];
        set_mac(g, 1, i3 * d2 - i2 * d3, shift);
        set_mac(g, 2, i1 * d3 - i3 * d1, shift);
        set_mac(g, 3, i2 * d1 - i1 * d2, shift);
        for (int i = 1; i <= 3; i++)
            set_ir(g, i, g->mac[i], lm);
        cost = 6;
        break;
    }
    case 0x10: /* DPCS */
        interpolate(g, (int64_t)g->rgbc[0] << 16, (int64_t)g->rgbc[1] << 16, (int64_t)g->rgbc[2] << 16, shift, lm);
        cost = 8;
        break;
    case 0x11: /* INTPL */
        interpolate(g, (int64_t)g->ir[1] << 12, (int64_t)g->ir[2] << 12, (int64_t)g->ir[3] << 12, shift, lm);
        cost = 8;
        break;
    case 0x12: /* MVMVA */
        mvmva(g, cmd, shift, lm);
        cost = 8;
        break;
    case 0x13: /* NCDS */
        ncd(g, 0, shift, lm);
        cost = 19;
        break;
    case 0x14: { /* CDP */
        int64_t c[3];
        int16_t ir[3] = {g->ir[1], g->ir[2], g->ir[3]};
        mat_vec(g, g->lcm, ir, g->bk, shift, lm);
        color_mul(g, g->rgbc, c);
        interpolate(g, c[0], c[1], c[2], shift, lm);
        cost = 13;
        break;
    }
    case 0x16: /* NCDT */
        ncd(g, 0, shift, lm);
        ncd(g, 1, shift, lm);
        ncd(g, 2, shift, lm);
        cost = 44;
        break;
    case 0x1B: /* NCCS */
        ncc(g, 0, shift, lm);
        cost = 17;
        break;
    case 0x1C: { /* CC */
        int64_t c[3];
        int16_t ir[3] = {g->ir[1], g->ir[2], g->ir[3]};
        mat_vec(g, g->lcm, ir, g->bk, shift, lm);
        color_mul(g, g->rgbc, c);
        for (int i = 0; i < 3; i++)
            set_mac(g, i + 1, c[i], shift);
        push_rgb(g);
        for (int i = 0; i < 3; i++)
            set_ir(g, i + 1, g->mac[i + 1], lm);
        cost = 11;
        break;
    }
    case 0x1E: /* NCS */
        nc(g, 0, shift, lm);
        cost = 14;
        break;
    case 0x20: /* NCT */
        nc(g, 0, shift, lm);
        nc(g, 1, shift, lm);
        nc(g, 2, shift, lm);
        cost = 30;
        break;
    case 0x28: /* SQR */
        for (int i = 1; i <= 3; i++)
            set_mac_ir(g, i, (int64_t)g->ir[i] * g->ir[i], shift, lm);
        cost = 5;
        break;
    case 0x29: { /* DCPL */
        int64_t c[3];
        color_mul(g, g->rgbc, c);
        interpolate(g, c[0], c[1], c[2], shift, lm);
        cost = 8;
        break;
    }
    case 0x2A: /* DPCT: DPCS three times on the color FIFO's oldest entry */
        for (int k = 0; k < 3; k++) {
            const uint8_t *c = g->rgb_fifo[0];
            interpolate(g, (int64_t)c[0] << 16, (int64_t)c[1] << 16, (int64_t)c[2] << 16, shift, lm);
        }
        cost = 17;
        break;
    case 0x2D: { /* AVSZ3 */
        int64_t v = (int64_t)g->zsf3 * ((int64_t)g->sz[1] + g->sz[2] + g->sz[3]);
        set_mac0(g, v);
        int64_t o = v >> 12;
        if (o < 0) {
            o = 0;
            g->flag |= F_SZ;
        } else if (o > 0xFFFF) {
            o = 0xFFFF;
            g->flag |= F_SZ;
        }
        g->otz = (uint16_t)o;
        cost = 5;
        break;
    }
    case 0x2E: { /* AVSZ4 */
        int64_t v = (int64_t)g->zsf4 * ((int64_t)g->sz[0] + g->sz[1] + g->sz[2] + g->sz[3]);
        set_mac0(g, v);
        int64_t o = v >> 12;
        if (o < 0) {
            o = 0;
            g->flag |= F_SZ;
        } else if (o > 0xFFFF) {
            o = 0xFFFF;
            g->flag |= F_SZ;
        }
        g->otz = (uint16_t)o;
        cost = 6;
        break;
    }
    case 0x3D: /* GPF */
        for (int i = 1; i <= 3; i++)
            set_mac(g, i, (int64_t)g->ir[0] * g->ir[i], shift);
        push_rgb(g);
        for (int i = 1; i <= 3; i++)
            set_ir(g, i, g->mac[i], lm);
        cost = 5;
        break;
    case 0x3E: /* GPL */
        for (int i = 1; i <= 3; i++)
            set_mac(g, i, ((int64_t)g->mac[i] << shift) + (int64_t)g->ir[0] * g->ir[i], shift);
        push_rgb(g);
        for (int i = 1; i <= 3; i++)
            set_ir(g, i, g->mac[i], lm);
        cost = 5;
        break;
    case 0x3F: /* NCCT */
        ncc(g, 0, shift, lm);
        ncc(g, 1, shift, lm);
        ncc(g, 2, shift, lm);
        cost = 39;
        break;
    default:
        cost = 2;
        break;
    }
    if (g->flag & F_ERRORS)
        g->flag |= 0x80000000u;
    return cost;
}

/* ---- Register transfers ---- */

static inline uint32_t pack16(int16_t lo, int16_t hi)
{
    return (uint32_t)(uint16_t)lo | (uint32_t)(uint16_t)hi << 16;
}

static inline uint8_t orgb_comp(int16_t ir)
{
    int v = ir >> 7;
    return (uint8_t)(v < 0 ? 0 : v > 0x1F ? 0x1F : v);
}

uint32_t ps1_gte_read_data(ps1_gte *g, int r)
{
    switch (r & 31) {
    case 0: return pack16(g->v[0][0], g->v[0][1]);
    case 1: return (uint32_t)(int32_t)g->v[0][2];
    case 2: return pack16(g->v[1][0], g->v[1][1]);
    case 3: return (uint32_t)(int32_t)g->v[1][2];
    case 4: return pack16(g->v[2][0], g->v[2][1]);
    case 5: return (uint32_t)(int32_t)g->v[2][2];
    case 6: return (uint32_t)g->rgbc[0] | (uint32_t)g->rgbc[1] << 8 | (uint32_t)g->rgbc[2] << 16 |
                   (uint32_t)g->rgbc[3] << 24;
    case 7: return g->otz;
    case 8: case 9: case 10: case 11: return (uint32_t)(int32_t)g->ir[r - 8];
    case 12: case 13: case 14: return pack16(g->sxy[r - 12][0], g->sxy[r - 12][1]);
    case 15: return pack16(g->sxy[2][0], g->sxy[2][1]);
    case 16: case 17: case 18: case 19: return g->sz[r - 16];
    case 20: case 21: case 22: {
        const uint8_t *c = g->rgb_fifo[r - 20];
        return (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24;
    }
    case 23: return g->res1;
    case 24: case 25: case 26: case 27: return (uint32_t)g->mac[r - 24];
    case 28: case 29:
        return (uint32_t)orgb_comp(g->ir[1]) | (uint32_t)orgb_comp(g->ir[2]) << 5 |
               (uint32_t)orgb_comp(g->ir[3]) << 10;
    case 30: return (uint32_t)g->lzcs;
    default: return (uint32_t)g->lzcr;
    }
}

void ps1_gte_write_data(ps1_gte *g, int r, uint32_t v)
{
    int16_t lo = (int16_t)v, hi = (int16_t)(v >> 16);
    switch (r & 31) {
    case 0: case 2: case 4:
        g->v[r / 2][0] = lo;
        g->v[r / 2][1] = hi;
        break;
    case 1: case 3: case 5: g->v[r / 2][2] = lo; break;
    case 6:
        g->rgbc[0] = (uint8_t)v;
        g->rgbc[1] = (uint8_t)(v >> 8);
        g->rgbc[2] = (uint8_t)(v >> 16);
        g->rgbc[3] = (uint8_t)(v >> 24);
        break;
    case 7: g->otz = (uint16_t)v; break;
    case 8: case 9: case 10: case 11: g->ir[r - 8] = lo; break;
    case 12: case 13: case 14:
        g->sxy[r - 12][0] = lo;
        g->sxy[r - 12][1] = hi;
        break;
    case 15: /* SXYP: pushes the FIFO */
        memcpy(g->sxy[0], g->sxy[1], sizeof g->sxy[0]);
        memcpy(g->sxy[1], g->sxy[2], sizeof g->sxy[0]);
        g->sxy[2][0] = lo;
        g->sxy[2][1] = hi;
        break;
    case 16: case 17: case 18: case 19: g->sz[r - 16] = (uint16_t)v; break;
    case 20: case 21: case 22: {
        uint8_t *c = g->rgb_fifo[r - 20];
        c[0] = (uint8_t)v;
        c[1] = (uint8_t)(v >> 8);
        c[2] = (uint8_t)(v >> 16);
        c[3] = (uint8_t)(v >> 24);
        break;
    }
    case 23: g->res1 = v; break;
    case 24: case 25: case 26: case 27: g->mac[r - 24] = (int32_t)v; break;
    case 28: /* IRGB: 5-bit components x 80h into IR1..IR3 */
        g->irgb = (uint16_t)(v & 0x7FFF);
        g->ir[1] = (int16_t)((v & 0x1F) << 7);
        g->ir[2] = (int16_t)(((v >> 5) & 0x1F) << 7);
        g->ir[3] = (int16_t)(((v >> 10) & 0x1F) << 7);
        break;
    case 29: break; /* ORGB: read only */
    case 30: {
        g->lzcs = (int32_t)v;
        uint32_t x = (int32_t)v < 0 ? ~v : v;
        g->lzcr = x ? __builtin_clz(x) : 32;
        break;
    }
    default: break; /* LZCR: read only */
    }
}

/* Matrices: 5 words each, the ninth element alone (sign-extended) in the fifth. */
static uint32_t mat_read(const int16_t m[3][3], int w)
{
    const int16_t *e = &m[0][0];
    return w == 4 ? (uint32_t)(int32_t)e[8] : pack16(e[w * 2], e[w * 2 + 1]);
}

static void mat_write(int16_t m[3][3], int w, uint32_t v)
{
    int16_t *e = &m[0][0];
    if (w == 4) {
        e[8] = (int16_t)v;
    } else {
        e[w * 2] = (int16_t)v;
        e[w * 2 + 1] = (int16_t)(v >> 16);
    }
}

uint32_t ps1_gte_read_ctrl(ps1_gte *g, int r)
{
    r &= 31;
    if (r < 5)
        return mat_read(g->rt, r);
    if (r < 8)
        return (uint32_t)g->tr[r - 5];
    if (r < 13)
        return mat_read(g->llm, r - 8);
    if (r < 16)
        return (uint32_t)g->bk[r - 13];
    if (r < 21)
        return mat_read(g->lcm, r - 16);
    if (r < 24)
        return (uint32_t)g->fc[r - 21];
    switch (r) {
    case 24: return (uint32_t)g->ofx;
    case 25: return (uint32_t)g->ofy;
    case 26: return (uint32_t)(int32_t)(int16_t)g->h; /* H reads back sign-extended */
    case 27: return (uint32_t)(int32_t)g->dqa;
    case 28: return (uint32_t)g->dqb;
    case 29: return (uint32_t)(int32_t)g->zsf3;
    case 30: return (uint32_t)(int32_t)g->zsf4;
    default: return g->flag;
    }
}

void ps1_gte_write_ctrl(ps1_gte *g, int r, uint32_t v)
{
    r &= 31;
    if (r < 5)
        mat_write(g->rt, r, v);
    else if (r < 8)
        g->tr[r - 5] = (int32_t)v;
    else if (r < 13)
        mat_write(g->llm, r - 8, v);
    else if (r < 16)
        g->bk[r - 13] = (int32_t)v;
    else if (r < 21)
        mat_write(g->lcm, r - 16, v);
    else if (r < 24)
        g->fc[r - 21] = (int32_t)v;
    else {
        switch (r) {
        case 24: g->ofx = (int32_t)v; break;
        case 25: g->ofy = (int32_t)v; break;
        case 26: g->h = (uint16_t)v; break;
        case 27: g->dqa = (int16_t)v; break;
        case 28: g->dqb = (int32_t)v; break;
        case 29: g->zsf3 = (int16_t)v; break;
        case 30: g->zsf4 = (int16_t)v; break;
        default:
            g->flag = v & 0x7FFFF000u;
            if (g->flag & F_ERRORS)
                g->flag |= 0x80000000u;
            break;
        }
    }
}
