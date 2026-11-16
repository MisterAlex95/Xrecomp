/* ctr_cpu.c -- the ARM11 MPCore (ARMv6K + VFPv2) interpreter of the 3DS spike (XBA-228), user mode only: ARM and
 * Thumb, the ARMv6 additions (media instructions, LDREX / STREX and their byte / halfword / doubleword forms, CLREX,
 * UMAAL, hints, unaligned loads and stores), the VFP, CP15's thread registers and barriers. SVC calls the HLE
 * kernel (ctr_kernel.c). Not cycle-accurate: one cycle per instruction. Its semantics are the lifted code's: the
 * ARMv4T / v5TE part is the DS spike's interpreter (consoles/nds/runtime/nds_cpu.c) without banked modes and with
 * ARMv6's unaligned accesses, and the media and VFP instructions run the same static inline functions as lifted code
 * (ctr_recomp_rt.h rc_media, rc_vfp_*), so a run may switch between the two at any instruction boundary. */
#include <stdio.h>
#include <string.h>

#include "ctr_internal.h"

#define CPU (m->h.cpu)
#define MEM (m->h.mem)
#define T_BIT 0x20u

static void stop(ctr_machine *m, const char *what, uint32_t pc, uint32_t op)
{
    ctr_stop(m, "%s at 0x%08X (opcode 0x%08X)", what, pc, op);
}

uint32_t ctr_cpu_cpsr(const ctr_cpu *c)
{
    return c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28 | c->q << 27 | (c->ge & 15) << 16 | c->cpsr_ctl;
}

void ctr_cpu_set_cpsr(ctr_cpu *c, uint32_t v)
{
    c->n = v >> 31;
    c->z = (v >> 30) & 1;
    c->c = (v >> 29) & 1;
    c->v = (v >> 28) & 1;
    c->q = (v >> 27) & 1;
    c->ge = (v >> 16) & 15;
    c->cpsr_ctl = v & 0xFF;
}

static void set_pc(ctr_machine *m, uint32_t target, bool thumb)
{
    m->branched = true;
    if (thumb) {
        CPU.cpsr_ctl |= T_BIT;
        CPU.r[15] = (target & ~1u) + 4;
    } else {
        CPU.cpsr_ctl &= ~T_BIT;
        CPU.r[15] = (target & ~3u) + 8;
    }
}

static inline uint32_t ror32(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

/* ---- ARM ---- */

static bool cond_ok(const ctr_machine *m, uint32_t c)
{
    const ctr_cpu *p = &m->h.cpu;
    switch (c) {
    case 0x0: return p->z;
    case 0x1: return !p->z;
    case 0x2: return p->c;
    case 0x3: return !p->c;
    case 0x4: return p->n;
    case 0x5: return !p->n;
    case 0x6: return p->v;
    case 0x7: return !p->v;
    case 0x8: return p->c && !p->z;
    case 0x9: return !p->c || p->z;
    case 0xA: return p->n == p->v;
    case 0xB: return p->n != p->v;
    case 0xC: return !p->z && p->n == p->v;
    case 0xD: return p->z || p->n != p->v;
    default: return true;
    }
}

static uint32_t shift_imm(ctr_machine *m, uint32_t typ, uint32_t v, uint32_t amt, uint32_t *carry)
{
    switch (typ) {
    case 0:
        if (amt == 0) {
            *carry = CPU.c;
            return v;
        }
        *carry = (v >> (32 - amt)) & 1;
        return v << amt;
    case 1:
        if (amt == 0) {
            *carry = v >> 31;
            return 0;
        }
        *carry = (v >> (amt - 1)) & 1;
        return v >> amt;
    case 2:
        if (amt == 0) {
            *carry = v >> 31;
            return (uint32_t)((int32_t)v >> 31);
        }
        *carry = (v >> (amt - 1)) & 1;
        return (uint32_t)((int32_t)v >> amt);
    default:
        if (amt == 0) {
            *carry = v & 1;
            return CPU.c << 31 | v >> 1;
        }
        *carry = (v >> (amt - 1)) & 1;
        return ror32(v, amt);
    }
}

static uint32_t add_flags(ctr_machine *m, uint32_t x, uint32_t y, uint32_t cin, bool s)
{
    uint64_t w = (uint64_t)x + y + cin;
    uint32_t r = (uint32_t)w;
    if (s) {
        CPU.n = r >> 31;
        CPU.z = r == 0;
        CPU.c = (uint32_t)(w >> 32);
        CPU.v = (~(x ^ y) & (x ^ r)) >> 31;
    }
    return r;
}

static void load_pc(ctr_machine *m, uint32_t v)
{
    set_pc(m, v, v & 1); /* ARMv5+: loads to PC interwork */
}

static void arm_dp(ctr_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t opc = (op >> 21) & 15, s = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t o2, sc, a;
    if (op & (1u << 25)) {
        uint32_t rot = (op >> 7) & 0x1E;
        o2 = ror32(op & 0xFF, rot);
        sc = rot ? o2 >> 31 : CPU.c;
        a = rn == 15 ? pc + 8 : CPU.r[rn];
    } else if (op & 0x10) {
        uint32_t rm = op & 15, rs = (op >> 8) & 15;
        uint32_t vm = rm == 15 ? pc + 12 : CPU.r[rm];
        uint32_t vs = (rs == 15 ? pc + 8 : CPU.r[rs]) & 0xFF;
        o2 = rc_shift_reg((op >> 5) & 3, vm, vs, CPU.c, &sc);
        a = rn == 15 ? pc + 12 : CPU.r[rn];
    } else {
        uint32_t rm = op & 15;
        o2 = shift_imm(m, (op >> 5) & 3, rm == 15 ? pc + 8 : CPU.r[rm], (op >> 7) & 31, &sc);
        a = rn == 15 ? pc + 8 : CPU.r[rn];
    }
    bool write = !(opc >= 8 && opc <= 11);
    bool sf = s && !(rd == 15 && write);
    uint32_t res = 0;
    bool logical = false;
    switch (opc) {
    case 0x0: res = a & o2; logical = true; break;
    case 0x1: res = a ^ o2; logical = true; break;
    case 0x2: res = add_flags(m, a, ~o2, 1, sf); break;
    case 0x3: res = add_flags(m, o2, ~a, 1, sf); break;
    case 0x4: res = add_flags(m, a, o2, 0, sf); break;
    case 0x5: res = add_flags(m, a, o2, CPU.c, sf); break;
    case 0x6: res = add_flags(m, a, ~o2, CPU.c, sf); break;
    case 0x7: res = add_flags(m, o2, ~a, CPU.c, sf); break;
    case 0x8: res = a & o2; logical = true; break;
    case 0x9: res = a ^ o2; logical = true; break;
    case 0xA: res = add_flags(m, a, ~o2, 1, true); break;
    case 0xB: res = add_flags(m, a, o2, 0, true); break;
    case 0xC: res = a | o2; logical = true; break;
    case 0xD: res = o2; logical = true; break;
    case 0xE: res = a & ~o2; logical = true; break;
    default: res = ~o2; logical = true; break;
    }
    if (logical && (sf || (!write && s))) {
        CPU.n = res >> 31;
        CPU.z = res == 0;
        CPU.c = sc;
    }
    if (!write)
        return;
    if (rd == 15) {
        if (s) {
            stop(m, "exception return in user mode", pc, op);
            return;
        }
        set_pc(m, res, false); /* no interworking for data processing on ARMv6 */
        return;
    }
    CPU.r[rd] = res;
}

static void arm_psr(ctr_machine *m, uint32_t op, uint32_t pc)
{
    if ((op & 0x0FBF0FFF) == 0x010F0000) { /* MRS */
        if (op & (1u << 22)) {
            stop(m, "MRS SPSR in user mode", pc, op);
            return;
        }
        CPU.r[(op >> 12) & 15] = ctr_cpu_cpsr(&CPU);
        return;
    }
    if (op & (1u << 22)) {
        stop(m, "MSR SPSR in user mode", pc, op);
        return;
    }
    uint32_t v = (op & (1u << 25)) ? ror32(op & 0xFF, (op >> 7) & 0x1E) : CPU.r[op & 15];
    uint32_t mask = 0;
    if (op & (1u << 18))
        mask |= 0x000F0000; /* GE */
    if (op & (1u << 19))
        mask |= 0xF8000000; /* N Z C V Q */
    uint32_t cur = ctr_cpu_cpsr(&CPU);
    ctr_cpu_set_cpsr(&CPU, (cur & ~mask) | (v & mask));
}

static void arm_single(ctr_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t p = (op >> 24) & 1, u = (op >> 23) & 1, b = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t off;
    if (op & (1u << 25)) {
        uint32_t c, rm = op & 15;
        off = shift_imm(m, (op >> 5) & 3, rm == 15 ? pc + 8 : CPU.r[rm], (op >> 7) & 31, &c);
    } else {
        off = op & 0xFFF;
    }
    uint32_t base = rn == 15 ? pc + 8 : CPU.r[rn];
    uint32_t moved = u ? base + off : base - off;
    uint32_t ad = p ? moved : base;
    bool wb = !p || w;
    if (l) {
        uint32_t v = b ? ctr_rd8(MEM, ad) : ctr_rd32(MEM, ad);
        if (wb)
            CPU.r[rn] = moved;
        if (rd == 15)
            load_pc(m, v);
        else
            CPU.r[rd] = v;
    } else {
        uint32_t v = rd == 15 ? pc + 12 : CPU.r[rd];
        if (b)
            ctr_wr8(MEM, ad, v);
        else
            ctr_wr32(MEM, ad, v);
        if (wb)
            CPU.r[rn] = moved;
    }
}

static void arm_half(ctr_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t p = (op >> 24) & 1, u = (op >> 23) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
    uint32_t off = (op & (1u << 22)) ? (((op >> 4) & 0xF0) | (op & 0xF)) : CPU.r[op & 15];
    uint32_t base = rn == 15 ? pc + 8 : CPU.r[rn];
    uint32_t moved = u ? base + off : base - off;
    uint32_t ad = p ? moved : base;
    bool wb = !p || w;
    if (l) {
        uint32_t v;
        if (sh == 1)
            v = ctr_rd16(MEM, ad);
        else if (sh == 2)
            v = (uint32_t)(int32_t)(int8_t)ctr_rd8(MEM, ad);
        else
            v = (uint32_t)(int32_t)(int16_t)ctr_rd16(MEM, ad);
        if (wb)
            CPU.r[rn] = moved;
        if (rd == 15)
            load_pc(m, v);
        else
            CPU.r[rd] = v;
        return;
    }
    if (sh == 1) {
        ctr_wr16(MEM, ad, rd == 15 ? pc + 12 : CPU.r[rd]);
    } else if (sh == 2) { /* LDRD */
        uint32_t v0 = ctr_rd32(MEM, ad), v1 = ctr_rd32(MEM, ad + 4);
        if (wb)
            CPU.r[rn] = moved;
        CPU.r[rd & 14] = v0;
        CPU.r[(rd & 14) + 1] = v1;
        return;
    } else { /* STRD */
        ctr_wr32(MEM, ad, CPU.r[rd & 14]);
        ctr_wr32(MEM, ad + 4, CPU.r[(rd & 14) + 1]);
    }
    if (wb)
        CPU.r[rn] = moved;
}

static void arm_block(ctr_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t p = (op >> 24) & 1, u = (op >> 23) & 1, s = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, lst = op & 0xFFFF;
    int n = __builtin_popcount(lst);
    if (!lst || s) {
        stop(m, s ? "LDM / STM with the S bit in user mode" : "LDM / STM with an empty list", pc, op);
        return;
    }
    uint32_t base = CPU.r[rn];
    uint32_t ad = (u ? base + (p ? 4 : 0) : base - 4u * n + (p ? 0 : 4)) & ~3u;
    uint32_t nb = u ? base + 4u * n : base - 4u * n;
    if (l) {
        if (w && !(lst >> rn & 1))
            CPU.r[rn] = nb;
        uint32_t pcv = 0;
        for (int i = 0; i < 16; i++) {
            if (!(lst >> i & 1))
                continue;
            uint32_t v = ctr_rd32(MEM, ad);
            ad += 4;
            if (i == 15)
                pcv = v;
            else
                CPU.r[i] = v;
        }
        if (lst & 0x8000)
            load_pc(m, pcv);
        return;
    }
    int first = __builtin_ctz(lst);
    for (int i = 0; i < 16; i++) {
        if (!(lst >> i & 1))
            continue;
        uint32_t v = i == 15 ? pc + 12 : (i == (int)rn && w && i != first) ? nb : CPU.r[i];
        ctr_wr32(MEM, ad, v);
        ad += 4;
    }
    if (w)
        CPU.r[rn] = nb;
}

/* LDREX / STREX and the B / H / D forms: the monitor is one flag (a single core, threads switch only at SVCs and
 * events, and a switch clears it). */
static void arm_exclusive(ctr_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t kind = (op >> 21) & 3, l = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15, rt = op & 15;
    uint32_t a = CPU.r[rn];
    if (rn == 15 || rd == 15) {
        stop(m, "exclusive access with PC", pc, op);
        return;
    }
    if (l) {
        CPU.excl = 1;
        switch (kind) {
        case 0: CPU.r[rd] = ctr_rd32(MEM, a); break;
        case 1: CPU.r[rd & 14] = ctr_rd32(MEM, a); CPU.r[(rd & 14) + 1] = ctr_rd32(MEM, a + 4); break;
        case 2: CPU.r[rd] = ctr_rd8(MEM, a); break;
        default: CPU.r[rd] = ctr_rd16(MEM, a); break;
        }
        return;
    }
    if (!CPU.excl) {
        CPU.r[rd] = 1;
        return;
    }
    switch (kind) {
    case 0: ctr_wr32(MEM, a, CPU.r[rt]); break;
    case 1: ctr_wr32(MEM, a, CPU.r[rt & 14]); ctr_wr32(MEM, a + 4, CPU.r[(rt & 14) + 1]); break;
    case 2: ctr_wr8(MEM, a, CPU.r[rt]); break;
    default: ctr_wr16(MEM, a, CPU.r[rt]); break;
    }
    CPU.excl = 0;
    CPU.r[rd] = 0;
}

static void arm_media(ctr_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t f16 = (op >> 16) & 15, f12 = (op >> 12) & 15, f8 = (op >> 8) & 15, f0 = op & 15;
    if ((op & 0x0FF000D0) == 0x07400010) { /* SMLALD / SMLSLD */
        uint64_t r = rc_media_long(op, CPU.r[f12], CPU.r[f16], CPU.r[f8], CPU.r[f0]);
        CPU.r[f12] = (uint32_t)r;
        CPU.r[f16] = (uint32_t)(r >> 32);
        return;
    }
    bool known = (op & 0x0F800010) == 0x06000010 || (op & 0x0FF00030) == 0x06800010
                 || (op & 0x0FF00FF0) == 0x06800FB0 || (op & 0x0FFF0FF0) == 0x06BF0F30
                 || (op & 0x0FFF0FF0) == 0x06BF0FB0 || (op & 0x0FFF0FF0) == 0x06FF0FB0
                 || ((op & 0x0F8003F0) == 0x06800070 && ((op >> 20) & 7) != 1 && ((op >> 20) & 7) != 5)
                 || (op & 0x0FF00FF0) == 0x06A00F30 || (op & 0x0FF00FF0) == 0x06E00F30
                 || (op & 0x0FA00030) == 0x06A00010
                 || ((op & 0x0FF000D0) == 0x07000010 || (op & 0x0FF000D0) == 0x07000050)
                 || (op & 0x0FF000D0) == 0x07500010 || (op & 0x0FF000D0) == 0x075000D0
                 || (op & 0x0FF000F0) == 0x07800010;
    uint32_t pre = (op >> 20) & 7;
    if ((op & 0x0F800010) == 0x06000010 && (pre == 0 || pre == 4 || ((op >> 5) & 7) == 5 || ((op >> 5) & 7) == 6))
        known = false;
    if (!known) {
        stop(m, "undefined ARM media encoding", pc, op);
        return;
    }
    uint32_t r = rc_media(op, CPU.r[f16], CPU.r[f12], CPU.r[f8], CPU.r[f0], &CPU.ge, &CPU.q);
    CPU.r[rc_media_dest_hi(op) ? f16 : f12] = r;
}

static void arm_coproc(ctr_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t cp = (op >> 8) & 15;
    if (cp == 10 || cp == 11) {
        if (!(op & 0x10)) {
            if (!rc_vfp_dp_ok(op)) {
                stop(m, "VFP operation outside VFPv2", pc, op);
                return;
            }
            rc_vfp_dp(&CPU, op);
            return;
        }
        uint32_t rt = (op >> 12) & 15;
        if (op & (1u << 20)) {
            if (((op >> 21) & 7) == 7 && ((op >> 16) & 15) == 1 && rt == 15) { /* VMRS APSR_nzcv, FPSCR */
                CPU.n = CPU.fpscr >> 31;
                CPU.z = (CPU.fpscr >> 30) & 1;
                CPU.c = (CPU.fpscr >> 29) & 1;
                CPU.v = (CPU.fpscr >> 28) & 1;
                return;
            }
            CPU.r[rt] = rc_vfp_mrc(&CPU, op);
        } else {
            rc_vfp_mcr(&CPU, op, CPU.r[rt]);
        }
        return;
    }
    if (cp == 15 && (op & 0x10)) {
        uint32_t cn = (op >> 16) & 15, cm = op & 15, op2 = (op >> 5) & 7, rt = (op >> 12) & 15;
        if (op & (1u << 20)) { /* MRC */
            if (cn == 13 && cm == 0 && op2 == 3) {
                CPU.r[rt] = CPU.tls;
                return;
            }
        } else if (cn == 7) { /* MCR c7: cache maintenance and barriers (DSB, DMB, flush prefetch): nothing to do */
            return;
        }
    }
    stop(m, "coprocessor operation", pc, op);
}

static void arm_execute(ctr_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t cond = op >> 28;
    if (cond == 0xF) {
        if ((op & 0xFE000000) == 0xFA000000) { /* BLX imm */
            uint32_t t = pc + 8 + (uint32_t)(((int32_t)(op << 8)) >> 6) + ((op >> 23) & 2);
            CPU.r[14] = pc + 4;
            set_pc(m, t, true);
            return;
        }
        if ((op & 0xFD70F000) == 0xF550F000)
            return; /* PLD */
        if (op == 0xF57FF01F) { /* CLREX */
            CPU.excl = 0;
            return;
        }
        if ((op & 0xFFF1FE20) == 0xF1000000)
            return; /* CPS: no effect in user mode */
        if ((op & 0xFFFFFDFF) == 0xF1010000) {
            if (op & 0x200)
                stop(m, "SETEND BE", pc, op);
            return;
        }
        stop(m, "undefined ARM encoding", pc, op);
        return;
    }
    if (!cond_ok(m, cond))
        return;
    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x0F8000F0) == 0x01800090 && ((op >> 8) & 15) == 15) {
            arm_exclusive(m, op, pc);
            return;
        }
        if ((op & 0x0FF000F0) == 0x00400090) { /* UMAAL */
            uint32_t hi = (op >> 16) & 15, lo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
            uint64_t r = (uint64_t)CPU.r[rm] * CPU.r[rs] + CPU.r[hi] + CPU.r[lo];
            CPU.r[lo] = (uint32_t)r;
            CPU.r[hi] = (uint32_t)(r >> 32);
            return;
        }
        if ((op & 0x0FFFFFD0) == 0x012FFF10) { /* BX / BLX rm */
            uint32_t v = CPU.r[op & 15];
            if (op & 0x20)
                CPU.r[14] = pc + 4;
            set_pc(m, v, v & 1);
            return;
        }
        if ((op & 0x0FC000F0) == 0x00000090) { /* MUL / MLA */
            uint32_t rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
            uint32_t r = CPU.r[rm] * CPU.r[rs];
            if (op & (1u << 21))
                r += CPU.r[rn];
            CPU.r[rd] = r;
            if (op & (1u << 20)) {
                CPU.n = r >> 31;
                CPU.z = r == 0;
            }
            return;
        }
        if ((op & 0x0F8000F0) == 0x00800090) { /* UMULL / UMLAL / SMULL / SMLAL */
            uint32_t hi = (op >> 16) & 15, lo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
            uint64_t r = (op & (1u << 22)) ? (uint64_t)((int64_t)(int32_t)CPU.r[rm] * (int32_t)CPU.r[rs])
                                           : (uint64_t)CPU.r[rm] * CPU.r[rs];
            if (op & (1u << 21))
                r += (uint64_t)CPU.r[hi] << 32 | CPU.r[lo];
            CPU.r[lo] = (uint32_t)r;
            CPU.r[hi] = (uint32_t)(r >> 32);
            if (op & (1u << 20)) {
                CPU.n = (uint32_t)(r >> 63);
                CPU.z = r == 0;
            }
            return;
        }
        if ((op & 0x0FB00FF0) == 0x01000090) { /* SWP / SWPB */
            uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15, a = CPU.r[rn];
            if (op & (1u << 22)) {
                uint32_t v = ctr_rd8(MEM, a);
                ctr_wr8(MEM, a, CPU.r[rm]);
                CPU.r[rd] = v;
            } else {
                uint32_t v = ctr_rd32(MEM, a);
                ctr_wr32(MEM, a, CPU.r[rm]);
                CPU.r[rd] = v;
            }
            return;
        }
        if ((op & 0x90) == 0x90 && (op & 0x60)) {
            arm_half(m, op, pc);
            return;
        }
        if ((op & 0x0FFF0FF0) == 0x016F0F10) { /* CLZ */
            CPU.r[(op >> 12) & 15] = rc_clz(CPU.r[op & 15]);
            return;
        }
        if ((op & 0x0F900FF0) == 0x01000050) { /* QADD / QSUB / QDADD / QDSUB */
            uint32_t rd = (op >> 12) & 15, kind = (op >> 21) & 3, q = 0;
            int32_t n = (int32_t)CPU.r[(op >> 16) & 15];
            if (kind >= 2)
                n = rc_qadd(n, n, &q);
            CPU.r[rd] = (uint32_t)((kind & 1) ? rc_qsub((int32_t)CPU.r[op & 15], n, &q)
                                              : rc_qadd((int32_t)CPU.r[op & 15], n, &q));
            CPU.q |= q;
            return;
        }
        if ((op & 0x0F900090) == 0x01000080) { /* SMLAxy / SMLAWy / SMULWy / SMLALxy / SMULxy */
            uint32_t kind = (op >> 21) & 3, rd = (op >> 16) & 15, rn = (op >> 12) & 15, q = 0;
            uint32_t vm = CPU.r[op & 15], vs = CPU.r[(op >> 8) & 15];
            int32_t hx = (op & 0x20) ? (int16_t)(vm >> 16) : (int16_t)vm;
            int32_t hy = (op & 0x40) ? (int16_t)(vs >> 16) : (int16_t)vs;
            if (kind == 1) {
                int32_t prod = (int32_t)(((int64_t)(int32_t)vm * hy) >> 16);
                CPU.r[rd] = (op & 0x20) ? (uint32_t)prod : (uint32_t)rc_qadd_flag(prod, (int32_t)CPU.r[rn], &q);
            } else if (kind == 3) {
                CPU.r[rd] = (uint32_t)(hx * hy);
            } else if (kind == 0) {
                CPU.r[rd] = (uint32_t)rc_qadd_flag(hx * hy, (int32_t)CPU.r[rn], &q);
            } else {
                uint64_t acc = (uint64_t)CPU.r[rd] << 32 | CPU.r[rn];
                acc += (uint64_t)(int64_t)(hx * hy);
                CPU.r[rn] = (uint32_t)acc;
                CPU.r[rd] = (uint32_t)(acc >> 32);
            }
            CPU.q |= q;
            return;
        }
        if ((op & 0x0FF000F0) == 0x01200070) {
            stop(m, "BKPT", pc, op);
            return;
        }
        if ((op & 0x01900000) == 0x01000000) {
            if ((op & 0x0FBF0FFF) == 0x010F0000 || (op & 0x0DB0F000) == 0x0120F000) {
                arm_psr(m, op, pc);
                return;
            }
            stop(m, "undefined ARM encoding", pc, op);
            return;
        }
        arm_dp(m, op, pc);
        return;
    case 1:
        if ((op & 0x0FFFFF00) == 0x0320F000)
            return; /* NOP, YIELD, WFE, WFI, SEV: hints (one core, threads switch at SVCs) */
        if ((op & 0x01900000) == 0x01000000) {
            if ((op & 0x0FB0F000) == 0x0320F000) {
                arm_psr(m, op, pc);
                return;
            }
            stop(m, "undefined ARM encoding", pc, op);
            return;
        }
        arm_dp(m, op, pc);
        return;
    case 2:
        arm_single(m, op, pc);
        return;
    case 3:
        if (op & 0x10) {
            arm_media(m, op, pc);
            return;
        }
        arm_single(m, op, pc);
        return;
    case 4:
        arm_block(m, op, pc);
        return;
    case 5: {
        uint32_t t = pc + 8 + (uint32_t)(((int32_t)(op << 8)) >> 6);
        if (op & (1u << 24))
            CPU.r[14] = pc + 4;
        set_pc(m, t, false);
        return;
    }
    case 6: {
        uint32_t cp = (op >> 8) & 15;
        if (cp != 10 && cp != 11) {
            stop(m, "coprocessor load / store", pc, op);
            return;
        }
        if ((op & 0x0FE00000) == 0x0C400000) { /* MCRR / MRRC: VMOV two core registers */
            uint32_t rt = (op >> 12) & 15, rt2 = (op >> 16) & 15;
            if (op & (1u << 20)) {
                uint64_t v = rc_vfp_mrrc(&CPU, op);
                CPU.r[rt] = (uint32_t)v;
                CPU.r[rt2] = (uint32_t)(v >> 32);
            } else {
                rc_vfp_mcrr(&CPU, op, CPU.r[rt], CPU.r[rt2]);
            }
            return;
        }
        uint32_t rn = (op >> 16) & 15;
        if (rn == 15 && (op & (1u << 21))) {
            stop(m, "VFP load / store writing back PC", pc, op);
            return;
        }
        uint32_t base = rn == 15 ? ((pc + 8) & ~3u) : CPU.r[rn];
        uint32_t nb = rc_vfp_ls(&CPU, MEM, op, base);
        if (rn != 15)
            CPU.r[rn] = nb;
        return;
    }
    default:
        if (op & (1u << 24)) { /* SVC: the HLE kernel, PC already past it */
            CPU.r[15] = pc + 12;
            m->branched = true;
            m->stats.svcs++;
            ctr_svc(m, op & 0xFF);
            return;
        }
        arm_coproc(m, op, pc);
        return;
    }
}

/* ---- Thumb (ARMv5TE's; the ARMv6 Thumb additions are not handled: libctru builds in ARM state) ---- */

static void thumb_execute(ctr_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t k = op >> 11;
    if (k <= 2) {
        uint32_t rd = op & 7, rs = (op >> 3) & 7, amt = (op >> 6) & 31, c;
        uint32_t r = shift_imm(m, k, CPU.r[rs], amt, &c);
        CPU.n = r >> 31;
        CPU.z = r == 0;
        CPU.c = c;
        CPU.r[rd] = r;
        return;
    }
    if (k == 3) {
        uint32_t rd = op & 7, rs = (op >> 3) & 7, x = (op >> 6) & 7;
        uint32_t b = (op & 0x400) ? x : CPU.r[x];
        CPU.r[rd] = (op & 0x200) ? add_flags(m, CPU.r[rs], ~b, 1, true) : add_flags(m, CPU.r[rs], b, 0, true);
        return;
    }
    if (k <= 7) {
        uint32_t rd = (op >> 8) & 7, imm = op & 0xFF;
        switch (k) {
        case 4: CPU.r[rd] = imm; CPU.n = 0; CPU.z = imm == 0; break;
        case 5: add_flags(m, CPU.r[rd], ~imm, 1, true); break;
        case 6: CPU.r[rd] = add_flags(m, CPU.r[rd], imm, 0, true); break;
        default: CPU.r[rd] = add_flags(m, CPU.r[rd], ~imm, 1, true); break;
        }
        return;
    }
    if (k == 8) {
        if (!(op & 0x400)) {
            uint32_t rd = op & 7, rs = (op >> 3) & 7, a = CPU.r[rd], b = CPU.r[rs], r = 0, c;
            bool wr = true;
            switch ((op >> 6) & 15) {
            case 0x0: r = a & b; break;
            case 0x1: r = a ^ b; break;
            case 0x2: r = rc_shift_reg(0, a, b & 0xFF, CPU.c, &c); CPU.c = c; break;
            case 0x3: r = rc_shift_reg(1, a, b & 0xFF, CPU.c, &c); CPU.c = c; break;
            case 0x4: r = rc_shift_reg(2, a, b & 0xFF, CPU.c, &c); CPU.c = c; break;
            case 0x5: CPU.r[rd] = add_flags(m, a, b, CPU.c, true); return;
            case 0x6: CPU.r[rd] = add_flags(m, a, ~b, CPU.c, true); return;
            case 0x7: r = rc_shift_reg(3, a, b & 0xFF, CPU.c, &c); CPU.c = c; break;
            case 0x8: r = a & b; wr = false; break;
            case 0x9: CPU.r[rd] = add_flags(m, 0, ~b, 1, true); return;
            case 0xA: add_flags(m, a, ~b, 1, true); return;
            case 0xB: add_flags(m, a, b, 0, true); return;
            case 0xC: r = a | b; break;
            case 0xD: r = a * b; break;
            case 0xE: r = a & ~b; break;
            default: r = ~b; break;
            }
            CPU.n = r >> 31;
            CPU.z = r == 0;
            if (wr)
                CPU.r[rd] = r;
            return;
        }
        uint32_t rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15, kk = (op >> 8) & 3;
        uint32_t s = rs == 15 ? pc + 4 : CPU.r[rs];
        if (kk == 3) {
            if (op & 0x80)
                CPU.r[14] = (pc + 2) | 1;
            set_pc(m, s, s & 1);
            return;
        }
        uint32_t d = rd == 15 ? pc + 4 : CPU.r[rd];
        if (kk == 1) {
            add_flags(m, d, ~s, 1, true);
            return;
        }
        uint32_t r = kk == 0 ? d + s : s;
        if (rd == 15)
            set_pc(m, r, true);
        else
            CPU.r[rd] = r;
        return;
    }
    if (k == 9) {
        CPU.r[(op >> 8) & 7] = ctr_rd32(MEM, ((pc + 4) & ~2u) + (op & 0xFF) * 4);
        return;
    }
    if (k == 0xA || k == 0xB) {
        uint32_t rd = op & 7, ad = CPU.r[(op >> 3) & 7] + CPU.r[(op >> 6) & 7];
        switch ((op >> 9) & 7) {
        case 0: ctr_wr32(MEM, ad, CPU.r[rd]); break;
        case 1: ctr_wr16(MEM, ad, CPU.r[rd]); break;
        case 2: ctr_wr8(MEM, ad, CPU.r[rd]); break;
        case 3: CPU.r[rd] = (uint32_t)(int32_t)(int8_t)ctr_rd8(MEM, ad); break;
        case 4: CPU.r[rd] = ctr_rd32(MEM, ad); break;
        case 5: CPU.r[rd] = ctr_rd16(MEM, ad); break;
        case 6: CPU.r[rd] = ctr_rd8(MEM, ad); break;
        default: CPU.r[rd] = (uint32_t)(int32_t)(int16_t)ctr_rd16(MEM, ad); break;
        }
        return;
    }
    if (k <= 0xF) {
        uint32_t rd = op & 7, off = (op >> 6) & 31, rb = (op >> 3) & 7;
        bool byte = op & 0x1000;
        uint32_t ad = CPU.r[rb] + (byte ? off : off * 4);
        if (op & 0x800)
            CPU.r[rd] = byte ? ctr_rd8(MEM, ad) : ctr_rd32(MEM, ad);
        else if (byte)
            ctr_wr8(MEM, ad, CPU.r[rd]);
        else
            ctr_wr32(MEM, ad, CPU.r[rd]);
        return;
    }
    if (k <= 0x11) {
        uint32_t rd = op & 7, ad = CPU.r[(op >> 3) & 7] + ((op >> 6) & 31) * 2;
        if (op & 0x800)
            CPU.r[rd] = ctr_rd16(MEM, ad);
        else
            ctr_wr16(MEM, ad, CPU.r[rd]);
        return;
    }
    if (k <= 0x13) {
        uint32_t rd = (op >> 8) & 7, ad = CPU.r[13] + (op & 0xFF) * 4;
        if (op & 0x800)
            CPU.r[rd] = ctr_rd32(MEM, ad);
        else
            ctr_wr32(MEM, ad, CPU.r[rd]);
        return;
    }
    if (k == 0x14) {
        CPU.r[(op >> 8) & 7] = ((pc + 4) & ~2u) + (op & 0xFF) * 4;
        return;
    }
    if (k == 0x15) {
        CPU.r[(op >> 8) & 7] = CPU.r[13] + (op & 0xFF) * 4;
        return;
    }
    if (k == 0x16 || k == 0x17) {
        if ((op & 0xF00) == 0) {
            uint32_t off = (op & 0x7F) * 4;
            CPU.r[13] = (op & 0x80) ? CPU.r[13] - off : CPU.r[13] + off;
            return;
        }
        if ((op & 0x600) == 0x400) {
            uint32_t lst = op & 0xFF, extra = (op >> 8) & 1;
            if (op & 0x800) {
                uint32_t ad = CPU.r[13];
                for (int i = 0; i < 8; i++)
                    if (lst >> i & 1) {
                        CPU.r[i] = ctr_rd32(MEM, ad);
                        ad += 4;
                    }
                uint32_t pcv = 0;
                if (extra) {
                    pcv = ctr_rd32(MEM, ad);
                    ad += 4;
                }
                CPU.r[13] = ad;
                if (extra)
                    load_pc(m, pcv);
            } else {
                uint32_t n = (uint32_t)__builtin_popcount(lst) + extra;
                uint32_t ad = CPU.r[13] - 4 * n;
                CPU.r[13] = ad;
                for (int i = 0; i < 8; i++)
                    if (lst >> i & 1) {
                        ctr_wr32(MEM, ad, CPU.r[i]);
                        ad += 4;
                    }
                if (extra)
                    ctr_wr32(MEM, ad, CPU.r[14]);
            }
            return;
        }
        stop(m, (op >> 8) == 0xBE ? "BKPT" : "undefined Thumb encoding", pc, op);
        return;
    }
    if (k == 0x18 || k == 0x19) {
        uint32_t rb = (op >> 8) & 7, lst = op & 0xFF;
        if (!lst) {
            stop(m, "Thumb LDM / STM with an empty list", pc, op);
            return;
        }
        uint32_t ad = CPU.r[rb], nb = ad + 4u * (uint32_t)__builtin_popcount(lst);
        if (op & 0x800) {
            if (!(lst >> rb & 1))
                CPU.r[rb] = nb;
            for (int i = 0; i < 8; i++)
                if (lst >> i & 1) {
                    CPU.r[i] = ctr_rd32(MEM, ad);
                    ad += 4;
                }
        } else {
            int first = __builtin_ctz(lst);
            for (int i = 0; i < 8; i++)
                if (lst >> i & 1) {
                    ctr_wr32(MEM, ad, (i == (int)rb && i != first) ? nb : CPU.r[i]);
                    ad += 4;
                }
            CPU.r[rb] = nb;
        }
        return;
    }
    if (k == 0x1A || k == 0x1B) {
        uint32_t c = (op >> 8) & 15;
        if (c == 15) {
            CPU.r[15] = pc + 6;
            m->branched = true;
            m->stats.svcs++;
            ctr_svc(m, op & 0xFF);
            return;
        }
        if (c == 14) {
            stop(m, "undefined Thumb encoding", pc, op);
            return;
        }
        if (cond_ok(m, c))
            set_pc(m, pc + 4 + (uint32_t)((int32_t)(int8_t)(op & 0xFF) * 2), true);
        return;
    }
    if (k == 0x1C) {
        set_pc(m, pc + 4 + (uint32_t)(((int32_t)(op << 21)) >> 20), true);
        return;
    }
    if (k == 0x1E) {
        CPU.r[14] = pc + 4 + (uint32_t)(((int32_t)(op << 21)) >> 9);
        return;
    }
    if (k == 0x1F) {
        uint32_t t = CPU.r[14] + (op & 0x7FF) * 2;
        CPU.r[14] = (pc + 2) | 1;
        set_pc(m, t, true);
        return;
    }
    if (k == 0x1D) {
        uint32_t t = (CPU.r[14] + (op & 0x7FF) * 2) & ~3u;
        CPU.r[14] = (pc + 2) | 1;
        set_pc(m, t, false);
        return;
    }
    stop(m, "undefined Thumb encoding", pc, op);
}

void ctr_cpu_step(ctr_machine *m)
{
    bool thumb = CPU.cpsr_ctl & T_BIT;
    uint32_t pc = CPU.r[15] - (thumb ? 4 : 8);
    m->h.now += 1;
    m->trace[m->trace_at++ & 63] = pc | thumb;
    if (pc - m->code_lo >= m->code_hi - m->code_lo) { /* not in the 3DSX's code: is it executable at all? */
        const ctr_region *r = ctr_mem_region(m, pc);
        if (!r || !(r->perm & 4)) {
            stop(m, "jump outside executable memory", pc, 0);
            return;
        }
    }
    if (thumb) {
        uint32_t op = ctr_rd16(MEM, pc);
        CPU.r[15] = pc + 4;
        m->branched = false;
        thumb_execute(m, op, pc);
        if (!m->branched)
            CPU.r[15] = pc + 6;
    } else {
        uint32_t op = ctr_rd32(MEM, pc);
        CPU.r[15] = pc + 8;
        m->branched = false;
        arm_execute(m, op, pc);
        if (!m->branched)
            CPU.r[15] = pc + 12;
    }
}
