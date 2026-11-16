/* sh4_ops.h -- SH-4 state and instruction semantics shared by the interpreter (sh4_interp.c) and lifted code
 * (consoles/dc/tools dc_tools.lift) (XBA-227, Dreamcast spike, epic XBA-221). Part of DC_RECOMP_ABI (dc_recomp.h):
 * change it together with the lifter and bump DC_RECOMP_ABI.
 *
 * One function per instruction of sh4_optab.h that is not a branch: `sh4i_NAME(c, pc, op)`, pc the instruction's
 * own address (PC-relative loads), op its 16-bit opcode. Lifted code calls them with constants, so the compiler
 * folds the field extraction away. Branches (BT / BF / BRA / BSR / JMP / JSR / RTS / RTE / BRAF / BSRF, with or
 * without delay slot) are the interpreter's and the lifter's business; the helpers they share are at the end.
 *
 * Model: little-endian SH7091 (SH-4) as the Dreamcast runs it: privileged mode, MMU off, caches transparent (OCBI /
 * OCBP / OCBWB / PREF on ordinary memory do nothing), the store queues behind PREF (dc_bus.c). One instruction per
 * cycle. FPU: FPSCR.PR (double precision), SZ (64-bit FMOV pairs), FR (bank swap) honoured; rounding mode, the
 * cause / flag bits and FPU exceptions are not modelled (host IEEE round to nearest, denormals as the host does).
 * The 64-bit pair moves transfer FR(n) at the lower address and FR(n+1) at +4; a double DRn is FR(n) high word,
 * FR(n+1) low word. */
#ifndef SH4_OPS_H
#define SH4_OPS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the Dreamcast runtime assumes a little-endian host (the SH-4 runs little-endian)"
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SH4_SR_T = 1u << 0,
    SH4_SR_S = 1u << 1,
    SH4_SR_IMASK = 0xFu << 4,
    SH4_SR_Q = 1u << 8,
    SH4_SR_M = 1u << 9,
    SH4_SR_FD = 1u << 15,
    SH4_SR_BL = 1u << 28,
    SH4_SR_RB = 1u << 29,
    SH4_SR_MD = 1u << 30,
    SH4_SR_MASK = 0x700083F3u,
    SH4_FPSCR_PR = 1u << 19,
    SH4_FPSCR_SZ = 1u << 20,
    SH4_FPSCR_FR = 1u << 21,
    SH4_FPSCR_MASK = 0x003FFFFFu,
};

typedef struct sh4_cpu {
    uint32_t r[16];
    uint32_t rbank[8]; /* R0-R7 of the bank SR.MD && SR.RB does not select */
    uint32_t sr;       /* SR without T, S, Q, M (they live below) */
    uint32_t t, s, q, m;
    uint32_t gbr, vbr, ssr, spc, sgr, dbr;
    uint32_t mach, macl, pr;
    uint32_t pc; /* between dispatches: the next instruction */
    uint32_t fpscr, fpul;
    uint32_t fr[16], xf[16]; /* current bank (FPSCR.FR swaps them), raw bits */
    uint32_t tra, expevt, intevt;
    uint64_t cycles;     /* one per instruction since reset */
    uint64_t next_event; /* lifted code returns to the dispatcher once cycles reaches it */
    uint8_t *ram;        /* main RAM (0x0C000000, mirrored to 0x0FFFFFFF): 16 MB, 32 MB on Naomi */
    uint32_t sleeping;   /* SLEEP until an interrupt */
    uint32_t depth;      /* nested direct calls between lifted functions */
    uint64_t rc_native;  /* instructions lifted code ran (packs built with -DDC_RC_COUNT) */
    uint32_t ram_mask;   /* main RAM size - 1: 0xFFFFFF (Dreamcast), 0x1FFFFFF (Naomi, XBA-235) */
} sh4_cpu;

/* The bus (dc_bus.c), exported by the host program for the pack to bind to. Addresses are virtual (P0-P4); the
 * inline accessors below take main RAM themselves. */
uint8_t dc_read8(sh4_cpu *c, uint32_t a);
uint16_t dc_read16(sh4_cpu *c, uint32_t a);
uint32_t dc_read32(sh4_cpu *c, uint32_t a);
void dc_write8(sh4_cpu *c, uint32_t a, uint8_t v);
void dc_write16(sh4_cpu *c, uint32_t a, uint16_t v);
void dc_write32(sh4_cpu *c, uint32_t a, uint32_t v);
void dc_pref(sh4_cpu *c, uint32_t a); /* store queue flush when a is in 0xE0000000-0xE3FFFFFF */

static inline bool sh4_is_ram(uint32_t a)
{
    return (a >> 29) != 7 && (a & 0x1C000000u) == 0x0C000000u;
}

static inline uint8_t sh4_r8(sh4_cpu *c, uint32_t a)
{
    return sh4_is_ram(a) ? c->ram[a & c->ram_mask] : dc_read8(c, a);
}
static inline uint16_t sh4_r16(sh4_cpu *c, uint32_t a)
{
    if (sh4_is_ram(a)) {
        uint16_t v;
        memcpy(&v, c->ram + (a & (c->ram_mask & ~1u)), 2);
        return v;
    }
    return dc_read16(c, a);
}
static inline uint32_t sh4_r32(sh4_cpu *c, uint32_t a)
{
    if (sh4_is_ram(a)) {
        uint32_t v;
        memcpy(&v, c->ram + (a & (c->ram_mask & ~3u)), 4);
        return v;
    }
    return dc_read32(c, a);
}
static inline void sh4_w8(sh4_cpu *c, uint32_t a, uint8_t v)
{
    if (sh4_is_ram(a))
        c->ram[a & c->ram_mask] = v;
    else
        dc_write8(c, a, v);
}
static inline void sh4_w16(sh4_cpu *c, uint32_t a, uint16_t v)
{
    if (sh4_is_ram(a))
        memcpy(c->ram + (a & (c->ram_mask & ~1u)), &v, 2);
    else
        dc_write16(c, a, v);
}
static inline void sh4_w32(sh4_cpu *c, uint32_t a, uint32_t v)
{
    if (sh4_is_ram(a))
        memcpy(c->ram + (a & (c->ram_mask & ~3u)), &v, 4);
    else
        dc_write32(c, a, v);
}

/* ---- status registers ---- */

static inline uint32_t sh4_get_sr(const sh4_cpu *c)
{
    return c->sr | c->t | (c->s << 1) | (c->q << 8) | (c->m << 9);
}

static inline void sh4_set_sr(sh4_cpu *c, uint32_t v)
{
    v &= SH4_SR_MASK;
    bool oldb = (c->sr & SH4_SR_MD) && (c->sr & SH4_SR_RB);
    bool newb = (v & SH4_SR_MD) && (v & SH4_SR_RB);
    if (oldb != newb) {
        for (int i = 0; i < 8; i++) {
            uint32_t t = c->r[i];
            c->r[i] = c->rbank[i];
            c->rbank[i] = t;
        }
    }
    c->t = v & 1;
    c->s = (v >> 1) & 1;
    c->q = (v >> 8) & 1;
    c->m = (v >> 9) & 1;
    c->sr = v & ~(SH4_SR_T | SH4_SR_S | SH4_SR_Q | SH4_SR_M);
}

static inline void sh4_set_fpscr(sh4_cpu *c, uint32_t v)
{
    v &= SH4_FPSCR_MASK;
    if ((v ^ c->fpscr) & SH4_FPSCR_FR) {
        uint32_t t[16];
        memcpy(t, c->fr, sizeof t);
        memcpy(c->fr, c->xf, sizeof t);
        memcpy(c->xf, t, sizeof t);
    }
    c->fpscr = v;
}

/* Exception / interrupt entry: SPC, SSR, SGR saved, privileged, bank 1, blocked, PC = VBR + off. */
static inline void sh4_enter_exception(sh4_cpu *c, uint32_t off, uint32_t spc)
{
    c->spc = spc;
    c->ssr = sh4_get_sr(c);
    c->sgr = c->r[15];
    sh4_set_sr(c, sh4_get_sr(c) | SH4_SR_MD | SH4_SR_RB | SH4_SR_BL);
    c->pc = c->vbr + off;
}

/* ---- FPU helpers ---- */

static inline float sh4_f(const uint32_t *f, unsigned n)
{
    float v;
    memcpy(&v, &f[n], 4);
    return v;
}
static inline void sh4_setf(uint32_t *f, unsigned n, float v)
{
    memcpy(&f[n], &v, 4);
}
static inline double sh4_d(const uint32_t *f, unsigned n)
{
    uint64_t b = ((uint64_t)f[n] << 32) | f[n + 1];
    double v;
    memcpy(&v, &b, 8);
    return v;
}
static inline void sh4_setd(uint32_t *f, unsigned n, double v)
{
    uint64_t b;
    memcpy(&b, &v, 8);
    f[n] = (uint32_t)(b >> 32);
    f[n + 1] = (uint32_t)b;
}
/* SZ=1 register pair: even = DRn (current bank), odd = XDn (the other bank). */
static inline uint32_t *sh4_pair(sh4_cpu *c, unsigned n)
{
    return (n & 1) ? &c->xf[n & 14] : &c->fr[n & 14];
}
static inline uint32_t sh4_ftrc(double v)
{
    if (v != v)
        return 0x80000000u;
    if (v >= 2147483647.0)
        return 0x7FFFFFFFu;
    if (v <= -2147483648.0)
        return 0x80000000u;
    return (uint32_t)(int32_t)v;
}

#define SH4_N ((op >> 8) & 15)
#define SH4_M ((op >> 4) & 15)
#define SH4_OP(NAME) static inline void sh4i_##NAME(sh4_cpu *c, uint32_t pc, uint32_t op)
#define SH4_UNUSED (void)pc, (void)op

/* ---- 0000 ---- */
SH4_OP(CLRT) { SH4_UNUSED; c->t = 0; }
SH4_OP(SETT) { SH4_UNUSED; c->t = 1; }
SH4_OP(CLRMAC) { SH4_UNUSED; c->mach = c->macl = 0; }
SH4_OP(LDTLB) { SH4_UNUSED; (void)c; }
SH4_OP(CLRS) { SH4_UNUSED; c->s = 0; }
SH4_OP(SETS) { SH4_UNUSED; c->s = 1; }
SH4_OP(NOP) { SH4_UNUSED; (void)c; }
SH4_OP(DIV0U) { SH4_UNUSED; c->m = c->q = c->t = 0; }
SH4_OP(SLEEP) { (void)op; c->sleeping = 1; c->pc = pc + 2; }
SH4_OP(STC_SR) { (void)pc; c->r[SH4_N] = sh4_get_sr(c); }
SH4_OP(STC_GBR) { (void)pc; c->r[SH4_N] = c->gbr; }
SH4_OP(STC_VBR) { (void)pc; c->r[SH4_N] = c->vbr; }
SH4_OP(STC_SSR) { (void)pc; c->r[SH4_N] = c->ssr; }
SH4_OP(STC_SPC) { (void)pc; c->r[SH4_N] = c->spc; }
SH4_OP(STC_SGR) { (void)pc; c->r[SH4_N] = c->sgr; }
SH4_OP(STC_DBR) { (void)pc; c->r[SH4_N] = c->dbr; }
SH4_OP(STC_BANK) { (void)pc; c->r[SH4_N] = c->rbank[SH4_M & 7]; }
SH4_OP(PREF) { (void)pc; dc_pref(c, c->r[SH4_N]); }
SH4_OP(OCBI) { SH4_UNUSED; (void)c; }
SH4_OP(OCBP) { SH4_UNUSED; (void)c; }
SH4_OP(OCBWB) { SH4_UNUSED; (void)c; }
SH4_OP(MOVCA_L) { (void)pc; sh4_w32(c, c->r[SH4_N], c->r[0]); }
SH4_OP(MOVT) { (void)pc; c->r[SH4_N] = c->t; }
SH4_OP(STS_MACH) { (void)pc; c->r[SH4_N] = c->mach; }
SH4_OP(STS_MACL) { (void)pc; c->r[SH4_N] = c->macl; }
SH4_OP(STS_PR) { (void)pc; c->r[SH4_N] = c->pr; }
SH4_OP(STS_FPUL) { (void)pc; c->r[SH4_N] = c->fpul; }
SH4_OP(STS_FPSCR) { (void)pc; c->r[SH4_N] = c->fpscr; }
SH4_OP(MOVB_S0) { (void)pc; sh4_w8(c, c->r[0] + c->r[SH4_N], (uint8_t)c->r[SH4_M]); }
SH4_OP(MOVW_S0) { (void)pc; sh4_w16(c, c->r[0] + c->r[SH4_N], (uint16_t)c->r[SH4_M]); }
SH4_OP(MOVL_S0) { (void)pc; sh4_w32(c, c->r[0] + c->r[SH4_N], c->r[SH4_M]); }
SH4_OP(MUL_L) { (void)pc; c->macl = c->r[SH4_N] * c->r[SH4_M]; }
SH4_OP(MOVB_L0) { (void)pc; c->r[SH4_N] = (uint32_t)(int8_t)sh4_r8(c, c->r[0] + c->r[SH4_M]); }
SH4_OP(MOVW_L0) { (void)pc; c->r[SH4_N] = (uint32_t)(int16_t)sh4_r16(c, c->r[0] + c->r[SH4_M]); }
SH4_OP(MOVL_L0) { (void)pc; c->r[SH4_N] = sh4_r32(c, c->r[0] + c->r[SH4_M]); }
SH4_OP(MAC_L)
{
    (void)pc;
    int32_t a = (int32_t)sh4_r32(c, c->r[SH4_N]);
    c->r[SH4_N] += 4;
    int32_t b = (int32_t)sh4_r32(c, c->r[SH4_M]);
    c->r[SH4_M] += 4;
    int64_t mac = (int64_t)(((uint64_t)c->mach << 32) | c->macl) + (int64_t)a * b;
    if (c->s) {
        if (mac > 0x00007FFFFFFFFFFFll)
            mac = 0x00007FFFFFFFFFFFll;
        else if (mac < -0x0000800000000000ll)
            mac = -0x0000800000000000ll;
    }
    c->mach = (uint32_t)((uint64_t)mac >> 32);
    c->macl = (uint32_t)mac;
}

/* ---- 0001 / 0101: MOV.L with displacement ---- */
SH4_OP(MOVL_SD) { (void)pc; sh4_w32(c, c->r[SH4_N] + (op & 15) * 4, c->r[SH4_M]); }
SH4_OP(MOVL_LD) { (void)pc; c->r[SH4_N] = sh4_r32(c, c->r[SH4_M] + (op & 15) * 4); }

/* ---- 0010 ---- */
SH4_OP(MOVB_S) { (void)pc; sh4_w8(c, c->r[SH4_N], (uint8_t)c->r[SH4_M]); }
SH4_OP(MOVW_S) { (void)pc; sh4_w16(c, c->r[SH4_N], (uint16_t)c->r[SH4_M]); }
SH4_OP(MOVL_S) { (void)pc; sh4_w32(c, c->r[SH4_N], c->r[SH4_M]); }
SH4_OP(MOVB_SM) { (void)pc; sh4_w8(c, c->r[SH4_N] - 1, (uint8_t)c->r[SH4_M]); c->r[SH4_N] -= 1; }
SH4_OP(MOVW_SM) { (void)pc; sh4_w16(c, c->r[SH4_N] - 2, (uint16_t)c->r[SH4_M]); c->r[SH4_N] -= 2; }
SH4_OP(MOVL_SM) { (void)pc; sh4_w32(c, c->r[SH4_N] - 4, c->r[SH4_M]); c->r[SH4_N] -= 4; }
SH4_OP(DIV0S)
{
    (void)pc;
    c->q = c->r[SH4_N] >> 31;
    c->m = c->r[SH4_M] >> 31;
    c->t = c->q ^ c->m;
}
SH4_OP(TST) { (void)pc; c->t = (c->r[SH4_N] & c->r[SH4_M]) == 0; }
SH4_OP(AND) { (void)pc; c->r[SH4_N] &= c->r[SH4_M]; }
SH4_OP(XOR) { (void)pc; c->r[SH4_N] ^= c->r[SH4_M]; }
SH4_OP(OR) { (void)pc; c->r[SH4_N] |= c->r[SH4_M]; }
SH4_OP(CMP_STR)
{
    (void)pc;
    uint32_t x = c->r[SH4_N] ^ c->r[SH4_M];
    c->t = !(x & 0xFF) || !(x & 0xFF00) || !(x & 0xFF0000) || !(x & 0xFF000000u);
}
SH4_OP(XTRCT) { (void)pc; c->r[SH4_N] = (c->r[SH4_N] >> 16) | (c->r[SH4_M] << 16); }
SH4_OP(MULU_W) { (void)pc; c->macl = (uint32_t)(uint16_t)c->r[SH4_N] * (uint16_t)c->r[SH4_M]; }
SH4_OP(MULS_W) { (void)pc; c->macl = (uint32_t)((int32_t)(int16_t)c->r[SH4_N] * (int16_t)c->r[SH4_M]); }

/* ---- 0011 ---- */
SH4_OP(CMP_EQ) { (void)pc; c->t = c->r[SH4_N] == c->r[SH4_M]; }
SH4_OP(CMP_HS) { (void)pc; c->t = c->r[SH4_N] >= c->r[SH4_M]; }
SH4_OP(CMP_GE) { (void)pc; c->t = (int32_t)c->r[SH4_N] >= (int32_t)c->r[SH4_M]; }
SH4_OP(CMP_HI) { (void)pc; c->t = c->r[SH4_N] > c->r[SH4_M]; }
SH4_OP(CMP_GT) { (void)pc; c->t = (int32_t)c->r[SH4_N] > (int32_t)c->r[SH4_M]; }
SH4_OP(DIV1)
{
    (void)pc;
    unsigned n = SH4_N;
    uint32_t old_q = c->q, rm = c->r[SH4_M], tmp0;
    uint32_t tmp1;
    c->q = c->r[n] >> 31;
    c->r[n] = (c->r[n] << 1) | c->t;
    if (old_q == c->m) {
        tmp0 = c->r[n];
        c->r[n] -= rm;
        tmp1 = c->r[n] > tmp0;
        if (old_q == 0)
            c->q = c->q ? !tmp1 : tmp1;
        else
            c->q = c->q ? tmp1 : !tmp1;
    } else {
        tmp0 = c->r[n];
        c->r[n] += rm;
        tmp1 = c->r[n] < tmp0;
        if (old_q == 0)
            c->q = c->q ? tmp1 : !tmp1;
        else
            c->q = c->q ? !tmp1 : tmp1;
    }
    c->t = c->q == c->m;
}
SH4_OP(DMULU_L)
{
    (void)pc;
    uint64_t v = (uint64_t)c->r[SH4_N] * c->r[SH4_M];
    c->mach = (uint32_t)(v >> 32);
    c->macl = (uint32_t)v;
}
SH4_OP(DMULS_L)
{
    (void)pc;
    int64_t v = (int64_t)(int32_t)c->r[SH4_N] * (int32_t)c->r[SH4_M];
    c->mach = (uint32_t)((uint64_t)v >> 32);
    c->macl = (uint32_t)v;
}
SH4_OP(SUB) { (void)pc; c->r[SH4_N] -= c->r[SH4_M]; }
SH4_OP(ADD) { (void)pc; c->r[SH4_N] += c->r[SH4_M]; }
SH4_OP(SUBC)
{
    (void)pc;
    uint32_t a = c->r[SH4_N], tmp1 = a - c->r[SH4_M], res = tmp1 - c->t;
    c->t = (a < tmp1) | (tmp1 < res);
    c->r[SH4_N] = res;
}
SH4_OP(ADDC)
{
    (void)pc;
    uint32_t a = c->r[SH4_N], tmp1 = a + c->r[SH4_M], res = tmp1 + c->t;
    c->t = (a > tmp1) | (tmp1 > res);
    c->r[SH4_N] = res;
}
SH4_OP(SUBV)
{
    (void)pc;
    uint32_t a = c->r[SH4_N], b = c->r[SH4_M], res = a - b;
    c->t = (((a ^ b) & (a ^ res)) >> 31) & 1;
    c->r[SH4_N] = res;
}
SH4_OP(ADDV)
{
    (void)pc;
    uint32_t a = c->r[SH4_N], b = c->r[SH4_M], res = a + b;
    c->t = ((~(a ^ b) & (a ^ res)) >> 31) & 1;
    c->r[SH4_N] = res;
}

/* ---- 0100 ---- */
SH4_OP(SHLL) { (void)pc; c->t = c->r[SH4_N] >> 31; c->r[SH4_N] <<= 1; }
SH4_OP(SHAL) { (void)pc; c->t = c->r[SH4_N] >> 31; c->r[SH4_N] <<= 1; }
SH4_OP(SHLR) { (void)pc; c->t = c->r[SH4_N] & 1; c->r[SH4_N] >>= 1; }
SH4_OP(SHAR) { (void)pc; c->t = c->r[SH4_N] & 1; c->r[SH4_N] = (uint32_t)((int32_t)c->r[SH4_N] >> 1); }
SH4_OP(ROTL) { (void)pc; uint32_t v = c->r[SH4_N]; c->t = v >> 31; c->r[SH4_N] = (v << 1) | c->t; }
SH4_OP(ROTR) { (void)pc; uint32_t v = c->r[SH4_N]; c->t = v & 1; c->r[SH4_N] = (v >> 1) | (c->t << 31); }
SH4_OP(ROTCL) { (void)pc; uint32_t v = c->r[SH4_N]; c->r[SH4_N] = (v << 1) | c->t; c->t = v >> 31; }
SH4_OP(ROTCR) { (void)pc; uint32_t v = c->r[SH4_N]; c->r[SH4_N] = (v >> 1) | (c->t << 31); c->t = v & 1; }
SH4_OP(SHLL2) { (void)pc; c->r[SH4_N] <<= 2; }
SH4_OP(SHLR2) { (void)pc; c->r[SH4_N] >>= 2; }
SH4_OP(SHLL8) { (void)pc; c->r[SH4_N] <<= 8; }
SH4_OP(SHLR8) { (void)pc; c->r[SH4_N] >>= 8; }
SH4_OP(SHLL16) { (void)pc; c->r[SH4_N] <<= 16; }
SH4_OP(SHLR16) { (void)pc; c->r[SH4_N] >>= 16; }
SH4_OP(DT) { (void)pc; c->r[SH4_N]--; c->t = c->r[SH4_N] == 0; }
SH4_OP(CMP_PZ) { (void)pc; c->t = (int32_t)c->r[SH4_N] >= 0; }
SH4_OP(CMP_PL) { (void)pc; c->t = (int32_t)c->r[SH4_N] > 0; }
SH4_OP(TAS_B)
{
    (void)pc;
    uint32_t a = c->r[SH4_N];
    uint8_t v = sh4_r8(c, a);
    c->t = v == 0;
    sh4_w8(c, a, v | 0x80);
}
#define SH4_STL(NAME, EXPR)                                                                                          \
    SH4_OP(NAME)                                                                                                     \
    {                                                                                                                \
        (void)pc;                                                                                                    \
        sh4_w32(c, c->r[SH4_N] - 4, (EXPR));                                                                         \
        c->r[SH4_N] -= 4;                                                                                            \
    }
SH4_STL(STSL_MACH, c->mach)
SH4_STL(STSL_MACL, c->macl)
SH4_STL(STSL_PR, c->pr)
SH4_STL(STSL_FPUL, c->fpul)
SH4_STL(STSL_FPSCR, c->fpscr)
SH4_STL(STCL_SR, sh4_get_sr(c))
SH4_STL(STCL_GBR, c->gbr)
SH4_STL(STCL_VBR, c->vbr)
SH4_STL(STCL_SSR, c->ssr)
SH4_STL(STCL_SPC, c->spc)
SH4_STL(STCL_SGR, c->sgr)
SH4_STL(STCL_DBR, c->dbr)
SH4_STL(STCL_BANK, c->rbank[SH4_M & 7])
#undef SH4_STL
#define SH4_LDL(NAME, DST)                                                                                           \
    SH4_OP(NAME)                                                                                                     \
    {                                                                                                                \
        (void)pc;                                                                                                    \
        uint32_t v_ = sh4_r32(c, c->r[SH4_N]);                                                                       \
        c->r[SH4_N] += 4;                                                                                            \
        DST = v_;                                                                                                    \
    }
SH4_LDL(LDSL_MACH, c->mach)
SH4_LDL(LDSL_MACL, c->macl)
SH4_LDL(LDSL_PR, c->pr)
SH4_LDL(LDSL_FPUL, c->fpul)
SH4_LDL(LDCL_GBR, c->gbr)
SH4_LDL(LDCL_VBR, c->vbr)
SH4_LDL(LDCL_SSR, c->ssr)
SH4_LDL(LDCL_SPC, c->spc)
SH4_LDL(LDCL_DBR, c->dbr)
SH4_LDL(LDCL_BANK, c->rbank[SH4_M & 7])
#undef SH4_LDL
SH4_OP(LDSL_FPSCR)
{
    (void)pc;
    uint32_t v = sh4_r32(c, c->r[SH4_N]);
    c->r[SH4_N] += 4;
    sh4_set_fpscr(c, v);
}
SH4_OP(LDCL_SR)
{
    uint32_t v = sh4_r32(c, c->r[SH4_N]);
    c->r[SH4_N] += 4;
    sh4_set_sr(c, v);
    (void)op;
    c->pc = pc + 2;
}
SH4_OP(LDC_SR) { sh4_set_sr(c, c->r[SH4_N]); c->pc = pc + 2; }
SH4_OP(LDS_MACH) { (void)pc; c->mach = c->r[SH4_N]; }
SH4_OP(LDS_MACL) { (void)pc; c->macl = c->r[SH4_N]; }
SH4_OP(LDS_PR) { (void)pc; c->pr = c->r[SH4_N]; }
SH4_OP(LDS_FPUL) { (void)pc; c->fpul = c->r[SH4_N]; }
SH4_OP(LDS_FPSCR) { (void)pc; sh4_set_fpscr(c, c->r[SH4_N]); }
SH4_OP(LDC_GBR) { (void)pc; c->gbr = c->r[SH4_N]; }
SH4_OP(LDC_VBR) { (void)pc; c->vbr = c->r[SH4_N]; }
SH4_OP(LDC_SSR) { (void)pc; c->ssr = c->r[SH4_N]; }
SH4_OP(LDC_SPC) { (void)pc; c->spc = c->r[SH4_N]; }
SH4_OP(LDC_DBR) { (void)pc; c->dbr = c->r[SH4_N]; }
SH4_OP(LDC_BANK) { (void)pc; c->rbank[SH4_M & 7] = c->r[SH4_N]; }
SH4_OP(SHAD)
{
    (void)pc;
    unsigned n = SH4_N;
    int32_t s = (int32_t)c->r[SH4_M];
    if (s >= 0)
        c->r[n] <<= (s & 31);
    else if ((s & 31) == 0)
        c->r[n] = ((int32_t)c->r[n] < 0) ? 0xFFFFFFFFu : 0;
    else
        c->r[n] = (uint32_t)((int32_t)c->r[n] >> ((~s & 31) + 1));
}
SH4_OP(SHLD)
{
    (void)pc;
    unsigned n = SH4_N;
    int32_t s = (int32_t)c->r[SH4_M];
    if (s >= 0)
        c->r[n] <<= (s & 31);
    else if ((s & 31) == 0)
        c->r[n] = 0;
    else
        c->r[n] >>= ((~s & 31) + 1);
}
SH4_OP(MAC_W)
{
    (void)pc;
    int16_t a = (int16_t)sh4_r16(c, c->r[SH4_N]);
    c->r[SH4_N] += 2;
    int16_t b = (int16_t)sh4_r16(c, c->r[SH4_M]);
    c->r[SH4_M] += 2;
    int32_t prod = (int32_t)a * b;
    if (c->s) {
        int64_t v = (int64_t)(int32_t)c->macl + prod;
        if (v > 0x7FFFFFFFll) {
            v = 0x7FFFFFFFll;
            c->mach = 1;
        } else if (v < -0x80000000ll) {
            v = -0x80000000ll;
            c->mach = 1;
        }
        c->macl = (uint32_t)v;
    } else {
        int64_t mac = (int64_t)(((uint64_t)c->mach << 32) | c->macl) + prod;
        c->mach = (uint32_t)((uint64_t)mac >> 32);
        c->macl = (uint32_t)mac;
    }
}

/* ---- 0110 ---- */
SH4_OP(MOVB_L) { (void)pc; c->r[SH4_N] = (uint32_t)(int8_t)sh4_r8(c, c->r[SH4_M]); }
SH4_OP(MOVW_L) { (void)pc; c->r[SH4_N] = (uint32_t)(int16_t)sh4_r16(c, c->r[SH4_M]); }
SH4_OP(MOVL_L) { (void)pc; c->r[SH4_N] = sh4_r32(c, c->r[SH4_M]); }
SH4_OP(MOV) { (void)pc; c->r[SH4_N] = c->r[SH4_M]; }
SH4_OP(MOVB_LP)
{
    (void)pc;
    unsigned n = SH4_N, m = SH4_M;
    uint32_t v = (uint32_t)(int8_t)sh4_r8(c, c->r[m]);
    if (n != m)
        c->r[m] += 1;
    c->r[n] = v;
}
SH4_OP(MOVW_LP)
{
    (void)pc;
    unsigned n = SH4_N, m = SH4_M;
    uint32_t v = (uint32_t)(int16_t)sh4_r16(c, c->r[m]);
    if (n != m)
        c->r[m] += 2;
    c->r[n] = v;
}
SH4_OP(MOVL_LP)
{
    (void)pc;
    unsigned n = SH4_N, m = SH4_M;
    uint32_t v = sh4_r32(c, c->r[m]);
    if (n != m)
        c->r[m] += 4;
    c->r[n] = v;
}
SH4_OP(NOT) { (void)pc; c->r[SH4_N] = ~c->r[SH4_M]; }
SH4_OP(SWAP_B)
{
    (void)pc;
    uint32_t v = c->r[SH4_M];
    c->r[SH4_N] = (v & 0xFFFF0000u) | ((v & 0xFF) << 8) | ((v >> 8) & 0xFF);
}
SH4_OP(SWAP_W) { (void)pc; uint32_t v = c->r[SH4_M]; c->r[SH4_N] = (v >> 16) | (v << 16); }
SH4_OP(NEGC)
{
    (void)pc;
    uint32_t tmp = 0u - c->r[SH4_M], res = tmp - c->t;
    c->t = (0 < tmp) | (tmp < res);
    c->r[SH4_N] = res;
}
SH4_OP(NEG) { (void)pc; c->r[SH4_N] = 0u - c->r[SH4_M]; }
SH4_OP(EXTU_B) { (void)pc; c->r[SH4_N] = c->r[SH4_M] & 0xFF; }
SH4_OP(EXTU_W) { (void)pc; c->r[SH4_N] = c->r[SH4_M] & 0xFFFF; }
SH4_OP(EXTS_B) { (void)pc; c->r[SH4_N] = (uint32_t)(int8_t)c->r[SH4_M]; }
SH4_OP(EXTS_W) { (void)pc; c->r[SH4_N] = (uint32_t)(int16_t)c->r[SH4_M]; }

/* ---- 0111 / 1000 / 1001 / 1100 / 1101 / 1110: immediates and displacements ---- */
SH4_OP(ADDI) { (void)pc; c->r[SH4_N] += (uint32_t)(int8_t)op; }
SH4_OP(MOVB_S4) { (void)pc; sh4_w8(c, c->r[SH4_M] + (op & 15), (uint8_t)c->r[0]); }
SH4_OP(MOVW_S4) { (void)pc; sh4_w16(c, c->r[SH4_M] + (op & 15) * 2, (uint16_t)c->r[0]); }
SH4_OP(MOVB_L4) { (void)pc; c->r[0] = (uint32_t)(int8_t)sh4_r8(c, c->r[SH4_M] + (op & 15)); }
SH4_OP(MOVW_L4) { (void)pc; c->r[0] = (uint32_t)(int16_t)sh4_r16(c, c->r[SH4_M] + (op & 15) * 2); }
SH4_OP(CMP_EQI) { (void)pc; c->t = c->r[0] == (uint32_t)(int8_t)op; }
SH4_OP(MOVW_PC) { c->r[SH4_N] = (uint32_t)(int16_t)sh4_r16(c, pc + 4 + (op & 0xFF) * 2); }
SH4_OP(MOVL_PC) { c->r[SH4_N] = sh4_r32(c, (pc & ~3u) + 4 + (op & 0xFF) * 4); }
SH4_OP(MOVA) { c->r[0] = (pc & ~3u) + 4 + (op & 0xFF) * 4; }
SH4_OP(MOVI) { (void)pc; c->r[SH4_N] = (uint32_t)(int8_t)op; }
SH4_OP(MOVB_SG) { (void)pc; sh4_w8(c, c->gbr + (op & 0xFF), (uint8_t)c->r[0]); }
SH4_OP(MOVW_SG) { (void)pc; sh4_w16(c, c->gbr + (op & 0xFF) * 2, (uint16_t)c->r[0]); }
SH4_OP(MOVL_SG) { (void)pc; sh4_w32(c, c->gbr + (op & 0xFF) * 4, c->r[0]); }
SH4_OP(MOVB_LG) { (void)pc; c->r[0] = (uint32_t)(int8_t)sh4_r8(c, c->gbr + (op & 0xFF)); }
SH4_OP(MOVW_LG) { (void)pc; c->r[0] = (uint32_t)(int16_t)sh4_r16(c, c->gbr + (op & 0xFF) * 2); }
SH4_OP(MOVL_LG) { (void)pc; c->r[0] = sh4_r32(c, c->gbr + (op & 0xFF) * 4); }
SH4_OP(TRAPA)
{
    c->tra = (op & 0xFF) << 2;
    c->expevt = 0x160;
    sh4_enter_exception(c, 0x100, pc + 2);
}
SH4_OP(TSTI) { (void)pc; c->t = (c->r[0] & (op & 0xFF)) == 0; }
SH4_OP(ANDI) { (void)pc; c->r[0] &= op & 0xFF; }
SH4_OP(XORI) { (void)pc; c->r[0] ^= op & 0xFF; }
SH4_OP(ORI) { (void)pc; c->r[0] |= op & 0xFF; }
SH4_OP(TSTB) { (void)pc; c->t = (sh4_r8(c, c->gbr + c->r[0]) & (op & 0xFF)) == 0; }
SH4_OP(ANDB) { (void)pc; uint32_t a = c->gbr + c->r[0]; sh4_w8(c, a, sh4_r8(c, a) & (op & 0xFF)); }
SH4_OP(XORB) { (void)pc; uint32_t a = c->gbr + c->r[0]; sh4_w8(c, a, sh4_r8(c, a) ^ (op & 0xFF)); }
SH4_OP(ORB) { (void)pc; uint32_t a = c->gbr + c->r[0]; sh4_w8(c, a, sh4_r8(c, a) | (op & 0xFF)); }

/* ---- 1111: FPU ---- */
#define SH4_PR (c->fpscr & SH4_FPSCR_PR)
#define SH4_SZ (c->fpscr & SH4_FPSCR_SZ)
SH4_OP(FSCHG) { SH4_UNUSED; c->fpscr ^= SH4_FPSCR_SZ; }
SH4_OP(FRCHG) { SH4_UNUSED; sh4_set_fpscr(c, c->fpscr ^ SH4_FPSCR_FR); }
SH4_OP(FTRV)
{
    (void)pc;
    unsigned n = ((op >> 10) & 3) * 4;
    float v[4], o[4];
    for (int i = 0; i < 4; i++)
        v[i] = sh4_f(c->fr, n + i);
    for (int i = 0; i < 4; i++)
        o[i] = sh4_f(c->xf, i) * v[0] + sh4_f(c->xf, 4 + i) * v[1] + sh4_f(c->xf, 8 + i) * v[2] +
               sh4_f(c->xf, 12 + i) * v[3];
    for (int i = 0; i < 4; i++)
        sh4_setf(c->fr, n + i, o[i]);
}
SH4_OP(FSCA)
{
    (void)pc;
    unsigned n = ((op >> 9) & 7) * 2;
    double a = (double)(c->fpul & 0xFFFF) * (6.283185307179586476925 / 65536.0);
    sh4_setf(c->fr, n, (float)sin(a));
    sh4_setf(c->fr, n + 1, (float)cos(a));
}
SH4_OP(FIPR)
{
    (void)pc;
    unsigned n = ((op >> 10) & 3) * 4, m = ((op >> 8) & 3) * 4;
    float s = 0;
    for (int i = 0; i < 4; i++)
        s += sh4_f(c->fr, n + i) * sh4_f(c->fr, m + i);
    sh4_setf(c->fr, n + 3, s);
}
SH4_OP(FSTS) { (void)pc; c->fr[SH4_N] = c->fpul; }
SH4_OP(FLDS) { (void)pc; c->fpul = c->fr[SH4_N]; }
SH4_OP(FLOAT)
{
    (void)pc;
    if (SH4_PR)
        sh4_setd(c->fr, SH4_N & 14, (double)(int32_t)c->fpul);
    else
        sh4_setf(c->fr, SH4_N, (float)(int32_t)c->fpul);
}
SH4_OP(FTRC)
{
    (void)pc;
    c->fpul = SH4_PR ? sh4_ftrc(sh4_d(c->fr, SH4_N & 14)) : sh4_ftrc((double)sh4_f(c->fr, SH4_N));
}
SH4_OP(FNEG) { (void)pc; c->fr[SH4_PR ? (SH4_N & 14) : SH4_N] ^= 0x80000000u; }
SH4_OP(FABS) { (void)pc; c->fr[SH4_PR ? (SH4_N & 14) : SH4_N] &= 0x7FFFFFFFu; }
SH4_OP(FSQRT)
{
    (void)pc;
    if (SH4_PR)
        sh4_setd(c->fr, SH4_N & 14, sqrt(sh4_d(c->fr, SH4_N & 14)));
    else
        sh4_setf(c->fr, SH4_N, sqrtf(sh4_f(c->fr, SH4_N)));
}
SH4_OP(FSRRA) { (void)pc; sh4_setf(c->fr, SH4_N, 1.0f / sqrtf(sh4_f(c->fr, SH4_N))); }
SH4_OP(FLDI0) { (void)pc; c->fr[SH4_N] = 0; }
SH4_OP(FLDI1) { (void)pc; c->fr[SH4_N] = 0x3F800000u; }
SH4_OP(FCNVSD)
{
    (void)pc;
    float f;
    memcpy(&f, &c->fpul, 4);
    sh4_setd(c->fr, SH4_N & 14, (double)f);
}
SH4_OP(FCNVDS)
{
    (void)pc;
    float f = (float)sh4_d(c->fr, SH4_N & 14);
    memcpy(&c->fpul, &f, 4);
}
#define SH4_FARITH(NAME, OPR)                                                                                        \
    SH4_OP(NAME)                                                                                                     \
    {                                                                                                                \
        (void)pc;                                                                                                    \
        unsigned n = SH4_N, m = SH4_M;                                                                               \
        if (SH4_PR)                                                                                                  \
            sh4_setd(c->fr, n & 14, sh4_d(c->fr, n & 14) OPR sh4_d(c->fr, m & 14));                                  \
        else                                                                                                         \
            sh4_setf(c->fr, n, sh4_f(c->fr, n) OPR sh4_f(c->fr, m));                                                 \
    }
SH4_FARITH(FADD, +)
SH4_FARITH(FSUB, -)
SH4_FARITH(FMUL, *)
SH4_FARITH(FDIV, /)
#undef SH4_FARITH
SH4_OP(FCMP_EQ)
{
    (void)pc;
    unsigned n = SH4_N, m = SH4_M;
    c->t = SH4_PR ? sh4_d(c->fr, n & 14) == sh4_d(c->fr, m & 14) : sh4_f(c->fr, n) == sh4_f(c->fr, m);
}
SH4_OP(FCMP_GT)
{
    (void)pc;
    unsigned n = SH4_N, m = SH4_M;
    c->t = SH4_PR ? sh4_d(c->fr, n & 14) > sh4_d(c->fr, m & 14) : sh4_f(c->fr, n) > sh4_f(c->fr, m);
}
SH4_OP(FMAC)
{
    (void)pc;
    unsigned n = SH4_N;
    sh4_setf(c->fr, n, sh4_f(c->fr, 0) * sh4_f(c->fr, SH4_M) + sh4_f(c->fr, n));
}
/* FMOV loads / stores: one word, or (SZ=1) a pair, FR(n) / XD at the lower address. */
static inline void sh4_fload(sh4_cpu *c, unsigned n, uint32_t a)
{
    if (SH4_SZ) {
        uint32_t *p = sh4_pair(c, n);
        p[0] = sh4_r32(c, a);
        p[1] = sh4_r32(c, a + 4);
    } else {
        c->fr[n] = sh4_r32(c, a);
    }
}
static inline void sh4_fstore(sh4_cpu *c, unsigned m, uint32_t a)
{
    if (SH4_SZ) {
        const uint32_t *p = sh4_pair(c, m);
        sh4_w32(c, a, p[0]);
        sh4_w32(c, a + 4, p[1]);
    } else {
        sh4_w32(c, a, c->fr[m]);
    }
}
SH4_OP(FMOV_L0) { (void)pc; sh4_fload(c, SH4_N, c->r[0] + c->r[SH4_M]); }
SH4_OP(FMOV_S0) { (void)pc; sh4_fstore(c, SH4_M, c->r[0] + c->r[SH4_N]); }
SH4_OP(FMOV_L) { (void)pc; sh4_fload(c, SH4_N, c->r[SH4_M]); }
SH4_OP(FMOV_LP)
{
    (void)pc;
    unsigned m = SH4_M;
    uint32_t a = c->r[m];
    c->r[m] += SH4_SZ ? 8 : 4;
    sh4_fload(c, SH4_N, a);
}
SH4_OP(FMOV_S) { (void)pc; sh4_fstore(c, SH4_M, c->r[SH4_N]); }
SH4_OP(FMOV_SM)
{
    (void)pc;
    unsigned n = SH4_N;
    uint32_t a = c->r[n] - (SH4_SZ ? 8 : 4);
    sh4_fstore(c, SH4_M, a);
    c->r[n] = a;
}
SH4_OP(FMOV)
{
    (void)pc;
    unsigned n = SH4_N, m = SH4_M;
    if (SH4_SZ) {
        uint32_t *d = sh4_pair(c, n);
        const uint32_t *s = sh4_pair(c, m);
        uint32_t s0 = s[0], s1 = s[1];
        d[0] = s0;
        d[1] = s1;
    } else {
        c->fr[n] = c->fr[m];
    }
}
#undef SH4_PR
#undef SH4_SZ

/* ---- branches: no sh4i_* semantics (the interpreter and the lifter handle them); empty ones keep the X-macro
 * dispatch uniform ---- */
#define SH4_BRANCH_STUB(NAME) SH4_OP(NAME) { SH4_UNUSED; (void)c; }
SH4_BRANCH_STUB(BT)
SH4_BRANCH_STUB(BF)
SH4_BRANCH_STUB(BT_S)
SH4_BRANCH_STUB(BF_S)
SH4_BRANCH_STUB(BRA)
SH4_BRANCH_STUB(BSR)
SH4_BRANCH_STUB(BRAF)
SH4_BRANCH_STUB(BSRF)
SH4_BRANCH_STUB(JMP)
SH4_BRANCH_STUB(JSR)
SH4_BRANCH_STUB(RTS)
SH4_BRANCH_STUB(RTE)
#undef SH4_BRANCH_STUB


static inline uint32_t sh4_bdisp8(uint32_t pc, uint32_t op)
{
    return pc + 4 + (uint32_t)((int32_t)(int8_t)op * 2);
}
static inline uint32_t sh4_bdisp12(uint32_t pc, uint32_t op)
{
    int32_t d = (int32_t)(op & 0xFFF);
    if (d & 0x800)
        d -= 0x1000;
    return pc + 4 + (uint32_t)(d * 2);
}

#ifdef __cplusplus
}
#endif

#endif
