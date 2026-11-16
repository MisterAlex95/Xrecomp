/* psp_cpu.c -- the Allegrex interpreter of the PSP spike (XBA-224): the MIPS II integer set with the Allegrex
 * additions (likely branches, MOVZ / MOVN, CLZ / CLO, MAX / MIN, MADD / MSUB(U), ROTR(V), EXT / INS, SEB / SEH /
 * WSBH / WSBW / BITREV, LL / SC, SYNC / CACHE, MFIC / MTIC) and the single-precision FPU. No load delay slot. The
 * VFPU, HALT and COP0 stop the machine with a message (the spike has none of them); so do the exceptions a user
 * program never survives on a PSP (overflow trap, address error, BREAK).
 *
 * pc / next_pc: the instruction at pc runs, then pc = next_pc and next_pc = pc + 4, or the target of a taken
 * transfer (its delay slot, at pc + 4, runs first). A likely branch not taken skips its slot. SYSCALL calls the HLE
 * after pc has moved on, so the HLE may save the context and switch threads. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "psp_internal.h"

void psp_fail(psp_machine *m, const char *fmt, ...)
{
    if (m->error[0])
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->error, sizeof m->error, fmt, ap);
    va_end(ap);
}

#define RS (h->r[(op >> 21) & 31])
#define RT (h->r[(op >> 16) & 31])
#define RT_N ((op >> 16) & 31)
#define RD_N ((op >> 11) & 31)
#define SA ((op >> 6) & 31)
#define IMM (op & 0xFFFFu)
#define SIMM ((uint32_t)(int32_t)(int16_t)(op & 0xFFFFu))
#define FS (h->f[(op >> 11) & 31])
#define FT (h->f[(op >> 16) & 31])
#define FD (h->f[(op >> 6) & 31])

static void setr(psp_rc_head *h, uint32_t r, uint32_t v)
{
    if (r)
        h->r[r] = v;
}

static void cop1(psp_machine *m, uint32_t op, uint32_t pc, uint32_t *npc, bool *likely_skip)
{
    psp_rc_head *h = &m->h;
    uint32_t fmt = (op >> 21) & 31, fsn = (op >> 11) & 31;
    switch (fmt) {
    case 0: setr(h, RT_N, rc_fbits(FS)); return;
    case 2: setr(h, RT_N, fsn == 31 ? h->fcr31 : fsn == 0 ? RC_FCR0 : 0); return;
    case 4: rc_fset(&FS, RT); return;
    case 6:
        if (fsn == 31)
            h->fcr31 = RT & 0x0183FFFFu;
        return;
    case 8: {
        uint32_t c = (h->fcr31 >> 23) & 1, tf = (op >> 16) & 1, likely = (op >> 17) & 1;
        if (c == tf) {
            *npc = pc + 4 + (SIMM << 2);
            m->branched = true;
        } else if (likely) {
            *likely_skip = true;
        }
        return;
    }
    case 16: {
        uint32_t f = op & 63;
        switch (f) {
        case 0x00: FD = FS + FT; return;
        case 0x01: FD = FS - FT; return;
        case 0x02: FD = FS * FT; return;
        case 0x03: FD = FS / FT; return;
        case 0x04: FD = rc_sqrtf(FS); return;
        case 0x05: FD = rc_fabsf(FS); return;
        case 0x06: FD = FS; return;
        case 0x07: FD = -FS; return;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: rc_fset(&FD, rc_cvt_w(FS, (int)(f & 3))); return;
        case 0x24: rc_fset(&FD, rc_cvt_w(FS, (int)(h->fcr31 & 3u))); return;
        default:
            if (f >= 0x30) {
                h->fcr31 = rc_fcmp(h->fcr31, FS, FT, (int)(f & 15));
                return;
            }
        }
        break;
    }
    case 20:
        if ((op & 63) == 0x20) {
            FD = (float)(int32_t)rc_fbits(FS);
            return;
        }
        break;
    }
    psp_fail(m, "FPU encoding 0x%08X at 0x%08X not handled", op, pc);
}

static int load(psp_machine *m, uint32_t a, int width, uint32_t *v, uint32_t pc)
{
    if (psp_read(m, a, width, v)) {
        psp_fail(m, "load%d from 0x%08X at 0x%08X: address error", width * 8, a, pc);
        return 1;
    }
    return 0;
}

static int store(psp_machine *m, uint32_t a, int width, uint32_t v, uint32_t pc)
{
    if (psp_write(m, a, width, v)) {
        psp_fail(m, "store%d to 0x%08X at 0x%08X: address error", width * 8, a, pc);
        return 1;
    }
    return 0;
}

void psp_cpu_step(psp_machine *m)
{
    psp_rc_head *h = &m->h;
    uint32_t pc = h->pc, op;
    m->branched = false;
    m->switched = false;
    if (psp_read(m, pc, 4, &op)) {
        psp_fail(m, "instruction fetch from 0x%08X", pc);
        return;
    }
    m->trace[m->trace_pos++ % PSP_TRACE] = pc;
    m->stats.interp++;
    h->cycles++;
    uint32_t npc = h->next_pc + 4; /* the one after next, unless a transfer here changes it */
    bool likely_skip = false, sys = false;
    uint32_t sys_code = 0;
    uint32_t a = RS + SIMM, v;
    switch (op >> 26) {
    case 0x00: {
        uint32_t f = op & 63;
        switch (f) {
        case 0x00: setr(h, RD_N, RT << SA); break;
        case 0x02: setr(h, RD_N, ((op >> 21) & 31) == 1 ? rc_rotr(RT, SA) : RT >> SA); break;
        case 0x03: setr(h, RD_N, (uint32_t)((int32_t)RT >> SA)); break;
        case 0x04: setr(h, RD_N, RT << (RS & 31)); break;
        case 0x06: setr(h, RD_N, SA == 1 ? rc_rotr(RT, RS & 31) : RT >> (RS & 31)); break;
        case 0x07: setr(h, RD_N, (uint32_t)((int32_t)RT >> (RS & 31))); break;
        case 0x08: npc = RS; m->branched = true; break;
        case 0x09: { uint32_t t = RS; setr(h, RD_N, pc + 8); npc = t; m->branched = true; break; }
        case 0x0A: if (RT == 0) setr(h, RD_N, RS); break;
        case 0x0B: if (RT != 0) setr(h, RD_N, RS); break;
        case 0x0C: sys = true; sys_code = (op >> 6) & 0xFFFFFu; break;
        case 0x0D: psp_fail(m, "BREAK at 0x%08X", pc); return;
        case 0x0F: break; /* SYNC */
        case 0x10: setr(h, RD_N, h->hi); break;
        case 0x11: h->hi = RS; break;
        case 0x12: setr(h, RD_N, h->lo); break;
        case 0x13: h->lo = RS; break;
        case 0x16: setr(h, RD_N, rc_clz(RS)); break;
        case 0x17: setr(h, RD_N, rc_clz(~RS)); break;
        case 0x18: {
            int64_t p = (int64_t)(int32_t)RS * (int32_t)RT;
            h->lo = (uint32_t)p;
            h->hi = (uint32_t)((uint64_t)p >> 32);
            break;
        }
        case 0x19: { uint64_t p = (uint64_t)RS * RT; h->lo = (uint32_t)p; h->hi = (uint32_t)(p >> 32); break; }
        case 0x1A: rc_div(RS, RT, &h->hi, &h->lo); break;
        case 0x1B: rc_divu(RS, RT, &h->hi, &h->lo); break;
        case 0x1C: case 0x1D: case 0x2E: case 0x2F: {
            uint64_t acc = ((uint64_t)h->hi << 32) | h->lo;
            uint64_t p = (f == 0x1C || f == 0x2E) ? (uint64_t)((int64_t)(int32_t)RS * (int32_t)RT)
                                                  : (uint64_t)RS * RT;
            acc = (f == 0x1C || f == 0x1D) ? acc + p : acc - p;
            h->lo = (uint32_t)acc;
            h->hi = (uint32_t)(acc >> 32);
            break;
        }
        case 0x20: case 0x22: {
            uint32_t x = RS, y = RT, r = f == 0x20 ? x + y : x - y;
            uint32_t ovf = f == 0x20 ? ~(x ^ y) & (x ^ r) : (x ^ y) & (x ^ r);
            if (ovf & 0x80000000u) {
                psp_fail(m, "integer overflow trap at 0x%08X", pc);
                return;
            }
            setr(h, RD_N, r);
            break;
        }
        case 0x21: setr(h, RD_N, RS + RT); break;
        case 0x23: setr(h, RD_N, RS - RT); break;
        case 0x24: setr(h, RD_N, RS & RT); break;
        case 0x25: setr(h, RD_N, RS | RT); break;
        case 0x26: setr(h, RD_N, RS ^ RT); break;
        case 0x27: setr(h, RD_N, ~(RS | RT)); break;
        case 0x2A: setr(h, RD_N, (int32_t)RS < (int32_t)RT); break;
        case 0x2B: setr(h, RD_N, RS < RT); break;
        case 0x2C: setr(h, RD_N, (int32_t)RS > (int32_t)RT ? RS : RT); break;
        case 0x2D: setr(h, RD_N, (int32_t)RS < (int32_t)RT ? RS : RT); break;
        default: psp_fail(m, "reserved SPECIAL encoding 0x%08X at 0x%08X", op, pc); return;
        }
        break;
    }
    case 0x01: {
        uint32_t rt = RT_N, s = RS;
        bool cond = (rt & 1) ? (int32_t)s >= 0 : (int32_t)s < 0;
        if ((rt & 0x0C) || (rt & 0x10 ? rt > 0x13 : 0)) {
            psp_fail(m, "reserved REGIMM encoding 0x%08X at 0x%08X", op, pc);
            return;
        }
        if (rt & 0x10)
            h->r[31] = pc + 8;
        if (cond) {
            npc = pc + 4 + (SIMM << 2);
            m->branched = true;
        } else if (rt & 2) {
            likely_skip = true;
        }
        break;
    }
    case 0x02: case 0x03:
        if (op >> 26 == 3)
            h->r[31] = pc + 8;
        npc = ((pc + 4) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
        m->branched = true;
        break;
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x14: case 0x15: case 0x16: case 0x17: {
        uint32_t k = (op >> 26) & 3, s = RS, t = RT;
        bool cond = k == 0 ? s == t : k == 1 ? s != t : k == 2 ? (int32_t)s <= 0 : (int32_t)s > 0;
        if (cond) {
            npc = pc + 4 + (SIMM << 2);
            m->branched = true;
        } else if (op >> 26 >= 0x14) {
            likely_skip = true;
        }
        break;
    }
    case 0x08: {
        uint32_t x = RS, y = SIMM, r = x + y;
        if (~(x ^ y) & (x ^ r) & 0x80000000u) {
            psp_fail(m, "integer overflow trap at 0x%08X", pc);
            return;
        }
        setr(h, RT_N, r);
        break;
    }
    case 0x09: setr(h, RT_N, RS + SIMM); break;
    case 0x0A: setr(h, RT_N, (int32_t)RS < (int32_t)SIMM); break;
    case 0x0B: setr(h, RT_N, RS < SIMM); break;
    case 0x0C: setr(h, RT_N, RS & IMM); break;
    case 0x0D: setr(h, RT_N, RS | IMM); break;
    case 0x0E: setr(h, RT_N, RS ^ IMM); break;
    case 0x0F: setr(h, RT_N, IMM << 16); break;
    case 0x11: cop1(m, op, pc, &npc, &likely_skip); break;
    case 0x1F: {
        uint32_t f = op & 63, pos = SA, msb = RD_N;
        if (f == 0x00) {
            uint32_t size = msb + 1;
            setr(h, RT_N, (RS >> pos) & (size >= 32 ? 0xFFFFFFFFu : (1u << size) - 1));
        } else if (f == 0x04 && msb >= pos) {
            uint32_t size = msb - pos + 1, mask = (size >= 32 ? 0xFFFFFFFFu : (1u << size) - 1) << pos;
            setr(h, RT_N, (RT & ~mask) | ((RS << pos) & mask));
        } else if (f == 0x20 && SA == 0x02) {
            setr(h, RD_N, rc_wsbh(RT));
        } else if (f == 0x20 && SA == 0x03) {
            setr(h, RD_N, __builtin_bswap32(RT));
        } else if (f == 0x20 && SA == 0x10) {
            setr(h, RD_N, (uint32_t)(int32_t)(int8_t)RT);
        } else if (f == 0x20 && SA == 0x14) {
            setr(h, RD_N, rc_bitrev(RT));
        } else if (f == 0x20 && SA == 0x18) {
            setr(h, RD_N, (uint32_t)(int32_t)(int16_t)RT);
        } else {
            psp_fail(m, "reserved SPECIAL3 encoding 0x%08X at 0x%08X", op, pc);
            return;
        }
        break;
    }
    case 0x20: if (load(m, a, 1, &v, pc)) return; setr(h, RT_N, (uint32_t)(int32_t)(int8_t)v); break;
    case 0x21: if (load(m, a, 2, &v, pc)) return; setr(h, RT_N, (uint32_t)(int32_t)(int16_t)v); break;
    case 0x22: if (load(m, a & ~3u, 4, &v, pc)) return; setr(h, RT_N, rc_lwl(RT, v, a)); break;
    case 0x23: case 0x30: if (load(m, a, 4, &v, pc)) return; setr(h, RT_N, v); break;
    case 0x24: if (load(m, a, 1, &v, pc)) return; setr(h, RT_N, v); break;
    case 0x25: if (load(m, a, 2, &v, pc)) return; setr(h, RT_N, v); break;
    case 0x26: if (load(m, a & ~3u, 4, &v, pc)) return; setr(h, RT_N, rc_lwr(RT, v, a)); break;
    case 0x28: if (store(m, a, 1, RT, pc)) return; break;
    case 0x29: if (store(m, a, 2, RT, pc)) return; break;
    case 0x2A: case 0x2E:
        if (psp_rc_swlr(m, a, RT, op >> 26 == 0x2E)) {
            psp_fail(m, "SWL / SWR to 0x%08X at 0x%08X: address error", a, pc);
            return;
        }
        break;
    case 0x2B: if (store(m, a, 4, RT, pc)) return; break;
    case 0x38: if (store(m, a, 4, RT, pc)) return; setr(h, RT_N, 1); break;
    case 0x2F: break; /* CACHE */
    case 0x31: if (load(m, a, 4, &v, pc)) return; rc_fset(&FT, v); break;
    case 0x39: if (store(m, a, 4, rc_fbits(FT), pc)) return; break;
    case 0x12: case 0x18: case 0x19: case 0x1B: case 0x32: case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x3A: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        psp_fail(m, "VFPU instruction 0x%08X at 0x%08X: no VFPU in the spike", op, pc);
        return;
    case 0x1C: /* SPECIAL2: MFIC / MTIC read / write the interrupt enable (nothing interrupts here); HALT stops */
        if ((op & 63) == 0x24) {
            setr(h, RT_N, m->ic);
            break;
        }
        if ((op & 63) == 0x26) {
            m->ic = RT;
            break;
        }
        psp_fail(m, "HALT 0x%08X at 0x%08X (kernel only)", op, pc);
        return;
    case 0x10: psp_fail(m, "COP0 0x%08X at 0x%08X (kernel only)", op, pc); return;
    default: psp_fail(m, "reserved encoding 0x%08X at 0x%08X", op, pc); return;
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
        psp_hle_syscall(m, sys_code);
}
