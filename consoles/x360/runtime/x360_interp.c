/* x360_interp.c -- the Xenon interpreter (XBA-270, a fallback for code the pack does not hold since XBA-283).
 *
 * Two jobs:
 * - RC_INTERP: the lifter hands a few instructions to the runtime (traps, which MSVC emits for asserts, divide-by-zero
 *   checks and debug breaks; the FPSCR moves; mcrxr; FP record forms; sc). x360_interp_one runs one, then execution goes
 *   back to lifted code at the next instruction.
 * - Missed code (XBA-283): when the dispatcher reaches an address no lifted function holds, x360_interp_run runs the
 *   code there until it reaches lifted code again (a call into the pack, a return to a lifted caller), so one run
 *   goes on past every miss and records them all; the next Prepare lifts them (discovered.txt), as the Xbox and
 *   GameCube self-healing loops do. Only the entry of missed code is recorded: the dispatcher's address, unless it
 *   is the return address of a call made by interpreted code (x360_interp_ret), and the targets of calls the
 *   interpreted code makes (bl / bcl / bctrl / bclrl, and bctr when no bounds check before it says it is a switch)
 *   that the pack does not hold.
 *
 * Semantics are the lifter's (tools/ppc/lift.py with consoles/x360/tools/x360_tools/xenon.py): titles run in 32-bit
 * mode (MSR[SF] = 0), record forms set CR0 from the low word, XER[CA] is the carry out of the low word,
 * CTR-decrementing branches test the low word of CTR, effective addresses are 32-bit; the helpers are those of
 * ppc_ops.h / xenon_ops.h / xenon_vmx.h lifted code calls. The integer, branch, load / store, FPU and system
 * instructions a title runs in user mode are here; VMX / VMX128 come from x360_interp_vmx.inc. Anything else stops
 * the title with the word and its address. */
#include <fenv.h>
#include <stdio.h>
#include <string.h>

#include "x360_internal.h"

static int trap_cond(unsigned to, int64_t a, int64_t b)
{
    return ((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) ||
           ((to & 2) && (uint64_t)a < (uint64_t)b) || ((to & 1) && (uint64_t)a > (uint64_t)b);
}

static void set_rounding(uint32_t fpscr)
{
    static const int modes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    fesetround(modes[fpscr & 3]);
}

static double u2d(uint64_t v)
{
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static uint64_t d2u(double d)
{
    uint64_t v;
    memcpy(&v, &d, 8);
    return v;
}

static uint32_t mask32(unsigned mb, unsigned me)
{
    uint32_t a = 0xFFFFFFFFu >> mb, b = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (a & b) : (a | b);
}

static uint64_t mask64(unsigned mb, unsigned me)
{
    uint64_t a = ~0ull >> mb, b = me == 63 ? ~0ull : ~(~0ull >> (me + 1));
    return mb <= me ? (a & b) : (a | b);
}

#define M_ (&x->m)
#define R(n) c->gpr[n]
#define RA0(n) ((n) ? c->gpr[n] : 0)
#define CR0(v) (c->cr = ppc_cr_set(c->cr, 0, ppc_cmps((int32_t)(uint32_t)(v), 0, c->xer_so)))
#define FSET_S(d, v) (c->fpr[d] = ppc_single(v))

/* bc / bclr / bcctr: the condition (and the CTR decrement, except for bcctr) */
static int bcond(xe_cpu *c, unsigned bo, unsigned bi, int ctr_ok)
{
    int ok = 1;
    if (!(bo & 4) && ctr_ok) {
        c->ctr--;
        ok = (bo & 2) ? (uint32_t)c->ctr == 0 : (uint32_t)c->ctr != 0;
    }
    if (!(bo & 16)) {
        unsigned bit = (c->cr >> (31 - bi)) & 1;
        ok = ok && bit == ((bo >> 3) & 1);
    }
    return ok;
}

#include "x360_interp_vmx.inc"

/* OE forms (XBA-284): the overflow of an XO-form operation on its operands (ci: XER[CA] before it), -1 for those with
 * no OE form (mulhw, mulhwu, mulhd, mulhdu: the bit is ignored). Worked out on the exact values, apart from the
 * lifter's sign rule (xenon_ops.h xe_ov_add): 32-bit mode, the add / subtract family overflows on the low word. */
static int oe_overflow(unsigned xo9, uint64_t a, uint64_t b, unsigned ci)
{
    int64_t x = (int32_t)(uint32_t)a, y = (int32_t)(uint32_t)b, s;
    switch (xo9) {
    case 266: s = x + y; break;          /* add */
    case 10: s = x + y; break;           /* addc */
    case 138: s = x + y + ci; break;     /* adde */
    case 202: s = x + ci; break;         /* addze */
    case 234: s = x - 1 + ci; break;     /* addme */
    case 40: s = y - x; break;           /* subf */
    case 8: s = y - x; break;            /* subfc */
    case 136: s = y - x - 1 + ci; break; /* subfe */
    case 200: s = -x - 1 + ci; break;    /* subfze */
    case 232: s = -x - 2 + ci; break;    /* subfme */
    case 104: s = -x; break;             /* neg */
    case 235: s = x * y; break;          /* mullw */
    case 491: return y == 0 || (x == INT32_MIN && y == -1); /* divw */
    case 459: return (uint32_t)b == 0;   /* divwu */
    case 233: {                          /* mulld */
        __int128 p = (__int128)(int64_t)a * (int64_t)b;
        return p != (__int128)(int64_t)p;
    }
    case 489: return b == 0 || ((int64_t)a == INT64_MIN && (int64_t)b == -1); /* divd */
    case 457: return b == 0;             /* divdu */
    default: return -1;
    }
    return s < INT32_MIN || s > INT32_MAX;
}

/* 1: the instruction is not one this interpreter knows (the caller reports it) */
static int exec(x360_machine *x, xe_cpu *c, uint32_t pc, uint32_t w)
{
    unsigned op = w >> 26, rd = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31, rc = (w >> 6) & 31;
    unsigned xo = (w >> 1) & 0x3FF, rec = w & 1;
    int64_t simm = (int16_t)(w & 0xFFFF);
    uint64_t uimm = w & 0xFFFF;
    uint32_t next = pc + 4;
    int trap = -1;
    switch (op) {
    case 2: /* tdi */
        trap = trap_cond(rd, (int64_t)R(ra), simm);
        break;
    case 3: /* twi */
        trap = trap_cond(rd, (int32_t)R(ra), simm);
        break;
    case 4:
    case 5:
    case 6:
        if (vmx_exec(x, c, w))
            return 1;
        break;
    case 7: /* mulli */
        R(rd) = (uint64_t)((int64_t)R(ra) * simm);
        break;
    case 8: { /* subfic */
        uint64_t a = R(ra);
        R(rd) = ~a + (uint64_t)simm + 1u;
        c->xer_ca = xe_carry(~a, (uint64_t)simm, 1);
        break;
    }
    case 10: /* cmpli */
        if (rd & 1)
            c->cr = ppc_cr_set(c->cr, rd >> 2, ppc_cmpu64(R(ra), uimm, c->xer_so));
        else
            c->cr = ppc_cr_set(c->cr, rd >> 2, ppc_cmpu((uint32_t)R(ra), (uint32_t)uimm, c->xer_so));
        break;
    case 11: /* cmpi */
        if (rd & 1)
            c->cr = ppc_cr_set(c->cr, rd >> 2, ppc_cmps64((int64_t)R(ra), simm, c->xer_so));
        else
            c->cr = ppc_cr_set(c->cr, rd >> 2, ppc_cmps((int32_t)R(ra), (int32_t)simm, c->xer_so));
        break;
    case 12:
    case 13: { /* addic, addic. */
        uint64_t a = R(ra);
        R(rd) = a + (uint64_t)simm;
        c->xer_ca = xe_carry(a, (uint64_t)simm, 0);
        if (op == 13)
            CR0(R(rd));
        break;
    }
    case 14: /* addi */
        R(rd) = RA0(ra) + (uint64_t)simm;
        break;
    case 15: /* addis */
        R(rd) = RA0(ra) + (uint64_t)(simm << 16);
        break;
    case 16: { /* bc */
        uint32_t t = (uint32_t)(int32_t)(int16_t)(w & 0xFFFC) + ((w & 2) ? 0 : pc);
        int ok = bcond(c, rd, ra, 1);
        if (w & 1)
            c->lr = pc + 4;
        if (ok)
            next = t;
        break;
    }
    case 17:
        x360_fail(x, "sc at %08X: system calls are the kernel's, not a title's", pc);
        return -1;
    case 18: { /* b */
        uint32_t li = w & 0x03FFFFFCu;
        if (li & 0x02000000u)
            li |= 0xFC000000u;
        if (w & 1)
            c->lr = pc + 4;
        next = li + ((w & 2) ? 0 : pc);
        break;
    }
    case 19:
        switch (xo) {
        case 0: /* mcrf */
            c->cr = ppc_cr_set(c->cr, rd >> 2, c->cr >> (28 - 4 * (ra >> 2)));
            break;
        case 16: { /* bclr */
            uint32_t t = (uint32_t)c->lr & ~3u;
            int ok = bcond(c, rd, ra, 1);
            if (w & 1)
                c->lr = pc + 4;
            if (ok)
                next = t;
            break;
        }
        case 528: { /* bcctr */
            uint32_t t = (uint32_t)c->ctr & ~3u;
            int ok = bcond(c, rd, ra, 0);
            if (w & 1)
                c->lr = pc + 4;
            if (ok)
                next = t;
            break;
        }
        case 150: /* isync */
            break;
        case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449: {
            unsigned a = ppc_cr_bit(c->cr, ra), b = ppc_cr_bit(c->cr, rb), v;
            switch (xo) {
            case 33: v = !(a | b); break;
            case 129: v = a & !b; break;
            case 193: v = a ^ b; break;
            case 225: v = !(a & b); break;
            case 257: v = a & b; break;
            case 289: v = !(a ^ b); break;
            case 417: v = a | !b; break;
            default: v = a | b; break;
            }
            c->cr = ppc_cr_setbit(c->cr, rd, v);
            break;
        }
        default:
            return 1;
        }
        break;
    case 20: { /* rlwimi */
        unsigned mb = rc, me = (w >> 1) & 31;
        uint32_t m = mask32(mb, me), r = ppc_rotl((uint32_t)R(rd), rb);
        uint64_t m64 = mb <= me ? m : (0xFFFFFFFF00000000ull | m), v = ((uint64_t)r << 32) | r;
        R(ra) = (v & m64) | (R(ra) & ~m64);
        if (rec)
            CR0(R(ra));
        break;
    }
    case 21:
    case 23: { /* rlwinm, rlwnm */
        unsigned mb = rc, me = (w >> 1) & 31, sh = op == 21 ? rb : (unsigned)(R(rb) & 31);
        uint32_t m = mask32(mb, me), r = ppc_rotl((uint32_t)R(rd), sh);
        R(ra) = mb <= me ? (uint64_t)(r & m) : (((uint64_t)r << 32) | (r & m));
        if (rec)
            CR0(R(ra));
        break;
    }
    case 24: R(ra) = R(rd) | uimm; break;
    case 25: R(ra) = R(rd) | (uimm << 16); break;
    case 26: R(ra) = R(rd) ^ uimm; break;
    case 27: R(ra) = R(rd) ^ (uimm << 16); break;
    case 28: R(ra) = R(rd) & uimm; CR0(R(ra)); break;
    case 29: R(ra) = R(rd) & (uimm << 16); CR0(R(ra)); break;
    case 30: { /* rld* */
        unsigned sh = rb | ((w >> 1) & 1) << 5, mb = rc | ((w >> 5) & 1) << 5;
        uint64_t s = R(rd), r;
        switch ((w >> 2) & 7) {
        case 0: r = ppc_rotl64(s, sh) & mask64(mb, 63); break;              /* rldicl */
        case 1: r = ppc_rotl64(s, sh) & mask64(0, mb); break;               /* rldicr */
        case 2: r = ppc_rotl64(s, sh) & mask64(mb, 63 - sh); break;         /* rldic */
        case 3: {                                                           /* rldimi */
            uint64_t m = mask64(mb, 63 - sh);
            r = (ppc_rotl64(s, sh) & m) | (R(ra) & ~m);
            break;
        }
        case 4:
            if (((w >> 1) & 15) == 8)
                r = ppc_rotl64(s, (unsigned)(R(rb) & 63)) & mask64(mb, 63); /* rldcl */
            else if (((w >> 1) & 15) == 9)
                r = ppc_rotl64(s, (unsigned)(R(rb) & 63)) & mask64(0, mb);  /* rldcr */
            else
                return 1;
            break;
        default:
            return 1;
        }
        R(ra) = r;
        if (rec)
            CR0(r);
        break;
    }
    case 31: {
        uint64_t a = R(ra), b = R(rb);
        uint32_t ea = (uint32_t)(RA0(ra) + b), eau = (uint32_t)(a + b);
        unsigned oe = (w >> 10) & 1, ci0 = c->xer_ca;
        switch (xo & 0x1FF) { /* XO-form arithmetic (OE: OV / SO after xo_rec) */
        case 266: R(rd) = a + b; goto xo_rec;
        case 40: R(rd) = b - a; goto xo_rec;
        case 10: R(rd) = a + b; c->xer_ca = xe_carry(a, b, 0); goto xo_rec;
        case 8: R(rd) = ~a + b + 1u; c->xer_ca = xe_carry(~a, b, 1); goto xo_rec;
        case 138: { uint32_t ci = c->xer_ca; R(rd) = a + b + ci; c->xer_ca = xe_carry(a, b, ci); goto xo_rec; }
        case 136: { uint32_t ci = c->xer_ca; R(rd) = ~a + b + ci; c->xer_ca = xe_carry(~a, b, ci); goto xo_rec; }
        case 202: { uint32_t ci = c->xer_ca; R(rd) = a + ci; c->xer_ca = xe_carry(a, 0, ci); goto xo_rec; }
        case 200: { uint32_t ci = c->xer_ca; R(rd) = ~a + ci; c->xer_ca = xe_carry(~a, 0, ci); goto xo_rec; }
        case 234: { uint32_t ci = c->xer_ca; R(rd) = a - 1u + ci; c->xer_ca = xe_carry(a, ~0ull, ci); goto xo_rec; }
        case 232: { uint32_t ci = c->xer_ca; R(rd) = ~a - 1u + ci; c->xer_ca = xe_carry(~a, ~0ull, ci); goto xo_rec; }
        case 104: R(rd) = 0u - a; goto xo_rec;
        case 235: R(rd) = (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b); goto xo_rec;
        case 75: R(rd) = (uint32_t)ppc_mulhw((uint32_t)a, (uint32_t)b); goto xo_rec;
        case 11: R(rd) = (uint32_t)ppc_mulhwu((uint32_t)a, (uint32_t)b); goto xo_rec;
        case 491: R(rd) = (uint32_t)ppc_divw((uint32_t)a, (uint32_t)b); goto xo_rec;
        case 459: R(rd) = (uint32_t)ppc_divwu((uint32_t)a, (uint32_t)b); goto xo_rec;
        case 233: R(rd) = a * b; goto xo_rec;
        case 73: R(rd) = ppc_mulhd(a, b); goto xo_rec;
        case 9: R(rd) = ppc_mulhdu(a, b); goto xo_rec;
        case 489: R(rd) = ppc_divd(a, b); goto xo_rec;
        case 457: R(rd) = ppc_divdu(a, b); goto xo_rec;
        default: break;
        }
        switch (xo) {
        case 0: /* cmp */
            if (rd & 1)
                c->cr = ppc_cr_set(c->cr, rd >> 2, ppc_cmps64((int64_t)a, (int64_t)b, c->xer_so));
            else
                c->cr = ppc_cr_set(c->cr, rd >> 2, ppc_cmps((int32_t)a, (int32_t)b, c->xer_so));
            break;
        case 32: /* cmpl */
            if (rd & 1)
                c->cr = ppc_cr_set(c->cr, rd >> 2, ppc_cmpu64(a, b, c->xer_so));
            else
                c->cr = ppc_cr_set(c->cr, rd >> 2, ppc_cmpu((uint32_t)a, (uint32_t)b, c->xer_so));
            break;
        case 4: /* tw */
            trap = trap_cond(rd, (int32_t)a, (int32_t)b);
            break;
        case 68: /* td */
            trap = trap_cond(rd, (int64_t)a, (int64_t)b);
            break;
        /* logic and shifts: rA = f(rS, rB) */
        case 28: R(ra) = R(rd) & b; goto x_rec;
        case 60: R(ra) = R(rd) & ~b; goto x_rec;
        case 444:
            if (rd == ra && ra == rb && (rd == 1 || rd == 31) && !rec && x->m.slice > 0) {
                x->m.rc_native += XE_SPIN_COST; /* a spin-wait's priority hint: as xenon_rt.h XE_SPIN_HINT */
                x->m.slice = 0;
            }
            R(ra) = R(rd) | b;
            goto x_rec;
        case 412: R(ra) = R(rd) | ~b; goto x_rec;
        case 316: R(ra) = R(rd) ^ b; goto x_rec;
        case 124: R(ra) = ~(R(rd) | b); goto x_rec;
        case 476: R(ra) = ~(R(rd) & b); goto x_rec;
        case 284: R(ra) = ~(R(rd) ^ b); goto x_rec;
        case 24: R(ra) = ppc_slw((uint32_t)R(rd), (uint32_t)b); goto x_rec;
        case 536: R(ra) = ppc_srw((uint32_t)R(rd), (uint32_t)b); goto x_rec;
        case 792: R(ra) = (uint64_t)(int64_t)(int32_t)ppc_sraw((uint32_t)R(rd), (uint32_t)b, &c->xer_ca); goto x_rec;
        case 824: R(ra) = (uint64_t)(int64_t)(int32_t)ppc_srawi((uint32_t)R(rd), rb, &c->xer_ca); goto x_rec;
        case 26: R(ra) = ppc_cntlzw((uint32_t)R(rd)); goto x_rec;
        case 922: R(ra) = (uint64_t)(int64_t)(int16_t)R(rd); goto x_rec;
        case 954: R(ra) = (uint64_t)(int64_t)(int8_t)R(rd); goto x_rec;
        case 986: R(ra) = (uint64_t)(int64_t)(int32_t)R(rd); goto x_rec;
        case 58: R(ra) = ppc_cntlzd(R(rd)); goto x_rec;
        case 27: R(ra) = ppc_sld(R(rd), b); goto x_rec;
        case 539: R(ra) = ppc_srd(R(rd), b); goto x_rec;
        case 794: R(ra) = ppc_srad(R(rd), b, &c->xer_ca); goto x_rec;
        case 826:
        case 827: /* sradi */
            R(ra) = ppc_sradi(R(rd), rb | (xo & 1) << 5, &c->xer_ca);
            goto x_rec;
        /* loads and stores */
        case 23: R(rd) = xe_lwz(M_, ea); break;
        case 87: R(rd) = xe_lbz(M_, ea); break;
        case 279: R(rd) = xe_lhz(M_, ea); break;
        case 343: R(rd) = (uint64_t)(int64_t)(int16_t)xe_lhz(M_, ea); break;
        case 21: R(rd) = xe_ld(M_, ea); break;
        case 341: R(rd) = (uint64_t)(int64_t)(int32_t)xe_lwz(M_, ea); break;
        case 55: R(rd) = xe_lwz(M_, eau); R(ra) = eau; break;
        case 119: R(rd) = xe_lbz(M_, eau); R(ra) = eau; break;
        case 311: R(rd) = xe_lhz(M_, eau); R(ra) = eau; break;
        case 375: R(rd) = (uint64_t)(int64_t)(int16_t)xe_lhz(M_, eau); R(ra) = eau; break;
        case 53: R(rd) = xe_ld(M_, eau); R(ra) = eau; break;
        case 373: R(rd) = (uint64_t)(int64_t)(int32_t)xe_lwz(M_, eau); R(ra) = eau; break;
        case 151: xe_stw(M_, ea, (uint32_t)R(rd)); break;
        case 215: xe_stb(M_, ea, (uint32_t)R(rd)); break;
        case 407: xe_sth(M_, ea, (uint32_t)R(rd)); break;
        case 149: xe_std(M_, ea, R(rd)); break;
        case 183: xe_stw(M_, eau, (uint32_t)R(rd)); R(ra) = eau; break;
        case 247: xe_stb(M_, eau, (uint32_t)R(rd)); R(ra) = eau; break;
        case 439: xe_sth(M_, eau, (uint32_t)R(rd)); R(ra) = eau; break;
        case 181: xe_std(M_, eau, R(rd)); R(ra) = eau; break;
        case 534: R(rd) = __builtin_bswap32(xe_lwz(M_, ea)); break;
        case 790: R(rd) = __builtin_bswap16((uint16_t)xe_lhz(M_, ea)); break;
        case 532: R(rd) = __builtin_bswap64(xe_ld(M_, ea)); break;
        case 662: xe_stw(M_, ea, __builtin_bswap32((uint32_t)R(rd))); break;
        case 918: xe_sth(M_, ea, __builtin_bswap16((uint16_t)R(rd))); break;
        case 660: xe_std(M_, ea, __builtin_bswap64(R(rd))); break;
        case 20: R(rd) = xe_lwarx(M_, c, ea); break;              /* lwarx (xenon_rt.h) */
        case 84: R(rd) = xe_ldarx(M_, c, ea); break;              /* ldarx */
        case 150: xe_stcx(M_, c, ea, (uint32_t)R(rd), 4); break;  /* stwcx. */
        case 214: xe_stcx(M_, c, ea, R(rd), 8); break;            /* stdcx. */
        case 535: FSET_S(rd, ppc_lfs_bits(xe_lwz(M_, ea))); break;
        case 567: FSET_S(rd, ppc_lfs_bits(xe_lwz(M_, eau))); R(ra) = eau; break;
        case 599: c->fpr[rd] = ppc_u2f(xe_ld(M_, ea)); break;
        case 631: c->fpr[rd] = ppc_u2f(xe_ld(M_, eau)); R(ra) = eau; break;
        case 663: xe_stw(M_, ea, ppc_stfs_bits(c->fpr[rd])); break;
        case 695: xe_stw(M_, eau, ppc_stfs_bits(c->fpr[rd])); R(ra) = eau; break;
        case 727: xe_std(M_, ea, ppc_f2u(c->fpr[rd])); break;
        case 759: xe_std(M_, eau, ppc_f2u(c->fpr[rd])); R(ra) = eau; break;
        case 983: xe_stw(M_, ea, (uint32_t)ppc_f2u(c->fpr[rd])); break;
        case 1014: xe_zero(M_, ea, rd == 1 ? 128 : 32); break; /* dcbz, dcbz128 */
        case 598: case 854: /* sync / lwsync, eieio: a host fence, as lifted code (RC_SYNC) */
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            break;
        case 54: case 86: case 246: case 278: case 470: case 982: /* cache hints, icbi */
            break;
        /* system registers */
        case 19: R(rd) = c->cr; break; /* mfcr (mfocrf reads the whole CR too) */
        case 144: { /* mtcrf */
            unsigned crm = (w >> 12) & 0xFF;
            uint32_t m = 0;
            for (int k = 0; k < 8; k++)
                if (crm & (0x80 >> k))
                    m |= 0xF0000000u >> (4 * k);
            c->cr = (c->cr & ~m) | ((uint32_t)R(rd) & m);
            break;
        }
        case 83: R(rd) = c->msr; break;
        case 146:
        case 178: /* mtmsr, mtmsrd: EE and RI (xenon.py op_mtmsrd) */
            c->msr = ((uint32_t)R(rd) & 0x8002u) | (c->msr & ~0x8002u);
            break;
        case 339:
        case 371: { /* mfspr, mftb */
            unsigned spr = ra | rb << 5;
            R(rd) = spr == 8 ? c->lr : spr == 9 ? c->ctr : spr == 1 ? (uint64_t)RC_XER() : xe_mfspr(M_, spr);
            break;
        }
        case 467: { /* mtspr */
            unsigned spr = ra | rb << 5;
            if (spr == 8)
                c->lr = R(rd);
            else if (spr == 9)
                c->ctr = R(rd);
            else if (spr == 1)
                RC_SET_XER(R(rd));
            else
                xe_mtspr(M_, spr, R(rd));
            break;
        }
        case 512: { /* mcrxr */
            uint32_t f = c->xer_so << 3 | c->xer_ov << 2 | c->xer_ca << 1;
            c->cr = (c->cr & ~(0xF0000000u >> (4 * (rd >> 2)))) | (f << (28 - 4 * (rd >> 2)));
            c->xer_so = c->xer_ov = c->xer_ca = 0;
            break;
        }
        default:
            if (vmx_exec(x, c, w))
                return 1;
            break;
        }
        break;
    xo_rec:
        if (oe) {
            int ov = oe_overflow(xo & 0x1FF, a, b, ci0);
            if (ov >= 0) {
                c->xer_ov = (uint32_t)ov;
                c->xer_so |= c->xer_ov;
            }
        }
        if (rec)
            CR0(R(rd));
        break;
    x_rec:
        if (rec)
            CR0(R(ra));
        break;
    }
    case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43: case 48: case 49: case 50: case 51: {
        uint32_t ea = (uint32_t)((op & 1 ? R(ra) : RA0(ra)) + (uint64_t)simm);
        switch (op & ~1u) {
        case 32: R(rd) = xe_lwz(M_, ea); break;
        case 34: R(rd) = xe_lbz(M_, ea); break;
        case 40: R(rd) = xe_lhz(M_, ea); break;
        case 42: R(rd) = (uint64_t)(int64_t)(int16_t)xe_lhz(M_, ea); break;
        case 48: FSET_S(rd, ppc_lfs_bits(xe_lwz(M_, ea))); break;
        default: c->fpr[rd] = ppc_u2f(xe_ld(M_, ea)); break;
        }
        if (op & 1)
            R(ra) = ea;
        break;
    }
    case 36: case 37: case 38: case 39: case 44: case 45: case 52: case 53: case 54: case 55: {
        uint32_t ea = (uint32_t)((op & 1 ? R(ra) : RA0(ra)) + (uint64_t)simm);
        switch (op & ~1u) {
        case 36: xe_stw(M_, ea, (uint32_t)R(rd)); break;
        case 38: xe_stb(M_, ea, (uint32_t)R(rd)); break;
        case 44: xe_sth(M_, ea, (uint32_t)R(rd)); break;
        case 52: xe_stw(M_, ea, ppc_stfs_bits(c->fpr[rd])); break;
        default: xe_std(M_, ea, ppc_f2u(c->fpr[rd])); break;
        }
        if (op & 1)
            R(ra) = ea;
        break;
    }
    case 46: { /* lmw */
        uint32_t ea = (uint32_t)(RA0(ra) + (uint64_t)simm);
        for (unsigned r = rd; r < 32; r++, ea += 4)
            R(r) = xe_lwz(M_, ea);
        break;
    }
    case 47: { /* stmw */
        uint32_t ea = (uint32_t)(RA0(ra) + (uint64_t)simm);
        for (unsigned r = rd; r < 32; r++, ea += 4)
            xe_stw(M_, ea, (uint32_t)R(r));
        break;
    }
    case 58:
    case 62: { /* ld / ldu / lwa, std / stdu */
        unsigned k = w & 3;
        int64_t ds = (int16_t)(w & 0xFFFC);
        uint32_t ea = (uint32_t)((k == 1 ? R(ra) : RA0(ra)) + (uint64_t)ds);
        if (op == 58 && k == 0)
            R(rd) = xe_ld(M_, ea);
        else if (op == 58 && k == 1)
            R(rd) = xe_ld(M_, ea), R(ra) = ea;
        else if (op == 58 && k == 2)
            R(rd) = (uint64_t)(int64_t)(int32_t)xe_lwz(M_, ea);
        else if (op == 62 && k == 0)
            xe_std(M_, ea, R(rd));
        else if (op == 62 && k == 1)
            xe_std(M_, ea, R(rd)), R(ra) = ea;
        else
            return 1;
        break;
    }
    case 59: { /* single precision */
        double fa = c->fpr[ra], fb = c->fpr[rb], fc = c->fpr[rc];
        switch ((w >> 1) & 31) {
        case 18: FSET_S(rd, fa / fb); break;
        case 20: FSET_S(rd, fa - fb); break;
        case 21: FSET_S(rd, fa + fb); break;
        case 22: FSET_S(rd, sqrt(fb)); break;
        case 24: FSET_S(rd, ppc_fres(fb)); break;
        case 25: FSET_S(rd, fa * fc); break;
        case 28: FSET_S(rd, ppc_fmsub(fa, fc, fb)); break;
        case 29: FSET_S(rd, ppc_fmadd(fa, fc, fb)); break;
        case 30: FSET_S(rd, ppc_fnmsub(fa, fc, fb)); break;
        case 31: FSET_S(rd, ppc_fnmadd(fa, fc, fb)); break;
        default: return 1;
        }
        goto fp_rec;
    }
    case 63: {
        double fa = c->fpr[ra], fb = c->fpr[rb], fc = c->fpr[rc];
        unsigned x5 = (w >> 1) & 31;
        if (x5 & 16) {
            switch (x5) {
            case 18: c->fpr[rd] = fa / fb; break;
            case 20: c->fpr[rd] = fa - fb; break;
            case 21: c->fpr[rd] = fa + fb; break;
            case 22: c->fpr[rd] = sqrt(fb); break;
            case 23: c->fpr[rd] = ppc_fsel(fa, fc, fb); break;
            case 25: c->fpr[rd] = fa * fc; break;
            case 26: c->fpr[rd] = ppc_frsqrte(fb); break;
            case 28: c->fpr[rd] = ppc_fmsub(fa, fc, fb); break;
            case 29: c->fpr[rd] = ppc_fmadd(fa, fc, fb); break;
            case 30: c->fpr[rd] = ppc_fnmsub(fa, fc, fb); break;
            case 31: c->fpr[rd] = ppc_fnmadd(fa, fc, fb); break;
            default: return 1;
            }
            goto fp_rec;
        }
        switch (xo) {
        case 0:
        case 32: { /* fcmpu, fcmpo */
            uint32_t f = ppc_fcmp(fa, fb);
            c->cr = ppc_cr_set(c->cr, rd >> 2, f);
            c->fpscr = (c->fpscr & ~0xF000u) | (f << 12);
            c->pc = next;
            return 0;
        }
        case 12: FSET_S(rd, fb); break;
        case 14: c->fpr[rd] = ppc_fctiw(fb, 0); break;
        case 15: c->fpr[rd] = ppc_fctiw(fb, 1); break;
        case 40: c->fpr[rd] = ppc_fneg(fb); break;
        case 72: c->fpr[rd] = fb; break;
        case 136: c->fpr[rd] = ppc_fnabs(fb); break;
        case 264: c->fpr[rd] = ppc_fabs(fb); break;
        case 814: c->fpr[rd] = ppc_u2f((uint64_t)ppc_fctid(fb, 0)); break;
        case 815: c->fpr[rd] = ppc_u2f((uint64_t)ppc_fctid(fb, 1)); break;
        case 846: c->fpr[rd] = (double)(int64_t)ppc_f2u(fb); break;
        case 583: /* mffs */
            c->fpr[rd] = u2d(0xFFF8000000000000ull | c->fpscr);
            break;
        case 711: { /* mtfsf FM, frB */
            unsigned fm = (w >> 17) & 0xFF;
            uint32_t v = (uint32_t)d2u(fb), m = 0;
            for (int k = 0; k < 8; k++)
                if (fm & (0x80 >> k))
                    m |= 0xF0000000u >> (4 * k);
            c->fpscr = (c->fpscr & ~m) | (v & m);
            set_rounding(c->fpscr);
            break;
        }
        case 134: { /* mtfsfi crfD, IMM */
            unsigned f = rd >> 2, imm = (w >> 12) & 15;
            uint32_t m = 0xF0000000u >> (4 * f);
            c->fpscr = (c->fpscr & ~m) | ((imm << (28 - 4 * f)) & m);
            set_rounding(c->fpscr);
            break;
        }
        case 70: /* mtfsb0 */
            c->fpscr &= ~(0x80000000u >> rd);
            set_rounding(c->fpscr);
            break;
        case 38: /* mtfsb1 */
            c->fpscr |= 0x80000000u >> rd;
            set_rounding(c->fpscr);
            break;
        case 64: { /* mcrfs crfD, crfS */
            unsigned d = rd >> 2, s = ra >> 2;
            uint32_t f = (c->fpscr >> (28 - 4 * s)) & 15;
            c->cr = (c->cr & ~(0xF0000000u >> (4 * d))) | (f << (28 - 4 * d));
            c->fpscr &= ~((0xF0000000u >> (4 * s)) & 0x9FF80700u); /* the exception bits copied are cleared */
            break;
        }
        default:
            return 1;
        }
        goto fp_rec;
    }
    default:
        return 1;
    }
    if (trap >= 0) {
        if (trap) {
            if (op == 3 && rd == 31 && ra == 0)
                x360_fail(x, "debug break (twi 31, r0, %d) at %08X", (int)simm, pc);
            else
                x360_fail(x, "trap at %08X (%08X): the title asserted", pc, w);
            return -1;
        }
    }
    c->pc = next;
    return 0;
fp_rec:
    if (rec) /* record form: CR1 from FPSCR[FX FEX VX OX] */
        c->cr = (c->cr & 0xF0FFFFFFu) | ((c->fpscr >> 4) & 0x0F000000u);
    c->pc = next;
    return 0;
}

int x360_interp_one(x360_machine *x)
{
    xe_cpu *c = &x360_cur(x)->cpu;
    uint32_t pc = c->pc, w = x360_r32(x, pc);
    x->st.interpreted++;
    int r = exec(x, c, pc, w);
    if (r > 0) {
        x360_fail(x, "instruction %08X at %08X is neither lifted nor interpreted (x360_interp.c)", w, pc);
        return -1;
    }
    return r;
}

/* ---- missed code (XBA-283) ---- */

/* return addresses of calls interpreted code made: the dispatcher reaching one is not a new miss */
static unsigned ret_slot(uint32_t a)
{
    return (a * 2654435761u) >> (32 - X360_INTERP_RET_BITS);
}

static void ret_add(x360_machine *x, uint32_t a)
{
    unsigned n = 1u << X360_INTERP_RET_BITS, k = ret_slot(a);
    for (unsigned i = 0; i < n; i++, k = (k + 1) & (n - 1)) {
        if (x->interp_ret[k] == a)
            return;
        if (!x->interp_ret[k]) {
            if (x->interp_nret < n / 2) { /* half full at most: probes stay short (more calls are just not kept) */
                x->interp_ret[k] = a;
                x->interp_nret++;
            }
            return;
        }
    }
}

int x360_interp_ret(const x360_machine *x, uint32_t a)
{
    unsigned n = 1u << X360_INTERP_RET_BITS, k = ret_slot(a);
    for (unsigned i = 0; i < n; i++, k = (k + 1) & (n - 1)) {
        if (x->interp_ret[k] == a)
            return 1;
        if (!x->interp_ret[k])
            return 0;
    }
    return 0;
}

/* where the dispatcher takes over: lifted code, a thunk, the runtime's own return addresses */
static int handled_elsewhere(x360_machine *x, uint32_t pc)
{
    if (pc == X360_THREAD_EXIT || pc == X360_SEH_RETURN || pc == X360_APC_RETURN)
        return 1;
    if (pc >= x->code_lo && pc < x->code_hi && !(pc & 3) && x->fn_tab[(pc - x->code_lo) >> 2])
        return 1;
    return x360_thunk_at(x, pc) != NULL;
}

/* a bctr after a bounds check (cmplwi rI, N within the 8 instructions before it): a switch through a jump table,
 * whose targets are inside the function; any other bctr is a tail call (GCC's and MSVC's calls through a pointer) */
static int jump_table(x360_machine *x, uint32_t pc)
{
    for (unsigned k = 1; k <= 8 && pc - 4 * k >= x->image_base; k++)
        if ((x360_r32(x, pc - 4 * k) >> 26) == 10)
            return 1;
    return 0;
}

int x360_interp_run(x360_machine *x, x360_thread *t, int fresh)
{
    xe_cpu *c = &t->cpu;
    if (fresh && !x360_interp_ret(x, c->pc))
        x360_note_missed(x, c->pc);
    unsigned n = 0;
    for (;;) {
        uint32_t pc = c->pc;
        if (pc < x->image_base || pc - x->image_base >= x->image_size || (pc & 3)) {
            x360_fail(x, "interpreted code jumped to %08X, outside the image (thread %u, LR %08X, CTR %08X, r1 %08X)", pc,
                      t->id, (uint32_t)c->lr, (uint32_t)c->ctr, (uint32_t)c->gpr[1]);
            return -1;
        }
        uint32_t w = x360_r32(x, pc);
        x->st.interpreted++;
        x->m.rc_native++; /* the time base and the instruction budget count interpreted code too */
        int r = exec(x, c, pc, w);
        if (r) {
            if (r > 0)
                x360_fail(x, "instruction %08X at %08X is neither lifted nor interpreted (x360_interp.c)", w, pc);
            return -1;
        }
        uint32_t to = c->pc;
        int branched = to != pc + 4;
        unsigned op = w >> 26;
        int call = (w & 1) && (op == 18 || op == 16 || (op == 19 && (((w >> 1) & 0x3FF) == 16 ||
                                                                         ((w >> 1) & 0x3FF) == 528)));
        if (call)
            ret_add(x, pc + 4);
        if (handled_elsewhere(x, to))
            return 0;
        if (branched && (call || (op == 19 && ((w >> 1) & 0x3FF) == 528 && !jump_table(x, pc))))
            x360_note_missed(x, to); /* a call, or a tail call through CTR, into code the pack does not hold */
        if ((++n & 7) == 0 || branched) {
            if (--x->m.slice < 0 && ((c->msr & 0x8000u) || x->m.slice < -XE_EE_GRACE))
                return 1; /* the time slice ran out inside interpreted code */
            if (x->m.rc_native >= x->stop_at || x->status != X360_RUNNING)
                return 1;
        }
    }
}
