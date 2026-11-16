/* gc_cpu.c -- the Gekko interpreter (XBA-225): 32-bit PowerPC as the 750 runs it, its supervisor model (MSR,
 * SRR0/1, the exceptions libogc uses: external interrupt, decrementer, system call, FP unavailable, program for
 * tw), and the Gekko's paired singles and quantised loads / stores. It runs whatever lifted code does not cover,
 * with the semantics of ppc_ops.h / gc_recomp_rt.h that lifted code uses too.
 *
 * Not modelled (spike): the MMU (BATs, page tables: EA & 0x3FFFFFFF is the physical address, gc_recomp_rt.h),
 * caches (dcbz zeroes, icbi drops lifted spans, the rest are no-ops), FPSCR exception bits and rounding modes (round
 * to nearest), lswx / stswx, eciwx / ecowx, the estimate tables of fres / frsqrte / ps_res / ps_rsqrte (exact). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gc_internal.h"

void gc_stop(gc_machine *m, const char *fmt, ...)
{
    if (m->error[0])
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->error, sizeof m->error, fmt, ap);
    va_end(ap);
    m->h.next_event = m->h.now; /* leave the run loop */
}

bool gc_irq_pending(gc_machine *m)
{
    return (m->pi_intsr & m->pi_intmr) != 0 || m->dec_pending;
}

void gc_check_interrupts(gc_machine *m)
{
    if ((m->h.cpu.msr & PPC_MSR_EE) && gc_irq_pending(m))
        m->h.next_event = m->h.now;
}

void gc_exception(gc_machine *m, uint32_t vector, uint32_t srr1_bits)
{
    gc_cpu *c = &m->h.cpu;
    c->spr[PPC_SPR_SRR0] = c->pc;
    c->spr[PPC_SPR_SRR1] = (c->msr & 0x87C0FFFFu) | srr1_bits;
    c->msr &= PPC_MSR_ME | PPC_MSR_IP;
    c->pc = ((c->msr & PPC_MSR_IP) ? 0xFFF00000u : 0) | vector;
    c->reserve = 0;
    if (m->verbose >= 2)
        fprintf(stderr, "gc: exception %04X from %08X\n", vector, c->spr[PPC_SPR_SRR0]);
}

uint32_t gc_mfspr(gc_machine *m, unsigned spr)
{
    gc_cpu *c = &m->h.cpu;
    switch (spr) {
    case PPC_SPR_XER:
        return RC_XER();
    case PPC_SPR_LR:
        return c->lr;
    case PPC_SPR_CTR:
        return c->ctr;
    case PPC_SPR_DEC: {
        uint64_t ticks = gc_tb(m) - (m->tb_base + m->dec_written / GC_TB_DIV);
        return m->dec - (uint32_t)ticks;
    }
    case PPC_SPR_TBL_R:
        return (uint32_t)gc_tb(m);
    case PPC_SPR_TBU_R:
        return (uint32_t)(gc_tb(m) >> 32);
    default:
        return c->spr[spr & 1023];
    }
}

void gc_mtspr(gc_machine *m, unsigned spr, uint32_t v)
{
    gc_cpu *c = &m->h.cpu;
    switch (spr) {
    case PPC_SPR_XER:
        RC_SET_XER(v);
        return;
    case PPC_SPR_LR:
        c->lr = v;
        return;
    case PPC_SPR_CTR:
        c->ctr = v;
        return;
    case PPC_SPR_DEC:
        m->dec = v;
        m->dec_written = m->h.now;
        gc_schedule(m);
        return;
    case PPC_SPR_TBL_W: {
        uint64_t tb = gc_tb(m);
        tb = (tb & 0xFFFFFFFF00000000ull) | v;
        m->tb_base = tb - m->h.now / GC_TB_DIV;
        gc_schedule(m);
        return;
    }
    case PPC_SPR_TBU_W: {
        uint64_t tb = gc_tb(m);
        tb = (tb & 0xFFFFFFFFull) | (uint64_t)v << 32;
        m->tb_base = tb - m->h.now / GC_TB_DIV;
        gc_schedule(m);
        return;
    }
    default:
        c->spr[spr & 1023] = v;
        return;
    }
}

void gc_mtmsr(gc_machine *m, uint32_t v)
{
    m->h.cpu.msr = v;
    gc_check_interrupts(m);
}

void gc_cache_op(gc_machine *m, uint32_t ea, unsigned kind)
{
    if (kind == GC_CACHE_DCBZ) {
        ea &= ~31u;
        for (unsigned i = 0; i < 32; i += 4)
            rc_stw(m, ea + i, 0);
    } else {
        gc_recomp_icbi(m, ea);
    }
}

/* ---- the interpreter ---- */

#define RD ((w >> 21) & 31)
#define RA ((w >> 16) & 31)
#define RB ((w >> 11) & 31)
#define RC6 ((w >> 6) & 31)
#define SIMM ((uint32_t)(int32_t)(int16_t)w)
#define UIMM (w & 0xFFFFu)

static inline uint32_t ea_d(gc_cpu *c, uint32_t w)
{
    return (RA ? c->gpr[RA] : 0) + SIMM;
}

static inline uint32_t ea_x(gc_cpu *c, uint32_t w)
{
    return (RA ? c->gpr[RA] : 0) + c->gpr[RB];
}

static void set_ov(gc_cpu *c, uint32_t ov)
{
    c->xer_ov = ov;
    c->xer_so |= ov;
}

/* Taken-ness of bc / bclr / bcctr (CTR decremented when BO says so). */
static bool bc_taken(gc_cpu *c, uint32_t bo, uint32_t bi)
{
    bool ctr_ok = true;
    if (!(bo & 4)) {
        c->ctr--;
        ctr_ok = (bo & 2) ? c->ctr == 0 : c->ctr != 0;
    }
    bool cond_ok = (bo & 16) || ppc_cr_bit(c->cr, bi) == ((bo >> 3) & 1);
    return ctr_ok && cond_ok;
}

static void fp_unavailable(gc_machine *m)
{
    gc_exception(m, 0x800, 0);
}

static void illegal(gc_machine *m, uint32_t pc, uint32_t w)
{
    gc_stop(m, "unsupported instruction %08X at %08X", w, pc);
}

/* Arithmetic XO-form ops of opcode 31 (OE in bit 10): true when handled. */
static bool xo_arith(gc_cpu *c, uint32_t w, unsigned xo9)
{
    uint32_t a = c->gpr[RA], b = c->gpr[RB], r, ov = 0;
    bool oe = (w >> 10) & 1;
    switch (xo9) {
    case 8: /* subfc */
        r = ~a + b + 1;
        c->xer_ca = ppc_carry(~a, b, 1);
        ov = ppc_ovf_add(~a, b, r);
        break;
    case 10: /* addc */
        r = a + b;
        c->xer_ca = ppc_carry(a, b, 0);
        ov = ppc_ovf_add(a, b, r);
        break;
    case 11: /* mulhwu */
        r = ppc_mulhwu(a, b);
        oe = false;
        break;
    case 40: /* subf */
        r = b - a;
        ov = ppc_ovf_add(~a, b, r);
        break;
    case 75: /* mulhw */
        r = ppc_mulhw(a, b);
        oe = false;
        break;
    case 104: /* neg */
        r = (uint32_t)0 - a;
        ov = a == 0x80000000u;
        break;
    case 136: { /* subfe */
        uint32_t ci = c->xer_ca;
        r = ~a + b + ci;
        c->xer_ca = ppc_carry(~a, b, ci);
        ov = ppc_ovf_add(~a, b, r);
        break;
    }
    case 138: { /* adde */
        uint32_t ci = c->xer_ca;
        r = a + b + ci;
        c->xer_ca = ppc_carry(a, b, ci);
        ov = ppc_ovf_add(a, b, r);
        break;
    }
    case 200: { /* subfze */
        uint32_t ci = c->xer_ca;
        r = ~a + ci;
        c->xer_ca = ppc_carry(~a, 0, ci);
        ov = ppc_ovf_add(~a, 0, r);
        break;
    }
    case 202: { /* addze */
        uint32_t ci = c->xer_ca;
        r = a + ci;
        c->xer_ca = ppc_carry(a, 0, ci);
        ov = ppc_ovf_add(a, 0, r);
        break;
    }
    case 232: { /* subfme */
        uint32_t ci = c->xer_ca;
        r = ~a + 0xFFFFFFFFu + ci;
        c->xer_ca = ppc_carry(~a, 0xFFFFFFFFu, ci);
        ov = ppc_ovf_add(~a, 0xFFFFFFFFu, r);
        break;
    }
    case 234: { /* addme */
        uint32_t ci = c->xer_ca;
        r = a + 0xFFFFFFFFu + ci;
        c->xer_ca = ppc_carry(a, 0xFFFFFFFFu, ci);
        ov = ppc_ovf_add(a, 0xFFFFFFFFu, r);
        break;
    }
    case 235: { /* mullw */
        int64_t p = (int64_t)(int32_t)a * (int32_t)b;
        r = (uint32_t)p;
        ov = p != (int64_t)(int32_t)r;
        break;
    }
    case 266: /* add */
        r = a + b;
        ov = ppc_ovf_add(a, b, r);
        break;
    case 459: /* divwu */
        r = ppc_divwu(a, b);
        ov = b == 0;
        break;
    case 491: /* divw */
        r = ppc_divw(a, b);
        ov = b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu);
        break;
    default:
        return false;
    }
    c->gpr[RD] = r;
    if (oe)
        set_ov(c, ov);
    if (w & 1)
        RC_CR0(r);
    return true;
}

static void op31(gc_machine *m, uint32_t pc, uint32_t w)
{
    gc_cpu *c = &m->h.cpu;
    unsigned xo = (w >> 1) & 0x3FF;
    if (xo_arith(c, w, xo & 0x1FF))
        return;
    uint32_t s = c->gpr[RD], b = c->gpr[RB], r, ea;
    bool rc = w & 1;
    switch (xo) {
    case 0: /* cmp */
        c->cr = ppc_cr_set(c->cr, RD >> 2, ppc_cmps((int32_t)c->gpr[RA], (int32_t)b, c->xer_so));
        return;
    case 32: /* cmpl */
        c->cr = ppc_cr_set(c->cr, RD >> 2, ppc_cmpu(c->gpr[RA], b, c->xer_so));
        return;
    case 4: { /* tw */
        int32_t a = (int32_t)c->gpr[RA], bb = (int32_t)b;
        unsigned to = RD;
        if (((to & 16) && a < bb) || ((to & 8) && a > bb) || ((to & 4) && a == bb) ||
            ((to & 2) && (uint32_t)a < (uint32_t)bb) || ((to & 1) && (uint32_t)a > (uint32_t)bb)) {
            c->pc = pc;
            gc_exception(m, 0x700, 0x00020000u);
        }
        return;
    }
    case 19: /* mfcr */
        c->gpr[RD] = c->cr;
        return;
    case 144: { /* mtcrf */
        unsigned crm = (w >> 12) & 0xFF;
        uint32_t mask = 0;
        for (int i = 0; i < 8; i++)
            if (crm & (0x80u >> i))
                mask |= 0xF0000000u >> (4 * i);
        c->cr = (c->cr & ~mask) | (s & mask);
        return;
    }
    case 512: /* mcrxr */
        c->cr = ppc_cr_set(c->cr, RD >> 2, c->xer_so << 3 | c->xer_ov << 2 | c->xer_ca << 1);
        c->xer_so = c->xer_ov = c->xer_ca = 0;
        return;
    case 83: /* mfmsr */
        c->gpr[RD] = c->msr;
        return;
    case 146: /* mtmsr */
        gc_mtmsr(m, s);
        return;
    case 339: /* mfspr */
    case 371: /* mftb */
        c->gpr[RD] = gc_mfspr(m, ((w >> 16) & 31) | (((w >> 11) & 31) << 5));
        return;
    case 467: /* mtspr */
        gc_mtspr(m, ((w >> 16) & 31) | (((w >> 11) & 31) << 5), s);
        return;
    case 595: /* mfsr */
        c->gpr[RD] = c->sr[(w >> 16) & 15];
        return;
    case 659: /* mfsrin */
        c->gpr[RD] = c->sr[b >> 28];
        return;
    case 210: /* mtsr */
        c->sr[(w >> 16) & 15] = s;
        return;
    case 242: /* mtsrin */
        c->sr[b >> 28] = s;
        return;
    case 24: r = ppc_slw(s, b); break;                     /* slw */
    case 536: r = ppc_srw(s, b); break;                    /* srw */
    case 792: r = ppc_sraw(s, b, &c->xer_ca); break;       /* sraw */
    case 824: r = ppc_srawi(s, RB, &c->xer_ca); break;     /* srawi */
    case 26: r = ppc_cntlzw(s); break;                     /* cntlzw */
    case 28: r = s & b; break;                             /* and */
    case 60: r = s & ~b; break;                            /* andc */
    case 124: r = ~(s | b); break;                         /* nor */
    case 284: r = ~(s ^ b); break;                         /* eqv */
    case 316: r = s ^ b; break;                            /* xor */
    case 412: r = s | ~b; break;                           /* orc */
    case 444: r = s | b; break;                            /* or */
    case 476: r = ~(s & b); break;                         /* nand */
    case 922: r = (uint32_t)(int32_t)(int16_t)s; break;    /* extsh */
    case 954: r = (uint32_t)(int32_t)(int8_t)s; break;     /* extsb */
    /* loads and stores, indexed */
    case 23: c->gpr[RD] = rc_lwz(m, ea_x(c, w)); return;  /* lwzx */
    case 55: ea = c->gpr[RA] + b; c->gpr[RD] = rc_lwz(m, ea); c->gpr[RA] = ea; return; /* lwzux */
    case 87: c->gpr[RD] = rc_lbz(m, ea_x(c, w)); return;  /* lbzx */
    case 119: ea = c->gpr[RA] + b; c->gpr[RD] = rc_lbz(m, ea); c->gpr[RA] = ea; return; /* lbzux */
    case 279: c->gpr[RD] = rc_lhz(m, ea_x(c, w)); return; /* lhzx */
    case 311: ea = c->gpr[RA] + b; c->gpr[RD] = rc_lhz(m, ea); c->gpr[RA] = ea; return; /* lhzux */
    case 343: c->gpr[RD] = (uint32_t)(int32_t)(int16_t)rc_lhz(m, ea_x(c, w)); return; /* lhax */
    case 375: ea = c->gpr[RA] + b; c->gpr[RD] = (uint32_t)(int32_t)(int16_t)rc_lhz(m, ea); c->gpr[RA] = ea; return;
    case 534: c->gpr[RD] = __builtin_bswap32(rc_lwz(m, ea_x(c, w))); return; /* lwbrx */
    case 790: c->gpr[RD] = __builtin_bswap16((uint16_t)rc_lhz(m, ea_x(c, w))); return; /* lhbrx */
    case 20: /* lwarx */
        c->gpr[RD] = rc_lwz(m, ea_x(c, w));
        c->reserve = 1;
        return;
    case 150: /* stwcx. */
        if (c->reserve) {
            rc_stw(m, ea_x(c, w), s);
            c->reserve = 0;
            c->cr = ppc_cr_set(c->cr, 0, PPC_CR_EQ | c->xer_so);
        } else {
            c->cr = ppc_cr_set(c->cr, 0, c->xer_so);
        }
        return;
    case 151: rc_stw(m, ea_x(c, w), s); return;           /* stwx */
    case 183: ea = c->gpr[RA] + b; rc_stw(m, ea, s); c->gpr[RA] = ea; return; /* stwux */
    case 215: rc_stb(m, ea_x(c, w), s); return;           /* stbx */
    case 247: ea = c->gpr[RA] + b; rc_stb(m, ea, s); c->gpr[RA] = ea; return; /* stbux */
    case 407: rc_sth(m, ea_x(c, w), s); return;           /* sthx */
    case 439: ea = c->gpr[RA] + b; rc_sth(m, ea, s); c->gpr[RA] = ea; return; /* sthux */
    case 662: rc_stw(m, ea_x(c, w), __builtin_bswap32(s)); return; /* stwbrx */
    case 918: rc_sth(m, ea_x(c, w), __builtin_bswap16((uint16_t)s)); return; /* sthbrx */
    case 597: { /* lswi */
        unsigned n = RB ? RB : 32, rd = RD;
        ea = RA ? c->gpr[RA] : 0;
        for (unsigned i = 0; i < n; i++) {
            if (i % 4 == 0) {
                if (i)
                    rd = (rd + 1) & 31;
                c->gpr[rd] = 0;
            }
            c->gpr[rd] |= rc_lbz(m, ea + i) << (24 - 8 * (i % 4));
        }
        return;
    }
    case 725: { /* stswi */
        unsigned n = RB ? RB : 32, rs = RD;
        ea = RA ? c->gpr[RA] : 0;
        for (unsigned i = 0; i < n; i++) {
            if (i % 4 == 0 && i)
                rs = (rs + 1) & 31;
            rc_stb(m, ea + i, c->gpr[rs] >> (24 - 8 * (i % 4)));
        }
        return;
    }
    /* FP loads and stores, indexed */
    case 535: case 567: case 599: case 631: case 663: case 695: case 727: case 759: case 983: {
        if (!(c->msr & PPC_MSR_FP)) {
            c->pc = pc;
            fp_unavailable(m);
            return;
        }
        bool upd = xo == 567 || xo == 631 || xo == 695 || xo == 759;
        ea = upd ? c->gpr[RA] + b : ea_x(c, w);
        switch (xo) {
        case 535: case 567: gc_fset_single(c, RD, ppc_lfs_bits(rc_lwz(m, ea))); break;
        case 599: case 631: c->ps0[RD] = ppc_u2f(rc_ld64(m, ea)); break;
        case 663: case 695: rc_stw(m, ea, ppc_stfs_bits(c->ps0[RD])); break;
        case 727: case 759: rc_st64(m, ea, ppc_f2u(c->ps0[RD])); break;
        default: rc_stw(m, ea, (uint32_t)ppc_f2u(c->ps0[RD])); break; /* stfiwx */
        }
        if (upd)
            c->gpr[RA] = ea;
        return;
    }
    /* cache and ordering */
    case 1014: gc_cache_op(m, ea_x(c, w), GC_CACHE_DCBZ); return; /* dcbz */
    case 982: gc_cache_op(m, ea_x(c, w), GC_CACHE_ICBI); return;  /* icbi */
    case 54: case 86: case 246: case 278: case 470: case 598: case 854: case 306: case 370: case 566:
        return; /* dcbst dcbf dcbtst dcbt dcbi sync eieio tlbie tlbia tlbsync */
    default:
        illegal(m, pc, w);
        return;
    }
    c->gpr[RA] = r;
    if (rc)
        RC_CR0(r);
}

static void op19(gc_machine *m, uint32_t pc, uint32_t w)
{
    gc_cpu *c = &m->h.cpu;
    unsigned xo = (w >> 1) & 0x3FF;
    unsigned d = RD, a = RA, b = RB;
    switch (xo) {
    case 0: /* mcrf */
        c->cr = ppc_cr_set(c->cr, d >> 2, (c->cr >> (28 - 4 * (a >> 2))) & 15);
        return;
    case 16: { /* bclr */
        uint32_t target = c->lr & ~3u;
        bool taken = bc_taken(c, d, a);
        if (w & 1) /* LK sets LR whether or not the branch is taken */
            c->lr = pc + 4;
        if (taken) {
            c->pc = target;
            m->branched = true;
        }
        return;
    }
    case 528: /* bcctr */
        bool taken = bc_taken(c, d | 4, a);
        if (w & 1)
            c->lr = pc + 4;
        if (taken) {
            c->pc = c->ctr & ~3u;
            m->branched = true;
        }
        return;
    case 50: /* rfi */
        c->msr = (c->msr & ~0x87C0FFFFu) | (c->spr[PPC_SPR_SRR1] & 0x87C0FFFFu);
        c->pc = c->spr[PPC_SPR_SRR0] & ~3u;
        c->reserve = 0;
        gc_check_interrupts(m);
        return;
    case 150: /* isync */
        return;
    case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449: {
        uint32_t x = ppc_cr_bit(c->cr, a), y = ppc_cr_bit(c->cr, b), r;
        switch (xo) {
        case 33: r = !(x | y); break;  /* crnor */
        case 129: r = x & !y; break;   /* crandc */
        case 193: r = x ^ y; break;    /* crxor */
        case 225: r = !(x & y); break; /* crnand */
        case 257: r = x & y; break;    /* crand */
        case 289: r = !(x ^ y); break; /* creqv */
        case 417: r = x | !y; break;   /* crorc */
        default: r = x | y; break;     /* cror */
        }
        c->cr = ppc_cr_setbit(c->cr, d, r);
        return;
    }
    default:
        illegal(m, pc, w);
    }
}

/* Opcodes 59 / 63: FP arithmetic. */
static void op_fp(gc_machine *m, uint32_t pc, uint32_t w, bool single)
{
    gc_cpu *c = &m->h.cpu;
    unsigned d = RD, a = RA, b = RB, cc = RC6;
    unsigned xo5 = (w >> 1) & 31, xo = (w >> 1) & 0x3FF;
    double fa = c->ps0[a], fb = c->ps0[b], fc = c->ps0[cc], r;
    if (!single && !(xo5 & 16)) { /* opcode 63 X-form */
        switch (xo) {
        case 0: case 32: { /* fcmpu / fcmpo */
            uint32_t f = ppc_fcmp(fa, fb);
            c->cr = ppc_cr_set(c->cr, d >> 2, f);
            c->fpscr = (c->fpscr & ~0xF000u) | (f << 12);
            return;
        }
        case 12: gc_fset_single(c, d, fb); break;           /* frsp */
        case 14: c->ps0[d] = ppc_fctiw(fb, 0); break;       /* fctiw */
        case 15: c->ps0[d] = ppc_fctiw(fb, 1); break;       /* fctiwz */
        case 40: c->ps0[d] = ppc_fneg(fb); break;           /* fneg */
        case 72: c->ps0[d] = fb; break;                     /* fmr */
        case 136: c->ps0[d] = ppc_fnabs(fb); break;         /* fnabs */
        case 264: c->ps0[d] = ppc_fabs(fb); break;          /* fabs */
        case 583: c->ps0[d] = ppc_u2f(0xFFF8000000000000ull | c->fpscr); break; /* mffs */
        case 711: { /* mtfsf */
            unsigned fm = (w >> 17) & 0xFF;
            uint32_t mask = 0, v = (uint32_t)ppc_f2u(fb);
            for (int i = 0; i < 8; i++)
                if (fm & (0x80u >> i))
                    mask |= 0xF0000000u >> (4 * i);
            c->fpscr = (c->fpscr & ~mask) | (v & mask);
            break;
        }
        case 38: c->fpscr |= 0x80000000u >> d; break;       /* mtfsb1 */
        case 70: c->fpscr &= ~(0x80000000u >> d); break;    /* mtfsb0 */
        case 134: { /* mtfsfi */
            unsigned sh = 28 - 4 * (d >> 2);
            c->fpscr = (c->fpscr & ~(0xFu << sh)) | (((w >> 12) & 15u) << sh);
            break;
        }
        case 64: { /* mcrfs */
            unsigned sh = 28 - 4 * (a >> 2);
            c->cr = ppc_cr_set(c->cr, d >> 2, (c->fpscr >> sh) & 15);
            break;
        }
        default:
            illegal(m, pc, w);
            return;
        }
        if (w & 1)
            c->cr = ppc_cr_set(c->cr, 1, c->fpscr >> 28);
        return;
    }
    switch (xo5) {
    case 18: r = fa / fb; break;
    case 20: r = fa - fb; break;
    case 21: r = fa + fb; break;
    case 22: r = sqrt(fb); break;
    case 23: /* fsel (63 only) */
        if (single) {
            illegal(m, pc, w);
            return;
        }
        c->ps0[d] = ppc_fsel(fa, fc, fb);
        goto rec;
    case 24: /* fres (59) */
        if (!single) {
            illegal(m, pc, w);
            return;
        }
        gc_fset_single(c, d, ppc_fres(fb));
        goto rec;
    case 25: r = fa * fc; break;
    case 26: /* frsqrte (63) */
        c->ps0[d] = ppc_frsqrte(fb);
        goto rec;
    case 28: r = fma(fa, fc, -fb); break;
    case 29: r = fma(fa, fc, fb); break;
    case 30: r = -fma(fa, fc, -fb); break;
    case 31: r = -fma(fa, fc, fb); break;
    default:
        illegal(m, pc, w);
        return;
    }
    if (single)
        gc_fset_single(c, d, r);
    else
        c->ps0[d] = r;
rec:
    if (w & 1)
        c->cr = ppc_cr_set(c->cr, 1, c->fpscr >> 28);
}

/* Opcode 4: the Gekko's paired singles (and dcbz_l). */
static void op4(gc_machine *m, uint32_t pc, uint32_t w)
{
    gc_cpu *c = &m->h.cpu;
    unsigned d = RD, a = RA, b = RB, cc = RC6;
    unsigned xo5 = (w >> 1) & 31, xo6 = (w >> 1) & 63, xo10 = (w >> 1) & 0x3FF;
    if (xo10 == 1014) { /* dcbz_l: a line of the locked cache */
        uint32_t ea = ((RA ? c->gpr[RA] : 0) + c->gpr[RB]) & ~31u;
        for (unsigned i = 0; i < 32; i += 4)
            rc_stw(m, ea + i, 0);
        return;
    }
    if (!(c->msr & PPC_MSR_FP)) {
        c->pc = pc;
        fp_unavailable(m);
        return;
    }
    if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) { /* psq_lx psq_stx psq_lux psq_stux */
        unsigned wbit = (w >> 10) & 1, i = (w >> 7) & 7;
        uint32_t ea = ((xo6 & 32) ? c->gpr[a] : (a ? c->gpr[a] : 0)) + c->gpr[b];
        if (xo6 & 1)
            gc_psq_store(m, d, ea, wbit, i);
        else
            gc_psq_load(m, d, ea, wbit, i);
        if (xo6 & 32)
            c->gpr[a] = ea;
        return;
    }
    double a0 = c->ps0[a], a1 = c->ps1[a], b0 = c->ps0[b], b1 = c->ps1[b], c0 = c->ps0[cc], c1 = c->ps1[cc];
    double r0, r1;
    switch (xo10) {
    case 0: case 32: case 64: case 96: { /* ps_cmpu0 ps_cmpo0 ps_cmpu1 ps_cmpo1 */
        uint32_t f = (xo10 & 64) ? ppc_fcmp(a1, b1) : ppc_fcmp(a0, b0);
        c->cr = ppc_cr_set(c->cr, d >> 2, f);
        c->fpscr = (c->fpscr & ~0xF000u) | (f << 12);
        return;
    }
    case 40: r0 = ppc_fneg(b0); r1 = ppc_fneg(b1); goto set;   /* ps_neg */
    case 72: r0 = b0; r1 = b1; goto set;                       /* ps_mr */
    case 136: r0 = ppc_fnabs(b0); r1 = ppc_fnabs(b1); goto set; /* ps_nabs */
    case 264: r0 = ppc_fabs(b0); r1 = ppc_fabs(b1); goto set;  /* ps_abs */
    case 528: r0 = a0; r1 = b0; goto set;                      /* ps_merge00 */
    case 560: r0 = a0; r1 = b1; goto set;                      /* ps_merge01 */
    case 592: r0 = a1; r1 = b0; goto set;                      /* ps_merge10 */
    case 624: r0 = a1; r1 = b1; goto set;                      /* ps_merge11 */
    default:
        break;
    }
    switch (xo5) {
    case 10: r0 = gc_ps_round(a0 + b1); r1 = c1; break;                    /* ps_sum0 */
    case 11: r0 = c0; r1 = gc_ps_round(a0 + b1); break;                    /* ps_sum1 */
    case 12: r0 = gc_ps_round(a0 * c0); r1 = gc_ps_round(a1 * c0); break;  /* ps_muls0 */
    case 13: r0 = gc_ps_round(a0 * c1); r1 = gc_ps_round(a1 * c1); break;  /* ps_muls1 */
    case 14: r0 = gc_ps_round(fma(a0, c0, b0)); r1 = gc_ps_round(fma(a1, c0, b1)); break; /* ps_madds0 */
    case 15: r0 = gc_ps_round(fma(a0, c1, b0)); r1 = gc_ps_round(fma(a1, c1, b1)); break; /* ps_madds1 */
    case 18: r0 = gc_ps_round(a0 / b0); r1 = gc_ps_round(a1 / b1); break;  /* ps_div */
    case 20: r0 = gc_ps_round(a0 - b0); r1 = gc_ps_round(a1 - b1); break;  /* ps_sub */
    case 21: r0 = gc_ps_round(a0 + b0); r1 = gc_ps_round(a1 + b1); break;  /* ps_add */
    case 23: r0 = ppc_fsel(a0, c0, b0); r1 = ppc_fsel(a1, c1, b1); break;  /* ps_sel */
    case 24: r0 = ppc_fres(b0); r1 = ppc_fres(b1); break;                  /* ps_res */
    case 25: r0 = gc_ps_round(a0 * c0); r1 = gc_ps_round(a1 * c1); break;  /* ps_mul */
    case 26: r0 = gc_ps_round(ppc_frsqrte(b0)); r1 = gc_ps_round(ppc_frsqrte(b1)); break; /* ps_rsqrte */
    case 28: r0 = gc_ps_round(fma(a0, c0, -b0)); r1 = gc_ps_round(fma(a1, c1, -b1)); break; /* ps_msub */
    case 29: r0 = gc_ps_round(fma(a0, c0, b0)); r1 = gc_ps_round(fma(a1, c1, b1)); break;   /* ps_madd */
    case 30: r0 = -gc_ps_round(fma(a0, c0, -b0)); r1 = -gc_ps_round(fma(a1, c1, -b1)); break; /* ps_nmsub */
    case 31: r0 = -gc_ps_round(fma(a0, c0, b0)); r1 = -gc_ps_round(fma(a1, c1, b1)); break;  /* ps_nmadd */
    default:
        illegal(m, pc, w);
        return;
    }
set:
    c->ps0[d] = r0;
    c->ps1[d] = r1;
    if (w & 1)
        c->cr = ppc_cr_set(c->cr, 1, c->fpscr >> 28);
}

void gc_cpu_step(gc_machine *m)
{
    gc_cpu *c = &m->h.cpu;
    uint32_t pc = c->pc;
    m->trace[m->trace_at++ & 63] = pc;
    m->branched = false;
    if (pc & 3) {
        gc_stop(m, "misaligned PC %08X", pc);
        return;
    }
    uint32_t pa = pc & 0x3FFFFFFFu;
    if (pa > GC_RAM_SIZE - 4) {
        gc_stop(m, "PC %08X outside MEM1", pc);
        return;
    }
    uint32_t w = rc_be32(m->h.ram + pa);
    c->pc = pc + 4;
    m->h.now++;
    unsigned op = w >> 26;
    uint32_t ea;
    switch (op) {
    case 3: { /* twi */
        int32_t a = (int32_t)c->gpr[RA], bb = (int32_t)SIMM;
        unsigned to = RD;
        if (((to & 16) && a < bb) || ((to & 8) && a > bb) || ((to & 4) && a == bb) ||
            ((to & 2) && (uint32_t)a < (uint32_t)bb) || ((to & 1) && (uint32_t)a > (uint32_t)bb)) {
            c->pc = pc;
            gc_exception(m, 0x700, 0x00020000u);
        }
        return;
    }
    case 4: op4(m, pc, w); return;
    case 7: c->gpr[RD] = (uint32_t)((int32_t)c->gpr[RA] * (int32_t)SIMM); return; /* mulli */
    case 8: { /* subfic */
        uint32_t a = c->gpr[RA];
        c->gpr[RD] = ~a + SIMM + 1;
        c->xer_ca = ppc_carry(~a, SIMM, 1);
        return;
    }
    case 10: c->cr = ppc_cr_set(c->cr, RD >> 2, ppc_cmpu(c->gpr[RA], UIMM, c->xer_so)); return; /* cmpli */
    case 11: c->cr = ppc_cr_set(c->cr, RD >> 2, ppc_cmps((int32_t)c->gpr[RA], (int32_t)SIMM, c->xer_so)); return;
    case 12: case 13: { /* addic addic. */
        uint32_t a = c->gpr[RA];
        c->gpr[RD] = a + SIMM;
        c->xer_ca = ppc_carry(a, SIMM, 0);
        if (op == 13)
            RC_CR0(c->gpr[RD]);
        return;
    }
    case 14: c->gpr[RD] = (RA ? c->gpr[RA] : 0) + SIMM; return;         /* addi */
    case 15: c->gpr[RD] = (RA ? c->gpr[RA] : 0) + (UIMM << 16); return; /* addis */
    case 16: { /* bc */
        uint32_t target = (uint32_t)(int32_t)(int16_t)(w & 0xFFFC) + ((w & 2) ? 0 : pc);
        bool taken = bc_taken(c, RD, RA);
        if (w & 1)
            c->lr = pc + 4;
        if (taken) {
            c->pc = target;
            m->branched = true;
        }
        return;
    }
    case 17: /* sc */
        gc_exception(m, 0xC00, 0);
        return;
    case 18: { /* b */
        int32_t li = (int32_t)(w << 6) >> 6;
        uint32_t target = (uint32_t)(li & ~3) + ((w & 2) ? 0 : pc);
        if (w & 1)
            c->lr = pc + 4;
        else if (target == pc && m->h.next_event > m->h.now) /* idle loop */
            m->h.now = m->h.next_event;
        c->pc = target;
        m->branched = true;
        return;
    }
    case 19: op19(m, pc, w); return;
    case 20: { /* rlwimi */
        uint32_t mask = ppc_mask(RC6, (w >> 1) & 31);
        uint32_t r = (ppc_rotl(c->gpr[RD], RB) & mask) | (c->gpr[RA] & ~mask);
        c->gpr[RA] = r;
        if (w & 1)
            RC_CR0(r);
        return;
    }
    case 21: { /* rlwinm */
        uint32_t r = ppc_rotl(c->gpr[RD], RB) & ppc_mask(RC6, (w >> 1) & 31);
        c->gpr[RA] = r;
        if (w & 1)
            RC_CR0(r);
        return;
    }
    case 23: { /* rlwnm */
        uint32_t r = ppc_rotl(c->gpr[RD], c->gpr[RB] & 31) & ppc_mask(RC6, (w >> 1) & 31);
        c->gpr[RA] = r;
        if (w & 1)
            RC_CR0(r);
        return;
    }
    case 24: c->gpr[RA] = c->gpr[RD] | UIMM; return;          /* ori */
    case 25: c->gpr[RA] = c->gpr[RD] | (UIMM << 16); return;  /* oris */
    case 26: c->gpr[RA] = c->gpr[RD] ^ UIMM; return;          /* xori */
    case 27: c->gpr[RA] = c->gpr[RD] ^ (UIMM << 16); return;  /* xoris */
    case 28: c->gpr[RA] = c->gpr[RD] & UIMM; RC_CR0(c->gpr[RA]); return;         /* andi. */
    case 29: c->gpr[RA] = c->gpr[RD] & (UIMM << 16); RC_CR0(c->gpr[RA]); return; /* andis. */
    case 31: op31(m, pc, w); return;
    case 32: c->gpr[RD] = rc_lwz(m, ea_d(c, w)); return;
    case 33: ea = c->gpr[RA] + SIMM; c->gpr[RD] = rc_lwz(m, ea); c->gpr[RA] = ea; return;
    case 34: c->gpr[RD] = rc_lbz(m, ea_d(c, w)); return;
    case 35: ea = c->gpr[RA] + SIMM; c->gpr[RD] = rc_lbz(m, ea); c->gpr[RA] = ea; return;
    case 36: rc_stw(m, ea_d(c, w), c->gpr[RD]); return;
    case 37: ea = c->gpr[RA] + SIMM; rc_stw(m, ea, c->gpr[RD]); c->gpr[RA] = ea; return;
    case 38: rc_stb(m, ea_d(c, w), c->gpr[RD]); return;
    case 39: ea = c->gpr[RA] + SIMM; rc_stb(m, ea, c->gpr[RD]); c->gpr[RA] = ea; return;
    case 40: c->gpr[RD] = rc_lhz(m, ea_d(c, w)); return;
    case 41: ea = c->gpr[RA] + SIMM; c->gpr[RD] = rc_lhz(m, ea); c->gpr[RA] = ea; return;
    case 42: c->gpr[RD] = (uint32_t)(int32_t)(int16_t)rc_lhz(m, ea_d(c, w)); return;
    case 43: ea = c->gpr[RA] + SIMM; c->gpr[RD] = (uint32_t)(int32_t)(int16_t)rc_lhz(m, ea); c->gpr[RA] = ea; return;
    case 44: rc_sth(m, ea_d(c, w), c->gpr[RD]); return;
    case 45: ea = c->gpr[RA] + SIMM; rc_sth(m, ea, c->gpr[RD]); c->gpr[RA] = ea; return;
    case 46: /* lmw */
        ea = ea_d(c, w);
        for (unsigned r = RD; r < 32; r++, ea += 4)
            c->gpr[r] = rc_lwz(m, ea);
        return;
    case 47: /* stmw */
        ea = ea_d(c, w);
        for (unsigned r = RD; r < 32; r++, ea += 4)
            rc_stw(m, ea, c->gpr[r]);
        return;
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: {
        if (!(c->msr & PPC_MSR_FP)) {
            c->pc = pc;
            fp_unavailable(m);
            return;
        }
        bool upd = op & 1;
        ea = upd ? c->gpr[RA] + SIMM : ea_d(c, w);
        switch (op & ~1u) {
        case 48: gc_fset_single(c, RD, ppc_lfs_bits(rc_lwz(m, ea))); break;
        case 50: c->ps0[RD] = ppc_u2f(rc_ld64(m, ea)); break;
        case 52: rc_stw(m, ea, ppc_stfs_bits(c->ps0[RD])); break;
        default: rc_st64(m, ea, ppc_f2u(c->ps0[RD])); break;
        }
        if (upd)
            c->gpr[RA] = ea;
        return;
    }
    case 56: case 57: case 60: case 61: { /* psq_l psq_lu psq_st psq_stu */
        if (!(c->msr & PPC_MSR_FP)) {
            c->pc = pc;
            fp_unavailable(m);
            return;
        }
        uint32_t d12 = (uint32_t)((int32_t)(w << 20) >> 20);
        bool upd = op & 1;
        ea = (upd ? c->gpr[RA] : (RA ? c->gpr[RA] : 0)) + d12;
        unsigned wbit = (w >> 15) & 1, i = (w >> 12) & 7;
        if (op >= 60)
            gc_psq_store(m, RD, ea, wbit, i);
        else
            gc_psq_load(m, RD, ea, wbit, i);
        if (upd)
            c->gpr[RA] = ea;
        return;
    }
    case 59: case 63:
        if (!(c->msr & PPC_MSR_FP)) {
            c->pc = pc;
            fp_unavailable(m);
            return;
        }
        op_fp(m, pc, w, op == 59);
        return;
    default:
        illegal(m, pc, w);
        return;
    }
}
