/* x360_interp.c -- the small interpreter behind RC_INTERP (XBA-270).
 *
 * The lifter lifts nearly everything a title runs (x360_tools: the 64-bit integer forms, FPU, all of VMX / VMX128)
 * and hands a few instructions to the runtime: traps (tw / twi / td / tdi, which MSVC emits for asserts, divide-by-
 * zero checks and debug breaks), the FPSCR moves (mffs / mtfsf / mtfsfi / mtfsb0 / mtfsb1 / mcrfs), mcrxr, and sc.
 * They run here, one at a time, then execution goes back to lifted code at the next instruction. Anything else is
 * reported (the word, its address) and stops the title: it means the lifter needs a new case, not that this file
 * should grow into a full interpreter. */
#include <fenv.h>
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

int x360_interp_one(x360_machine *x)
{
    x360_thread *t = x360_cur(x);
    xe_cpu *c = &t->cpu;
    uint32_t pc = c->pc, w = x360_r32(x, pc);
    unsigned op = w >> 26, rd = (w >> 21) & 31, ra = (w >> 16) & 31, rb = (w >> 11) & 31, xo = (w >> 1) & 0x3FF;
    int16_t simm = (int16_t)(w & 0xFFFF);
    x->st.interpreted++;
    int trap = -1;
    if (op == 3) /* twi */
        trap = trap_cond(rd, (int32_t)c->gpr[ra], simm);
    else if (op == 2) /* tdi */
        trap = trap_cond(rd, (int64_t)c->gpr[ra], simm);
    else if (op == 31 && xo == 4) /* tw */
        trap = trap_cond(rd, (int32_t)c->gpr[ra], (int32_t)c->gpr[rb]);
    else if (op == 31 && xo == 68) /* td */
        trap = trap_cond(rd, (int64_t)c->gpr[ra], (int64_t)c->gpr[rb]);
    if (trap >= 0) {
        if (trap) {
            if (op == 3 && rd == 31 && ra == 0)
                x360_fail(x, "debug break (twi 31, r0, %d) at %08X", simm, pc);
            else
                x360_fail(x, "trap at %08X (%08X): the title asserted", pc, w);
            return -1;
        }
        c->pc = pc + 4;
        return 0;
    }
    if (op == 63) {
        switch (xo) {
        case 583: /* mffs */
            c->fpr[rd] = u2d(0xFFF8000000000000ull | c->fpscr);
            break;
        case 711: { /* mtfsf FM, frB */
            unsigned fm = (w >> 17) & 0xFF;
            uint32_t v = (uint32_t)d2u(c->fpr[rb]), m = 0;
            for (int k = 0; k < 8; k++)
                if (fm & (0x80 >> k))
                    m |= 0xF0000000u >> (4 * k);
            c->fpscr = (c->fpscr & ~m) | (v & m);
            break;
        }
        case 134: { /* mtfsfi crfD, IMM */
            unsigned f = rd >> 2, imm = (w >> 12) & 15;
            uint32_t m = 0xF0000000u >> (4 * f);
            c->fpscr = (c->fpscr & ~m) | ((imm << (28 - 4 * f)) & m);
            break;
        }
        case 70: /* mtfsb0 */
            c->fpscr &= ~(0x80000000u >> rd);
            break;
        case 38: /* mtfsb1 */
            c->fpscr |= 0x80000000u >> rd;
            break;
        case 64: { /* mcrfs crfD, crfS */
            unsigned d = rd >> 2, s = ra >> 2;
            uint32_t f = (c->fpscr >> (28 - 4 * s)) & 15;
            c->cr = (c->cr & ~(0xF0000000u >> (4 * d))) | (f << (28 - 4 * d));
            c->fpscr &= ~((0xF0000000u >> (4 * s)) & 0x9FF80700u); /* the exception bits copied are cleared */
            break;
        }
        default:
            goto unknown;
        }
        if (w & 1) /* record form: CR1 from FPSCR[FX FEX VX OX] */
            c->cr = (c->cr & 0xF0FFFFFFu) | ((c->fpscr >> 4) & 0x0F000000u);
        set_rounding(c->fpscr);
        c->pc = pc + 4;
        return 0;
    }
    if (op == 31 && xo == 512) { /* mcrxr */
        unsigned d = rd >> 2;
        uint32_t f = c->xer_so << 3 | c->xer_ov << 2 | c->xer_ca << 1;
        c->cr = (c->cr & ~(0xF0000000u >> (4 * d))) | (f << (28 - 4 * d));
        c->xer_so = c->xer_ov = c->xer_ca = 0;
        c->pc = pc + 4;
        return 0;
    }
    if (op == 17) {
        x360_fail(x, "sc at %08X: system calls are the kernel's, not a title's", pc);
        return -1;
    }
unknown:
    x360_fail(x, "instruction %08X at %08X is neither lifted nor interpreted (x360_interp.c)", w, pc);
    return -1;
}
