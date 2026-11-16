/* ps2_cpu.c -- the EE (R5900) interpreter of the PS2 spike (XBA-226): MIPS III integer set with 64-bit GPRs (the
 * 32-bit ops sign-extend), the R5900's additions (three-operand MULT / MULTU, MADD(U)(1), the pipeline-1
 * MULT1 / DIV1 / MFHI1 ..., MOVZ / MOVN, likely branches, LQ / SQ, the MMI ops of ps2_alu.h, the SA register), the
 * R5900's single-precision FPU (ps2_alu.h: not IEEE), and the COP0 a user program touches (MFC0 / MTC0, EI / DI).
 * No load delay slot. COP2 (VU0 macro mode), ERET / TLB ops, the MMI ops the spike does not have (parallel
 * multiplies and divides, PEXT5 / PPAC5, QFSRV ...) and the exceptions a user program never survives (overflow, a
 * trap taken, address error, BREAK) stop the machine with a message.
 *
 * pc / next_pc: the instruction at pc runs, then pc = next_pc and next_pc = pc + 4, or the target of a taken
 * transfer (its delay slot, at pc + 4, runs first). A likely branch not taken skips its slot. SYSCALL calls the HLE
 * (ps2_hle.c) after pc has moved on: v1 holds the call number, the result goes to v0. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ps2_internal.h"

void ps2_fail(ps2_machine *m, const char *fmt, ...)
{
    if (m->error[0])
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->error, sizeof m->error, fmt, ap);
    va_end(ap);
}

#define RS_N ((op >> 21) & 31)
#define RT_N ((op >> 16) & 31)
#define RD_N ((op >> 11) & 31)
#define SA ((op >> 6) & 31)
#define IMM (op & 0xFFFFu)
#define SIMM ((uint64_t)(int64_t)(int16_t)(op & 0xFFFFu))
#define RS (h->r[RS_N])
#define RT (h->r[RT_N])
#define FS (h->f[(op >> 11) & 31])
#define FT (h->f[(op >> 16) & 31])
#define FD (h->f[(op >> 6) & 31])

static void setr(ps2_rc_head *h, uint32_t r, uint64_t v)
{
    if (r)
        h->r[r] = v;
}

static ps2_q getq(ps2_rc_head *h, uint32_t r)
{
    return r ? ps2_q_make(h->r[r], h->rh[r]) : ps2_q_make(0, 0);
}

static void setq(ps2_rc_head *h, uint32_t r, ps2_q v)
{
    if (r) {
        h->r[r] = v.lo;
        h->rh[r] = v.hi;
    }
}

static int load(ps2_machine *m, uint32_t a, int width, void *v, uint32_t pc)
{
    if (ps2_read(m, a, width, v)) {
        ps2_fail(m, "load%d from 0x%08X at 0x%08X: address error", width * 8, a, pc);
        return 1;
    }
    return 0;
}

static int store(ps2_machine *m, uint32_t a, int width, const void *v, uint32_t pc)
{
    if (ps2_write(m, a, width, v)) {
        ps2_fail(m, "store%d to 0x%08X at 0x%08X: address error", width * 8, a, pc);
        return 1;
    }
    return 0;
}

static int overflow_trap(ps2_machine *m, uint32_t pc)
{
    ps2_fail(m, "integer overflow trap at 0x%08X", pc);
    return 1;
}

static void cop0(ps2_machine *m, uint32_t op, uint32_t pc)
{
    ps2_rc_head *h = &m->h;
    uint32_t sub = RS_N, rd = RD_N;
    if (sub == 0) { /* MFC0 */
        uint32_t v = rd == 9 ? (uint32_t)h->cycles : m->cop0[rd];
        setr(h, RT_N, PS2_SX32(v));
        return;
    }
    if (sub == 4) { /* MTC0 */
        m->cop0[rd] = (uint32_t)RT;
        return;
    }
    if (sub == 0x10) {
        switch (op & 63) {
        case 0x38: m->cop0[12] |= 0x10000u; return;  /* EI: Status.EIE */
        case 0x39: m->cop0[12] &= ~0x10000u; return; /* DI */
        /* TLBR / TLBWI / TLBWR / TLBP (PS2SDK's TLB patch, in kernel mode): the map is fixed, nothing to do; TLBP
         * finds no entry */
        case 0x01: case 0x02: case 0x06: return;
        case 0x08: m->cop0[0] = 0x80000000u; return;
        default: break;
        }
    }
    ps2_fail(m, "COP0 0x%08X at 0x%08X (kernel only)", op, pc);
}

static void cop1(ps2_machine *m, uint32_t op, uint32_t pc, uint32_t *npc, bool *likely_skip)
{
    ps2_rc_head *h = &m->h;
    uint32_t fmt = RS_N, fsn = (op >> 11) & 31;
    switch (fmt) {
    case 0: setr(h, RT_N, PS2_SX32(FS)); return;
    case 2: setr(h, RT_N, PS2_SX32(fsn == 31 ? h->fcr31 : fsn == 0 ? 0x2E30u : 0)); return;
    case 4: FS = (uint32_t)RT; return;
    case 6:
        if (fsn == 31)
            h->fcr31 = (uint32_t)RT;
        return;
    case 8: {
        uint32_t c = (h->fcr31 >> 23) & 1, tf = (op >> 16) & 1, likely = (op >> 17) & 1;
        if (c == tf) {
            *npc = pc + 4 + (uint32_t)(SIMM << 2);
            m->branched = true;
        } else if (likely) {
            *likely_skip = true;
        }
        return;
    }
    case 16: {
        uint32_t f = op & 63;
        switch (f) {
        case 0x00: FD = ps2_fadd(FS, FT); return;
        case 0x01: FD = ps2_fsub(FS, FT); return;
        case 0x02: FD = ps2_fmul(FS, FT); return;
        case 0x03: FD = ps2_fdiv(FS, FT); return;
        case 0x04: FD = ps2_fsqrt(FT); return;
        case 0x05: FD = FS & 0x7FFFFFFFu; return;
        case 0x06: FD = FS; return;
        case 0x07: FD = FS ^ 0x80000000u; return;
        case 0x16: FD = ps2_frsqrt(FS, FT); return;
        case 0x18: h->acc = ps2_fadd(FS, FT); return;
        case 0x19: h->acc = ps2_fsub(FS, FT); return;
        case 0x1A: h->acc = ps2_fmul(FS, FT); return;
        case 0x1C: FD = ps2_fadd(h->acc, ps2_fmul(FS, FT)); return;
        case 0x1D: FD = ps2_fsub(h->acc, ps2_fmul(FS, FT)); return;
        case 0x1E: h->acc = ps2_fadd(h->acc, ps2_fmul(FS, FT)); return;
        case 0x1F: h->acc = ps2_fsub(h->acc, ps2_fmul(FS, FT)); return;
        case 0x24: FD = ps2_cvt_w(FS); return;
        case 0x28: FD = ps2_fmax(FS, FT); return;
        case 0x29: FD = ps2_fmin(FS, FT); return;
        case 0x30: case 0x32: case 0x34: case 0x36: h->fcr31 = ps2_fcmp(h->fcr31, FS, FT, f); return;
        default: break;
        }
        break;
    }
    case 20:
        if ((op & 63) == 0x20) {
            FD = ps2_cvt_s(FS);
            return;
        }
        break;
    }
    ps2_fail(m, "FPU encoding 0x%08X at 0x%08X not handled", op, pc);
}

static void mmi(ps2_machine *m, uint32_t op, uint32_t pc)
{
    ps2_rc_head *h = &m->h;
    uint32_t f = op & 63, sa = SA;
    ps2_q s = getq(h, RS_N), t = getq(h, RT_N);
    switch (f) {
    case 0x08: case 0x28: case 0x09: case 0x29:
        switch (f << 8 | sa) {
#define X(F, S, NAME)                                                                                                \
    case (F) << 8 | (S): setq(h, RD_N, ps2_mmi_##NAME(s, t)); return;
            PS2_MMI_LIST(X)
#undef X
        case 0x09 << 8 | 0x08: setq(h, RD_N, ps2_q_make(h->hi, h->hi1)); return; /* PMFHI */
        case 0x09 << 8 | 0x09: setq(h, RD_N, ps2_q_make(h->lo, h->lo1)); return; /* PMFLO */
        case 0x29 << 8 | 0x08: h->hi = s.lo; h->hi1 = s.hi; return;              /* PMTHI */
        case 0x29 << 8 | 0x09: h->lo = s.lo; h->lo1 = s.hi; return;              /* PMTLO */
        default: break;
        }
        break;
    case 0x00: ps2_madd(RS, RT, &h->hi, &h->lo); setr(h, RD_N, h->lo); return;
    case 0x01: ps2_maddu(RS, RT, &h->hi, &h->lo); setr(h, RD_N, h->lo); return;
    case 0x20: ps2_madd(RS, RT, &h->hi1, &h->lo1); setr(h, RD_N, h->lo1); return;
    case 0x21: ps2_maddu(RS, RT, &h->hi1, &h->lo1); setr(h, RD_N, h->lo1); return;
    case 0x04: setr(h, RD_N, ps2_plzcw(RS)); return;
    case 0x10: setr(h, RD_N, h->hi1); return;
    case 0x11: h->hi1 = RS; return;
    case 0x12: setr(h, RD_N, h->lo1); return;
    case 0x13: h->lo1 = RS; return;
    case 0x18: ps2_mult(RS, RT, &h->hi1, &h->lo1); setr(h, RD_N, h->lo1); return;
    case 0x19: ps2_multu(RS, RT, &h->hi1, &h->lo1); setr(h, RD_N, h->lo1); return;
    case 0x1A: ps2_div(RS, RT, &h->hi1, &h->lo1); return;
    case 0x1B: ps2_divu(RS, RT, &h->hi1, &h->lo1); return;
    case 0x30:
        if (sa == 0 || sa == 1) {
            ps2_q lo = ps2_q_make(h->lo, h->lo1), hi = ps2_q_make(h->hi, h->hi1);
            setq(h, RD_N, sa == 0 ? ps2_pmfhl_lw(lo, hi) : ps2_pmfhl_uw(lo, hi));
            return;
        }
        break;
    case 0x31:
        if (sa == 0) {
            ps2_q lo = ps2_q_make(h->lo, h->lo1), hi = ps2_q_make(h->hi, h->hi1);
            ps2_pmthl_lw(s, &lo, &hi);
            h->lo = lo.lo;
            h->lo1 = lo.hi;
            h->hi = hi.lo;
            h->hi1 = hi.hi;
            return;
        }
        break;
    case 0x34: setq(h, RD_N, ps2_mmi_psllh(t, sa)); return;
    case 0x36: setq(h, RD_N, ps2_mmi_psrlh(t, sa)); return;
    case 0x37: setq(h, RD_N, ps2_mmi_psrah(t, sa)); return;
    case 0x3C: setq(h, RD_N, ps2_mmi_psllw(t, sa)); return;
    case 0x3E: setq(h, RD_N, ps2_mmi_psrlw(t, sa)); return;
    case 0x3F: setq(h, RD_N, ps2_mmi_psraw(t, sa)); return;
    default: break;
    }
    ps2_fail(m, "MMI encoding 0x%08X at 0x%08X not in the spike", op, pc);
}

static bool trap_cond(uint32_t k, uint64_t a, uint64_t b)
{
    switch (k) {
    case 0: return (int64_t)a >= (int64_t)b; /* TGE */
    case 1: return a >= b;                   /* TGEU */
    case 2: return (int64_t)a < (int64_t)b;  /* TLT */
    case 3: return a < b;                    /* TLTU */
    case 4: return a == b;                   /* TEQ */
    case 6: return a != b;                   /* TNE */
    default: return false;
    }
}

void ps2_cpu_step(ps2_machine *m)
{
    ps2_rc_head *h = &m->h;
    uint32_t pc = h->pc, op;
    m->branched = false;
    if (ps2_read(m, pc, 4, &op)) {
        ps2_fail(m, "instruction fetch from 0x%08X", pc);
        return;
    }
    m->trace[m->trace_pos++ % PS2_TRACE] = pc;
    m->stats.interp++;
    h->cycles++;
    uint32_t npc = h->next_pc + 4; /* the one after next, unless a transfer here changes it */
    bool likely_skip = false, sys = false;
    uint32_t a = (uint32_t)(RS + SIMM);
    uint32_t btarget = pc + 4 + (uint32_t)(SIMM << 2);
    switch (op >> 26) {
    case 0x00: {
        uint32_t f = op & 63;
        uint64_t s = RS, t = RT;
        switch (f) {
        case 0x00: setr(h, RD_N, PS2_SX32((uint32_t)t << SA)); break;
        case 0x02: setr(h, RD_N, PS2_SX32((uint32_t)t >> SA)); break;
        case 0x03: setr(h, RD_N, PS2_SX32((uint32_t)((int32_t)t >> SA))); break;
        case 0x04: setr(h, RD_N, PS2_SX32((uint32_t)t << (s & 31))); break;
        case 0x06: setr(h, RD_N, PS2_SX32((uint32_t)t >> (s & 31))); break;
        case 0x07: setr(h, RD_N, PS2_SX32((uint32_t)((int32_t)t >> (s & 31)))); break;
        case 0x08: npc = (uint32_t)s; m->branched = true; break;
        case 0x09: setr(h, RD_N, PS2_SX32(pc + 8)); npc = (uint32_t)s; m->branched = true; break;
        case 0x0A: if (t == 0) setr(h, RD_N, s); break;
        case 0x0B: if (t != 0) setr(h, RD_N, s); break;
        case 0x0C: sys = true; break;
        case 0x0D: ps2_fail(m, "BREAK at 0x%08X", pc); return;
        case 0x0F: break; /* SYNC / SYNC.P */
        case 0x10: setr(h, RD_N, h->hi); break;
        case 0x11: h->hi = s; break;
        case 0x12: setr(h, RD_N, h->lo); break;
        case 0x13: h->lo = s; break;
        case 0x14: setr(h, RD_N, t << (s & 63)); break;
        case 0x16: setr(h, RD_N, t >> (s & 63)); break;
        case 0x17: setr(h, RD_N, (uint64_t)((int64_t)t >> (s & 63))); break;
        case 0x18: ps2_mult(s, t, &h->hi, &h->lo); setr(h, RD_N, h->lo); break;
        case 0x19: ps2_multu(s, t, &h->hi, &h->lo); setr(h, RD_N, h->lo); break;
        case 0x1A: ps2_div(s, t, &h->hi, &h->lo); break;
        case 0x1B: ps2_divu(s, t, &h->hi, &h->lo); break;
        case 0x20: case 0x22: {
            uint32_t x = (uint32_t)s, y = (uint32_t)t, r = f == 0x20 ? x + y : x - y;
            uint32_t ovf = f == 0x20 ? ~(x ^ y) & (x ^ r) : (x ^ y) & (x ^ r);
            if ((ovf & 0x80000000u) && overflow_trap(m, pc))
                return;
            setr(h, RD_N, PS2_SX32(r));
            break;
        }
        case 0x21: setr(h, RD_N, PS2_SX32((uint32_t)s + (uint32_t)t)); break;
        case 0x23: setr(h, RD_N, PS2_SX32((uint32_t)s - (uint32_t)t)); break;
        case 0x24: setr(h, RD_N, s & t); break;
        case 0x25: setr(h, RD_N, s | t); break;
        case 0x26: setr(h, RD_N, s ^ t); break;
        case 0x27: setr(h, RD_N, ~(s | t)); break;
        case 0x28: setr(h, RD_N, h->sa); break;
        case 0x29: h->sa = (uint32_t)s; break;
        case 0x2A: setr(h, RD_N, (int64_t)s < (int64_t)t); break;
        case 0x2B: setr(h, RD_N, s < t); break;
        case 0x2C: case 0x2E: {
            uint64_t r = f == 0x2C ? s + t : s - t;
            uint64_t ovf = f == 0x2C ? ~(s ^ t) & (s ^ r) : (s ^ t) & (s ^ r);
            if ((ovf >> 63) && overflow_trap(m, pc))
                return;
            setr(h, RD_N, r);
            break;
        }
        case 0x2D: setr(h, RD_N, s + t); break;
        case 0x2F: setr(h, RD_N, s - t); break;
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x36:
            if (trap_cond(f & 7, s, t)) {
                ps2_fail(m, "trap (0x%08X) taken at 0x%08X", op, pc);
                return;
            }
            break;
        case 0x38: setr(h, RD_N, t << SA); break;
        case 0x3A: setr(h, RD_N, t >> SA); break;
        case 0x3B: setr(h, RD_N, (uint64_t)((int64_t)t >> SA)); break;
        case 0x3C: setr(h, RD_N, t << (SA + 32)); break;
        case 0x3E: setr(h, RD_N, t >> (SA + 32)); break;
        case 0x3F: setr(h, RD_N, (uint64_t)((int64_t)t >> (SA + 32))); break;
        default: ps2_fail(m, "reserved SPECIAL encoding 0x%08X at 0x%08X", op, pc); return;
        }
        break;
    }
    case 0x01: {
        uint32_t rt = RT_N;
        int64_t s = (int64_t)RS;
        if (rt >= 0x08 && rt <= 0x0E && rt != 0x0D) {
            uint64_t imm = SIMM;
            static const uint8_t kind[7] = {0, 1, 2, 3, 4, 5, 6}; /* TGEI TGEIU TLTI TLTIU TEQI - TNEI */
            if (trap_cond(kind[rt - 8], (uint64_t)s, imm)) {
                ps2_fail(m, "trap (0x%08X) taken at 0x%08X", op, pc);
                return;
            }
            break;
        }
        if (rt == 0x18 || rt == 0x19) { /* MTSAB / MTSAH */
            uint32_t v = ((uint32_t)s ^ IMM) & (rt == 0x18 ? 15u : 7u);
            h->sa = rt == 0x18 ? v * 8 : v * 16;
            break;
        }
        if ((rt & 0x0C) || rt > 0x13) {
            ps2_fail(m, "reserved REGIMM encoding 0x%08X at 0x%08X", op, pc);
            return;
        }
        bool cond = (rt & 1) ? s >= 0 : s < 0;
        if (rt & 0x10)
            h->r[31] = PS2_SX32(pc + 8);
        if (cond) {
            npc = btarget;
            m->branched = true;
        } else if (rt & 2) {
            likely_skip = true;
        }
        break;
    }
    case 0x02: case 0x03:
        if (op >> 26 == 3)
            h->r[31] = PS2_SX32(pc + 8);
        npc = ((pc + 4) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
        m->branched = true;
        break;
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x14: case 0x15: case 0x16: case 0x17: {
        uint32_t k = (op >> 26) & 3;
        uint64_t s = RS, t = RT;
        bool cond = k == 0 ? s == t : k == 1 ? s != t : k == 2 ? (int64_t)s <= 0 : (int64_t)s > 0;
        if (cond) {
            npc = btarget;
            m->branched = true;
        } else if (op >> 26 >= 0x14) {
            likely_skip = true;
        }
        break;
    }
    case 0x08: {
        uint32_t x = (uint32_t)RS, y = (uint32_t)SIMM, r = x + y;
        if ((~(x ^ y) & (x ^ r) & 0x80000000u) && overflow_trap(m, pc))
            return;
        setr(h, RT_N, PS2_SX32(r));
        break;
    }
    case 0x09: setr(h, RT_N, PS2_SX32((uint32_t)RS + (uint32_t)SIMM)); break;
    case 0x0A: setr(h, RT_N, (int64_t)RS < (int64_t)SIMM); break;
    case 0x0B: setr(h, RT_N, RS < SIMM); break;
    case 0x0C: setr(h, RT_N, RS & IMM); break;
    case 0x0D: setr(h, RT_N, RS | IMM); break;
    case 0x0E: setr(h, RT_N, RS ^ IMM); break;
    case 0x0F: setr(h, RT_N, PS2_SX32(IMM << 16)); break;
    case 0x10: cop0(m, op, pc); if (m->error[0]) return; break;
    case 0x11: cop1(m, op, pc, &npc, &likely_skip); if (m->error[0]) return; break;
    case 0x18: {
        uint64_t s = RS, t = SIMM, r = s + t;
        if (((~(s ^ t) & (s ^ r)) >> 63) && overflow_trap(m, pc))
            return;
        setr(h, RT_N, r);
        break;
    }
    case 0x19: setr(h, RT_N, RS + SIMM); break;
    case 0x1C: mmi(m, op, pc); if (m->error[0]) return; break;
    case 0x1A: case 0x1B: case 0x22: case 0x26: case 0x2A: case 0x2C: case 0x2D: case 0x2E: {
        static const int8_t kinds[64] = {[0x22] = 0, [0x26] = 1, [0x1A] = 2, [0x1B] = 3,
                                         [0x2A] = 4, [0x2E] = 5, [0x2C] = 6, [0x2D] = 7};
        uint64_t out = 0;
        if (ps2_rc_unaligned(m, kinds[op >> 26], a, RT, &out)) {
            ps2_fail(m, "unaligned access (0x%08X) at 0x%08X to 0x%08X: address error", op, pc, a);
            return;
        }
        if (kinds[op >> 26] < 4)
            setr(h, RT_N, out);
        break;
    }
    case 0x1E: {
        uint64_t q[2];
        if (load(m, a & ~15u, 16, q, pc))
            return;
        setq(h, RT_N, ps2_q_make(q[0], q[1]));
        break;
    }
    case 0x1F: {
        uint64_t q[2] = {RT, RT_N ? h->rh[RT_N] : 0};
        if (store(m, a & ~15u, 16, q, pc))
            return;
        break;
    }
    case 0x20: { int8_t v; if (load(m, a, 1, &v, pc)) return; setr(h, RT_N, (uint64_t)(int64_t)v); break; }
    case 0x21: { int16_t v; if (load(m, a, 2, &v, pc)) return; setr(h, RT_N, (uint64_t)(int64_t)v); break; }
    case 0x23: { int32_t v; if (load(m, a, 4, &v, pc)) return; setr(h, RT_N, (uint64_t)(int64_t)v); break; }
    case 0x24: { uint8_t v; if (load(m, a, 1, &v, pc)) return; setr(h, RT_N, v); break; }
    case 0x25: { uint16_t v; if (load(m, a, 2, &v, pc)) return; setr(h, RT_N, v); break; }
    case 0x27: { uint32_t v; if (load(m, a, 4, &v, pc)) return; setr(h, RT_N, v); break; }
    case 0x37: { uint64_t v; if (load(m, a, 8, &v, pc)) return; setr(h, RT_N, v); break; }
    case 0x28: { uint8_t v = (uint8_t)RT; if (store(m, a, 1, &v, pc)) return; break; }
    case 0x29: { uint16_t v = (uint16_t)RT; if (store(m, a, 2, &v, pc)) return; break; }
    case 0x2B: { uint32_t v = (uint32_t)RT; if (store(m, a, 4, &v, pc)) return; break; }
    case 0x3F: { uint64_t v = RT; if (store(m, a, 8, &v, pc)) return; break; }
    case 0x2F: case 0x33: break; /* CACHE, PREF */
    case 0x31: if (load(m, a, 4, &FT, pc)) return; break;
    case 0x39: if (store(m, a, 4, &FT, pc)) return; break;
    case 0x12: case 0x36: case 0x3E:
        ps2_fail(m, "COP2 (VU0 macro mode) 0x%08X at 0x%08X: no VU0 in the spike", op, pc);
        return;
    default: ps2_fail(m, "reserved encoding 0x%08X at 0x%08X", op, pc); return;
    }
    if (likely_skip) {
        /* the slot does not run: continue after it */
        h->pc = h->next_pc + 4;
        h->next_pc = h->pc + 4;
    } else {
        h->pc = h->next_pc;
        h->next_pc = npc;
    }
    if (sys)
        ps2_hle_syscall(m, (int32_t)h->r[3]);
}
