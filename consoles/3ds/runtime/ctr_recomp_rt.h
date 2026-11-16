/* ctr_recomp_rt.h -- the ABI between lifted 3DS ARM11 code (consoles/3ds/tools ctr_tools.recomp) and the core
 * (XBA-228 spike), and the ARMv6K / VFPv2 semantics both share. Part of CTR_RECOMP_ABI (ctr_recomp.h): change it
 * together with the tools and bump CTR_RECOMP_ABI.
 *
 * The lifter is gba_tools' through nds_tools' ARMv5TE layer, extended to ARMv6K + VFPv2 (ctr_tools/lift.py): it emits
 * the RC_* macro vocabulary of consoles/gba/runtime/gba_recomp_rt.h, and this header gives those macros their 3DS
 * meaning. Same shape as the DS (registers, flags and the clock in C locals; blocks with one event check per segment;
 * calls, returns and indirect jumps back to the dispatcher with PC set; rare instructions handed to the interpreter
 * with CTR_RC_EXIT_INTERP), a different machine:
 * - user mode only: no banked registers, no exceptions; SVC is the HLE kernel (ctr_kernel.c), always run by the
 *   interpreter;
 * - memory is flat: the 4 GB guest address space is one host reservation (ctr_mem.c), every load and store is
 *   `mem + address`, unaligned accesses included (ARMv6 with U = 1: no rotation, as the 3DS runs); there is no I/O
 *   in a user process, so no bus calls and no side effects on a write;
 * - the GE flags (SEL, parallel add / subtract), the exclusive monitor (LDREX / STREX), the thread pointer
 *   (CP15 c13, read with MRC) and the VFP registers live in the CPU struct;
 * - ARMv6 media instructions and VFP operations are one static inline function each (rc_media, rc_vfp_*), called
 *   by the interpreter with the opcode it fetched and by lifted code with the opcode as a constant (the compiler
 *   folds the decoding away): the two cannot disagree. */
#ifndef CTR_RECOMP_RT_H
#define CTR_RECOMP_RT_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "lifted 3DS code assumes a little-endian host"
#endif

#include "ctr_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctr_cpu {
    uint32_t r[16];      /* between instructions r[15] = next instruction + 8 (ARM) / + 4 (Thumb) */
    uint32_t n, z, c, v; /* flags, 0 or 1 */
    uint32_t cpsr_ctl;   /* CPSR bits 0-7 (user mode 0x10, T); flags live in n z c v q ge */
    uint32_t q;          /* sticky saturation flag (CPSR bit 27), 0 or 1 */
    uint32_t ge;         /* GE[3:0] (CPSR bits 19-16) */
    uint32_t excl;       /* exclusive monitor: 1 after LDREX, cleared by STREX, CLREX and a thread switch */
    uint32_t tls;        /* CP15 c13 c0 3: the thread's TLS (thread local storage) address */
    uint32_t fpscr, fpexc;
    union {
        uint64_t d[16];
        uint32_t s[32];
    } vfp;
} ctr_cpu;

typedef struct ctr_rc_head {
    ctr_cpu cpu;
    uint64_t now;          /* ARM11 cycles since creation (one per instruction, plus one per load) */
    uint64_t next_event;   /* the CPU runs until now reaches it */
    uint8_t wait16[16][2]; /* all zero: the GBA lifter's wait-state terms */
    uint8_t wait32[16][2];
    uint8_t *mem;          /* the guest's 4 GB address space (ctr_mem.c) */
    uint8_t rc_exit;       /* CTR_RC_EXIT_* */
    uint8_t rc_hint;       /* with CTR_RC_EXIT_INTERP: instructions the interpreter may run before native again */
    uint8_t pad_[6];
    uint64_t rc_native;    /* instructions run natively (packs built with -DCTR_RC_COUNT) */
} ctr_rc_head;

enum {
    CTR_RC_EXIT_NORMAL = 0,      /* PC (and T) say where execution continues */
    CTR_RC_EXIT_INTERP = 1,      /* the interpreter runs the instruction(s) at PC, then dispatch goes on */
    CTR_RC_EXIT_NOT_COVERED = 2, /* this function does not hold PC: nothing ran */
};

#define RC_INLINE static inline __attribute__((always_inline))

/* ---- memory: flat, unaligned allowed ---- */

RC_INLINE uint32_t ctr_rd32(const uint8_t *mem, uint32_t a)
{
    uint32_t v;
    memcpy(&v, mem + a, 4);
    return v;
}
RC_INLINE uint32_t ctr_rd16(const uint8_t *mem, uint32_t a)
{
    uint16_t v;
    memcpy(&v, mem + a, 2);
    return v;
}
RC_INLINE uint32_t ctr_rd8(const uint8_t *mem, uint32_t a)
{
    return mem[a];
}
RC_INLINE void ctr_wr32(uint8_t *mem, uint32_t a, uint32_t v)
{
    memcpy(mem + a, &v, 4);
}
RC_INLINE void ctr_wr16(uint8_t *mem, uint32_t a, uint32_t v)
{
    uint16_t t = (uint16_t)v;
    memcpy(mem + a, &t, 2);
}
RC_INLINE void ctr_wr8(uint8_t *mem, uint32_t a, uint32_t v)
{
    mem[a] = (uint8_t)v;
}

/* ---- ARMv4T / v5TE helpers (the DS spike's semantics) ---- */

RC_INLINE uint32_t rc_ror(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

RC_INLINE int rc_mul_cycles(uint32_t rs, int sign)
{
    (void)rs;
    (void)sign;
    return 1;
}

/* ARMv5+: UMULLS / SMULLS / UMLALS / SMLALS leave C unchanged (the lifter writes `fC = rc_mull_carry(...)`). */
#define rc_mull_carry(RM, RS, SIGN) ((void)(RM), (void)(RS), fC)

RC_INLINE uint32_t rc_shift_reg(uint32_t type, uint32_t v, uint32_t amt, uint32_t cin, uint32_t *carry)
{
    if (amt == 0) {
        *carry = cin;
        return v;
    }
    switch (type) {
    case 0:
        if (amt < 32) {
            *carry = (v >> (32 - amt)) & 1;
            return v << amt;
        }
        *carry = amt == 32 ? (v & 1) : 0;
        return 0;
    case 1:
        if (amt < 32) {
            *carry = (v >> (amt - 1)) & 1;
            return v >> amt;
        }
        *carry = amt == 32 ? (v >> 31) : 0;
        return 0;
    case 2:
        if (amt < 32) {
            *carry = (v >> (amt - 1)) & 1;
            return (uint32_t)((int32_t)v >> amt);
        }
        *carry = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    default:
        amt &= 31;
        if (amt == 0) {
            *carry = v >> 31;
            return v;
        }
        *carry = (v >> (amt - 1)) & 1;
        return rc_ror(v, amt);
    }
}

RC_INLINE uint32_t rc_clz(uint32_t v)
{
    return v ? (uint32_t)__builtin_clz(v) : 32u;
}

RC_INLINE int32_t rc_qadd(int32_t a, int32_t b, uint32_t *q)
{
    int64_t s = (int64_t)a + b;
    if (s > INT32_MAX) {
        *q = 1;
        return INT32_MAX;
    }
    if (s < INT32_MIN) {
        *q = 1;
        return INT32_MIN;
    }
    return (int32_t)s;
}

RC_INLINE int32_t rc_qsub(int32_t a, int32_t b, uint32_t *q)
{
    int64_t s = (int64_t)a - b;
    if (s > INT32_MAX) {
        *q = 1;
        return INT32_MAX;
    }
    if (s < INT32_MIN) {
        *q = 1;
        return INT32_MIN;
    }
    return (int32_t)s;
}

RC_INLINE int32_t rc_qadd_flag(int32_t a, int32_t b, uint32_t *q)
{
    uint32_t s = (uint32_t)a + (uint32_t)b;
    if (((uint32_t)a ^ s) & ((uint32_t)b ^ s) & 0x80000000u)
        *q = 1;
    return (int32_t)s;
}

/* ---- ARMv6 media instructions (ARM ARM A4, « media instructions », and UMAAL) ----
 *
 * rc_media(op, f16, f12, f8, f0, &ge, &q): the result of the media instruction op, given the values of the
 * registers its fields 19-16, 15-12, 11-8 and 3-0 name. The destination is bits 19-16 for the signed multiplies
 * and USAD8 (rc_media_dest_hi), bits 15-12 otherwise. SMLALD / SMLSLD (64-bit) are rc_media_long. */

RC_INLINE int rc_media_dest_hi(uint32_t op)
{
    return (op & 0x0F800010u) == 0x07000010u || (op & 0x0FF000F0u) == 0x07800010u;
}

RC_INLINE int32_t rc_sat_s(int64_t v, unsigned bits, uint32_t *q)
{
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -((int64_t)1 << (bits - 1));
    if (v > hi) {
        *q = 1;
        return (int32_t)hi;
    }
    if (v < lo) {
        *q = 1;
        return (int32_t)lo;
    }
    return (int32_t)v;
}

RC_INLINE uint32_t rc_sat_u(int64_t v, unsigned bits, uint32_t *q)
{
    int64_t hi = ((int64_t)1 << bits) - 1;
    if (v > hi) {
        *q = 1;
        return (uint32_t)hi;
    }
    if (v < 0) {
        *q = 1;
        return 0;
    }
    return (uint32_t)v;
}

/* Parallel add / subtract: prefix (bits 22-20) 1 S, 2 Q, 3 SH, 5 U, 6 UQ, 7 UH; op (bits 7-5) 0 ADD16, 1 ASX,
 * 2 SAX, 3 SUB16, 4 ADD8, 7 SUB8. S and U set GE. */
RC_INLINE uint32_t rc_parallel(uint32_t op, uint32_t n, uint32_t m, uint32_t *ge)
{
    unsigned pre = (op >> 20) & 7, kind = (op >> 5) & 7;
    int sgn = pre < 4;
    unsigned mode = pre & 3; /* 1 modular (GE), 2 saturating, 3 halving */
    int lanes = kind >= 4 ? 4 : 2, bits = kind >= 4 ? 8 : 16;
    uint32_t mask = (1u << bits) - 1, res = 0, g = 0;
    for (int i = 0; i < lanes; i++) {
        int j = i;
        int sub;
        switch (kind) {
        case 0: case 4: sub = 0; break;
        case 3: case 7: sub = 1; break;
        case 1: sub = i == 0; j = i ^ 1; break; /* ASX: bottom n.lo - m.hi, top n.hi + m.lo */
        default: sub = i == 1; j = i ^ 1; break; /* SAX: bottom n.lo + m.hi, top n.hi - m.lo */
        }
        uint32_t an = (n >> (i * bits)) & mask, am = (m >> (j * bits)) & mask;
        int32_t a = sgn ? (int32_t)(an << (32 - bits)) >> (32 - bits) : (int32_t)an;
        int32_t b = sgn ? (int32_t)(am << (32 - bits)) >> (32 - bits) : (int32_t)am;
        int32_t r = sub ? a - b : a + b;
        uint32_t lane;
        if (mode == 1) {
            lane = (uint32_t)r & mask;
            int set = sgn ? r >= 0 : (sub ? r >= 0 : r >= (int32_t)(1u << bits));
            if (set)
                g |= (lanes == 2 ? 3u : 1u) << (i * (lanes == 2 ? 2 : 1));
        } else if (mode == 2) {
            uint32_t dummy = 0;
            lane = sgn ? (uint32_t)rc_sat_s(r, (unsigned)bits, &dummy) & mask : rc_sat_u(r, (unsigned)bits, &dummy);
        } else {
            lane = (uint32_t)(r >> 1) & mask;
        }
        res |= lane << (i * bits);
    }
    if (mode == 1)
        *ge = g;
    return res;
}

RC_INLINE uint32_t rc_extend(uint32_t op, uint32_t n, uint32_t m)
{
    uint32_t v = rc_ror(m, ((op >> 10) & 3) * 8);
    unsigned kind = (op >> 20) & 7;
    int add = ((op >> 16) & 15) != 15;
    uint32_t r;
    switch (kind) {
    case 0: /* SXTB16 / SXTAB16 */
    case 4: { /* UXTB16 / UXTAB16 */
        uint32_t lo = v & 0xFF, hi = (v >> 16) & 0xFF;
        if (kind == 0) {
            lo = (uint32_t)(int32_t)(int8_t)lo;
            hi = (uint32_t)(int32_t)(int8_t)hi;
        }
        if (add) {
            lo += n;
            hi += n >> 16;
        }
        return (lo & 0xFFFF) | (hi & 0xFFFF) << 16;
    }
    case 2: r = (uint32_t)(int32_t)(int8_t)v; break;
    case 3: r = (uint32_t)(int32_t)(int16_t)v; break;
    case 6: r = v & 0xFF; break;
    default: r = v & 0xFFFF; break; /* 7 */
    }
    return add ? r + n : r;
}

RC_INLINE uint32_t rc_media(uint32_t op, uint32_t f16, uint32_t f12, uint32_t f8, uint32_t f0, uint32_t *ge,
                            uint32_t *q)
{
    (void)f12;
    if ((op & 0x0F800010u) == 0x06000010u) /* parallel add / subtract */
        return rc_parallel(op, f16, f0, ge);
    if ((op & 0x0FF00030u) == 0x06800010u) { /* PKHBT / PKHTB */
        unsigned sh = (op >> 7) & 31;
        if (op & 0x40) /* PKHTB: ASR, #0 = #32 */
            return (f16 & 0xFFFF0000u) | ((uint32_t)((int32_t)f0 >> (sh ? sh : 31)) & 0xFFFF);
        return (f16 & 0xFFFF) | ((f0 << sh) & 0xFFFF0000u);
    }
    if ((op & 0x0FF00FF0u) == 0x06800FB0u) { /* SEL */
        uint32_t r = 0;
        for (int i = 0; i < 4; i++)
            r |= ((*ge >> i) & 1 ? f16 : f0) & (0xFFu << (8 * i));
        return r;
    }
    if ((op & 0x0FFF0FF0u) == 0x06BF0F30u) /* REV */
        return __builtin_bswap32(f0);
    if ((op & 0x0FFF0FF0u) == 0x06BF0FB0u) /* REV16 */
        return (f0 & 0xFF00FF00u) >> 8 | (f0 & 0x00FF00FFu) << 8;
    if ((op & 0x0FFF0FF0u) == 0x06FF0FB0u) /* REVSH */
        return (uint32_t)(int32_t)(int16_t)(uint16_t)((f0 & 0xFF) << 8 | (f0 >> 8 & 0xFF));
    if ((op & 0x0F8003F0u) == 0x06800070u) /* SXT* / UXT* (and the accumulating forms) */
        return rc_extend(op, f16, f0);
    if ((op & 0x0FF00FF0u) == 0x06A00F30u) { /* SSAT16 */
        unsigned bits = ((op >> 16) & 15) + 1;
        uint32_t lo = (uint32_t)rc_sat_s((int16_t)f0, bits, q), hi = (uint32_t)rc_sat_s((int16_t)(f0 >> 16), bits, q);
        return (lo & 0xFFFF) | hi << 16;
    }
    if ((op & 0x0FF00FF0u) == 0x06E00F30u) { /* USAT16 */
        unsigned bits = (op >> 16) & 15;
        return rc_sat_u((int16_t)f0, bits, q) | rc_sat_u((int16_t)(f0 >> 16), bits, q) << 16;
    }
    if ((op & 0x0FA00030u) == 0x06A00010u) { /* SSAT / USAT */
        unsigned sh = (op >> 7) & 31;
        int64_t v = (op & 0x40) ? (int64_t)((int32_t)f0 >> (sh ? sh : 31)) : (int64_t)(int32_t)(f0 << sh);
        if (op & (1u << 22))
            return rc_sat_u(v, (op >> 16) & 31, q);
        return (uint32_t)rc_sat_s(v, ((op >> 16) & 31) + 1, q);
    }
    if ((op & 0x0F800010u) == 0x07000010u) { /* signed multiplies: n = f0, m = f8, a = f12 (unless 15) */
        unsigned k = (op >> 20) & 7, o2 = (op >> 5) & 7;
        int acc = ((op >> 12) & 15) != 15;
        if (k == 0) { /* SMLAD / SMUAD (o2 0, 1), SMLSD / SMUSD (o2 2, 3); bit 5 swaps m's halves */
            uint32_t mm = (o2 & 1) ? rc_ror(f8, 16) : f8;
            int32_t p1 = (int32_t)(int16_t)f0 * (int16_t)mm, p2 = (int32_t)(int16_t)(f0 >> 16) * (int16_t)(mm >> 16);
            int32_t r;
            if (o2 & 2) {
                r = (int32_t)((uint32_t)p1 - (uint32_t)p2);
            } else {
                r = rc_qadd_flag(p1, p2, q);
            }
            return acc ? (uint32_t)rc_qadd_flag(r, (int32_t)f12, q) : (uint32_t)r;
        }
        if (k == 5) { /* SMMLA / SMMUL (o2 0, 1), SMMLS (o2 6, 7); bit 5 rounds */
            int64_t p = (int64_t)(int32_t)f0 * (int32_t)f8;
            int64_t a = acc || (o2 & 6) ? (int64_t)((uint64_t)f12 << 32) : 0;
            int64_t r = (o2 & 6) ? a - p : a + p;
            if (o2 & 1)
                r += 0x80000000ll;
            return (uint32_t)((uint64_t)r >> 32);
        }
        return 0;
    }
    if ((op & 0x0FF000F0u) == 0x07800010u) { /* USAD8 / USADA8 */
        uint32_t s = 0;
        for (int i = 0; i < 4; i++) {
            int d = (int)((f0 >> (8 * i)) & 0xFF) - (int)((f8 >> (8 * i)) & 0xFF);
            s += (uint32_t)(d < 0 ? -d : d);
        }
        return ((op >> 12) & 15) != 15 ? s + f12 : s;
    }
    return 0;
}

/* SMLALD / SMLSLD: RdHi:RdLo (fields 19-16:15-12) += the dual products of f0 and f8 (bit 5 swaps f8's halves). */
RC_INLINE uint64_t rc_media_long(uint32_t op, uint32_t lo, uint32_t hi, uint32_t f8, uint32_t f0)
{
    uint32_t mm = (op & 0x20) ? rc_ror(f8, 16) : f8;
    int64_t p1 = (int64_t)((int32_t)(int16_t)f0 * (int16_t)mm);
    int64_t p2 = (int64_t)((int32_t)(int16_t)(f0 >> 16) * (int16_t)(mm >> 16));
    uint64_t acc = (uint64_t)hi << 32 | lo;
    return acc + (uint64_t)((op & 0x40) ? p1 - p2 : p1 + p2);
}

/* ---- VFPv2 (ARM ARM C, VFP11) ----
 * Scalar only (FPSCR LEN = 0, as libctru sets it), round to nearest for arithmetic (the host's mode); flush to zero
 * and default NaN are not modelled (results with denormals or NaN payloads may differ from the hardware's). VMLA and
 * friends round twice (not fused): build with -ffp-contract=off. */

RC_INLINE float rc_fs(const ctr_cpu *c, unsigned i)
{
    float f;
    memcpy(&f, &c->vfp.s[i & 31], 4);
    return f;
}
RC_INLINE double rc_fd(const ctr_cpu *c, unsigned i)
{
    double d;
    memcpy(&d, &c->vfp.d[i & 15], 8);
    return d;
}
RC_INLINE void rc_set_fs(ctr_cpu *c, unsigned i, float f)
{
    memcpy(&c->vfp.s[i & 31], &f, 4);
}
RC_INLINE void rc_set_fd(ctr_cpu *c, unsigned i, double d)
{
    memcpy(&c->vfp.d[i & 15], &d, 8);
}

/* Float to integer with the ARM's saturation (NaN gives 0); rmode 0 nearest even, 1 +inf, 2 -inf, 3 toward zero. */
RC_INLINE uint32_t rc_vfp_to_int(double v, int sgn, unsigned rmode)
{
    if (v != v)
        return 0;
    double r;
    switch (rmode) {
    case 0: r = nearbyint(v); break; /* the host's default mode: to nearest, ties to even */
    case 1: r = ceil(v); break;
    case 2: r = floor(v); break;
    default: r = trunc(v); break;
    }
    if (sgn) {
        if (r >= 2147483647.0)
            return 0x7FFFFFFFu;
        if (r <= -2147483648.0)
            return 0x80000000u;
        return (uint32_t)(int32_t)r;
    }
    if (r >= 4294967295.0)
        return 0xFFFFFFFFu;
    if (r <= 0.0)
        return 0;
    return (uint32_t)r;
}

RC_INLINE void rc_vfp_compare(ctr_cpu *c, double a, double b)
{
    uint32_t f;
    if (a != a || b != b)
        f = 0x3; /* unordered: C V */
    else if (a == b)
        f = 0x6; /* Z C */
    else if (a < b)
        f = 0x8; /* N */
    else
        f = 0x2; /* C */
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | f << 28;
}

/* CDP to cp10 / cp11: the VFP data-processing instructions. */
RC_INLINE void rc_vfp_dp(ctr_cpu *c, uint32_t op)
{
    unsigned dbl = (op >> 8) & 1;
    unsigned vd = (op >> 12) & 15, vn = (op >> 16) & 15, vm = op & 15;
    unsigned D = (op >> 22) & 1, N = (op >> 7) & 1, M = (op >> 5) & 1;
    unsigned sd = vd << 1 | D, sn = vn << 1 | N, sm = vm << 1 | M;
    unsigned opc = ((op >> 23) & 1) << 3 | ((op >> 21) & 1) << 2 | ((op >> 20) & 1) << 1 | ((op >> 6) & 1); /* p q r s */
    if (opc != 15) {
        if (dbl) {
            double a = rc_fd(c, vn), b = rc_fd(c, vm), d = rc_fd(c, vd), p = a * b, r;
            switch (opc) {
            case 0: r = d + p; break;
            case 1: r = d - p; break;
            case 2: r = -d + p; break;
            case 3: r = -d - p; break;
            case 4: r = p; break;
            case 5: r = -p; break;
            case 6: r = a + b; break;
            case 7: r = a - b; break;
            case 8: r = a / b; break;
            default: return;
            }
            rc_set_fd(c, vd, r);
        } else {
            float a = rc_fs(c, sn), b = rc_fs(c, sm), d = rc_fs(c, sd), p = a * b, r;
            switch (opc) {
            case 0: r = d + p; break;
            case 1: r = d - p; break;
            case 2: r = -d + p; break;
            case 3: r = -d - p; break;
            case 4: r = p; break;
            case 5: r = -p; break;
            case 6: r = a + b; break;
            case 7: r = a - b; break;
            case 8: r = a / b; break;
            default: return;
            }
            rc_set_fs(c, sd, r);
        }
        return;
    }
    unsigned ext = vn << 1 | N; /* the extension opcode: Fn field and N */
    unsigned rmode = (c->fpscr >> 22) & 3;
    switch (ext) {
    case 0x00: /* FCPY */
        if (dbl)
            c->vfp.d[vd] = c->vfp.d[vm];
        else
            c->vfp.s[sd] = c->vfp.s[sm];
        return;
    case 0x01: /* FABS */
        if (dbl)
            c->vfp.d[vd] = c->vfp.d[vm] & ~(1ull << 63);
        else
            c->vfp.s[sd] = c->vfp.s[sm] & ~(1u << 31);
        return;
    case 0x02: /* FNEG */
        if (dbl)
            c->vfp.d[vd] = c->vfp.d[vm] ^ (1ull << 63);
        else
            c->vfp.s[sd] = c->vfp.s[sm] ^ (1u << 31);
        return;
    case 0x03: /* FSQRT */
        if (dbl)
            rc_set_fd(c, vd, sqrt(rc_fd(c, vm)));
        else
            rc_set_fs(c, sd, sqrtf(rc_fs(c, sm)));
        return;
    case 0x08: /* FCMP */
    case 0x09: /* FCMPE */
        if (dbl)
            rc_vfp_compare(c, rc_fd(c, vd), rc_fd(c, vm));
        else
            rc_vfp_compare(c, rc_fs(c, sd), rc_fs(c, sm));
        return;
    case 0x0A: /* FCMPZ */
    case 0x0B: /* FCMPEZ */
        rc_vfp_compare(c, dbl ? rc_fd(c, vd) : (double)rc_fs(c, sd), 0.0);
        return;
    case 0x0F: /* FCVTDS (single to double, cp10) / FCVTSD (double to single, cp11) */
        if (dbl)
            rc_set_fs(c, sd, (float)rc_fd(c, vm));
        else
            rc_set_fd(c, vd, (double)rc_fs(c, sm));
        return;
    case 0x10: /* FUITO: the integer is in Sm */
    case 0x11: /* FSITO */
    {
        uint32_t w = c->vfp.s[sm];
        if (dbl)
            rc_set_fd(c, vd, ext & 1 ? (double)(int32_t)w : (double)w);
        else
            rc_set_fs(c, sd, ext & 1 ? (float)(int32_t)w : (float)w);
        return;
    }
    case 0x18: /* FTOUI */
    case 0x19: /* FTOUIZ */
    case 0x1A: /* FTOSI */
    case 0x1B: /* FTOSIZ */
    {
        double v = dbl ? rc_fd(c, vm) : (double)rc_fs(c, sm);
        c->vfp.s[sd] = rc_vfp_to_int(v, ext >= 0x1A, (ext & 1) ? 3u : rmode);
        return;
    }
    default:
        return; /* not VFPv2: the interpreter refuses it before calling (ctr_vfp_dp_ok) */
    }
}

/* VFPv2 data-processing encodings rc_vfp_dp implements. */
RC_INLINE int rc_vfp_dp_ok(uint32_t op)
{
    unsigned opc = ((op >> 23) & 1) << 3 | ((op >> 21) & 1) << 2 | ((op >> 20) & 1) << 1 | ((op >> 6) & 1);
    if (op & 0x10)
        return 0;
    if (opc <= 8)
        return 1;
    if (opc != 15)
        return 0;
    unsigned ext = ((op >> 16) & 15) << 1 | ((op >> 7) & 1);
    switch (ext) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0F:
    case 0x10: case 0x11: case 0x18: case 0x19: case 0x1A: case 0x1B:
        return 1;
    default:
        return 0;
    }
}

/* LDC / STC to cp10 / cp11: VLDR / VSTR / VLDM / VSTM. base: the value of Rn (PC already aligned + 8). Returns Rn
 * after write-back (the same value without W). */
RC_INLINE uint32_t rc_vfp_ls(ctr_cpu *c, uint8_t *mem, uint32_t op, uint32_t base)
{
    unsigned p = (op >> 24) & 1, u = (op >> 23) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    unsigned dbl = (op >> 8) & 1, imm = op & 0xFF;
    unsigned first = dbl ? ((op >> 12) & 15) * 2 : (((op >> 12) & 15) << 1 | ((op >> 22) & 1));
    uint32_t words = p && !w ? (dbl ? 2u : 1u) : imm;   /* VLDR / VSTR: one register */
    uint32_t addr = p && !w ? (u ? base + imm * 4 : base - imm * 4) : (u ? base : base - imm * 4);
    if (dbl && !(p && !w))
        words &= ~1u; /* FLDMX / FSTMX: the odd word is a format word, not transferred */
    for (uint32_t i = 0; i < words; i++) {
        unsigned s = (first + i) & 31;
        if (l)
            c->vfp.s[s] = ctr_rd32(mem, addr + 4 * i);
        else
            ctr_wr32(mem, addr + 4 * i, c->vfp.s[s]);
    }
    if (p && !w)
        return base;
    if (!w)
        return base;
    return u ? base + imm * 4 : base - imm * 4;
}

/* MRC from cp10 / cp11: FMRS (VMOV Rt, Sn), FMRDL / FMRDH (low / high word of Dn), FMRX (VMRS Rt, FPSCR / FPEXC /
 * FPSID). Rt = 15 with FPSCR is VMRS APSR_nzcv: the lifter handles it. */
RC_INLINE uint32_t rc_vfp_mrc(const ctr_cpu *c, uint32_t op)
{
    unsigned opc = (op >> 21) & 7, vn = (op >> 16) & 15;
    if (opc == 7) {
        switch (vn) {
        case 0: return 0x410120B4u; /* FPSID: ARM11 MPCore VFP11 */
        case 1: return c->fpscr;
        case 8: return c->fpexc;
        default: return 0;
        }
    }
    if ((op >> 8) & 1) /* cp11: FMRDL / FMRDH */
        return opc & 1 ? (uint32_t)(c->vfp.d[vn] >> 32) : (uint32_t)c->vfp.d[vn];
    return c->vfp.s[vn << 1 | ((op >> 7) & 1)];
}

/* MCR to cp10 / cp11: FMSR, FMDLR / FMDHR, FMXR. */
RC_INLINE void rc_vfp_mcr(ctr_cpu *c, uint32_t op, uint32_t v)
{
    unsigned opc = (op >> 21) & 7, vn = (op >> 16) & 15;
    if (opc == 7) {
        if (vn == 1)
            c->fpscr = v;
        else if (vn == 8)
            c->fpexc = v;
        return;
    }
    if ((op >> 8) & 1) {
        if (opc & 1)
            c->vfp.d[vn] = (c->vfp.d[vn] & 0xFFFFFFFFull) | (uint64_t)v << 32;
        else
            c->vfp.d[vn] = (c->vfp.d[vn] & ~0xFFFFFFFFull) | v;
        return;
    }
    c->vfp.s[vn << 1 | ((op >> 7) & 1)] = v;
}

/* MCRR / MRRC to cp10 / cp11: two core registers to / from Sm, Sm+1 (cp10) or Dm (cp11). */
RC_INLINE void rc_vfp_mcrr(ctr_cpu *c, uint32_t op, uint32_t lo, uint32_t hi)
{
    if ((op >> 8) & 1) {
        c->vfp.d[op & 15] = (uint64_t)hi << 32 | lo;
    } else {
        unsigned s = (op & 15) << 1 | ((op >> 5) & 1);
        c->vfp.s[s] = lo;
        c->vfp.s[(s + 1) & 31] = hi;
    }
}

RC_INLINE uint64_t rc_vfp_mrrc(const ctr_cpu *c, uint32_t op)
{
    if ((op >> 8) & 1)
        return c->vfp.d[op & 15];
    unsigned s = (op & 15) << 1 | ((op >> 5) & 1);
    return (uint64_t)c->vfp.s[(s + 1) & 31] << 32 | c->vfp.s[s];
}

/* ---- the shape of a lifted function (gba_tools' lifter, nds_tools' and ctr_tools' subclasses) ---- */

#define RC_ENTER(T, REGION)                                                                                          \
    ctr_rc_head *const h = (ctr_rc_head *)(void *)m_;                                                                \
    enum { RC_T = (T), RC_PIPE = (T) ? 4 : 8 };                                                                      \
    uint8_t *const mem = h->mem;                                                                                     \
    uint32_t r0 = h->cpu.r[0], r1 = h->cpu.r[1], r2 = h->cpu.r[2], r3 = h->cpu.r[3], r4 = h->cpu.r[4];             \
    uint32_t r5 = h->cpu.r[5], r6 = h->cpu.r[6], r7 = h->cpu.r[7], r8 = h->cpu.r[8], r9 = h->cpu.r[9];             \
    uint32_t r10 = h->cpu.r[10], r11 = h->cpu.r[11], r12 = h->cpu.r[12], r13 = h->cpu.r[13], r14 = h->cpu.r[14];    \
    uint32_t fN = h->cpu.n, fZ = h->cpu.z, fC = h->cpu.c, fV = h->cpu.v;                                             \
    uint64_t now = h->now, ne = h->next_event;                                                                       \
    const uint32_t fb = 1;                                                                                           \
    uint32_t epc = 0;                                                                                                \
    (void)(REGION);                                                                                                  \
    (void)mem

#define RC_PC() (h->cpu.r[15] - RC_PIPE)
#define RC_M ((ctr_machine *)(void *)h)

#ifdef CTR_RC_COUNT
#define RC_COUNT() (h->rc_native++)
#else
#define RC_COUNT() ((void)0)
#endif

#define RC_I(A)                                                                                                      \
    do {                                                                                                             \
        now += 1;                                                                                                    \
        RC_COUNT();                                                                                                  \
    } while (0)

#define RC_STOP(A, NFC, EXTRA, N)                                                                                    \
    do {                                                                                                             \
        if (__builtin_expect(now + (uint32_t)(NFC) * fb + (uint32_t)(EXTRA) >= ne, 0)) {                             \
            epc = (A);                                                                                               \
            if (now >= ne)                                                                                           \
                goto x_exit;                                                                                         \
            h->rc_hint = (N);                                                                                        \
            goto x_interp;                                                                                           \
        }                                                                                                            \
    } while (0)

#define RC_CHECK(A, NFC, EXTRA, N)                                                                                   \
    do {                                                                                                             \
        if (__builtin_expect(now + (uint32_t)(NFC) * fb + (uint32_t)(EXTRA) >= ne, 0)) {                             \
            epc = (A);                                                                                               \
            h->rc_hint = (N);                                                                                        \
            goto x_interp;                                                                                           \
        }                                                                                                            \
    } while (0)

#define RC_MID(N)                                                                                                    \
    do {                                                                                                             \
        h->rc_hint = (N);                                                                                            \
        epc = RC_PC();                                                                                               \
        goto x_interp;                                                                                               \
    } while (0)

#define RC_EXIT(A)                                                                                                   \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_exit;                                                                                                 \
    } while (0)

#define RC_INTERP(A)                                                                                                 \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_interp;                                                                                               \
    } while (0)

#define RC_MID_TABLE(LO, TAB)                                                                                        \
    do {                                                                                                             \
        uint32_t o__ = RC_PC() - (LO), i__ = o__ >> (RC_T ? 1 : 2);                                                  \
        if (!(o__ & (RC_T ? 1u : 3u)) && i__ < sizeof(TAB) && (TAB)[i__])                                            \
            RC_MID((TAB)[i__]);                                                                                      \
    } while (0)

#define RC_NOT_COVERED()                                                                                             \
    do {                                                                                                             \
        h->rc_exit = CTR_RC_EXIT_NOT_COVERED;                                                                        \
        return;                                                                                                      \
    } while (0)

/* Memory: flat, so no bus calls and nothing to sync. SEQ and OB (the GBA's sequential flag and open bus) unused. */
#define RD8(A, SEQ, OB) ctr_rd8(mem, (A))
#define RD16(A, SEQ, OB) ctr_rd16(mem, (A))
#define RD32(A, SEQ, OB) ctr_rd32(mem, (A))
#define WR8(A, V, SEQ, OB) ctr_wr8(mem, (A), (V))
#define WR16(A, V, SEQ, OB) ctr_wr16(mem, (A), (V))
#define WR32(A, V, SEQ, OB) ctr_wr32(mem, (A), (V))

#define RC_RUN_RAM(A, N) 1
#define RC_RUN_WAITS(A, N) ((void)0)
#define RC_RUN_BASE(A) ((void)0)
#define RC_FRD32(A) ctr_rd32(mem, (A))
#define RC_FWR32(A, V) ctr_wr32(mem, (A), (V))
#define RC_BRD32(A, SEQ, OB) ctr_rd32(mem, (A))
#define RC_BWR32(A, V, SEQ, OB) ctr_wr32(mem, (A), (V))

#define RC_NEXTRA() 0u
#define RC_REFILL(R15) ((void)0)
#define RC_LIT(W) ((uint32_t)(W))

#define RC_JUMP_DYN(T_)                                                                                              \
    do {                                                                                                             \
        uint32_t t__ = (T_) & (RC_T ? ~1u : ~3u);                                                                     \
        RC_EXIT(t__);                                                                                                 \
    } while (0)
#define RC_JUMP_LOCAL(A, LABEL) goto LABEL

#define RC_BX(T_)                                                                                                    \
    do {                                                                                                             \
        uint32_t t__ = (T_);                                                                                          \
        if (t__ & 1) {                                                                                                \
            h->cpu.cpsr_ctl |= 0x20u;                                                                                \
            h->cpu.r[15] = (t__ & ~1u) + 4;                                                                           \
        } else {                                                                                                     \
            h->cpu.cpsr_ctl &= ~0x20u;                                                                               \
            h->cpu.r[15] = (t__ & ~3u) + 8;                                                                           \
        }                                                                                                            \
        goto x_flush;                                                                                                \
    } while (0)

#define RC_EPILOGUE()                                                                                                \
    x_interp: __attribute__((unused));                                                                               \
    h->rc_exit = CTR_RC_EXIT_INTERP;                                                                                 \
    x_exit: __attribute__((unused));                                                                                 \
    h->cpu.r[15] = epc + RC_PIPE;                                                                                    \
    x_flush: __attribute__((unused));                                                                                \
    h->cpu.r[0] = r0; h->cpu.r[1] = r1; h->cpu.r[2] = r2; h->cpu.r[3] = r3; h->cpu.r[4] = r4;                     \
    h->cpu.r[5] = r5; h->cpu.r[6] = r6; h->cpu.r[7] = r7; h->cpu.r[8] = r8; h->cpu.r[9] = r9;                     \
    h->cpu.r[10] = r10; h->cpu.r[11] = r11; h->cpu.r[12] = r12; h->cpu.r[13] = r13; h->cpu.r[14] = r14;            \
    h->cpu.n = fN; h->cpu.z = fZ; h->cpu.c = fC; h->cpu.v = fV;                                                     \
    h->now = now

#define RC_FN __attribute__((visibility("hidden"))) void
#define RC_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
}
#endif

#endif
