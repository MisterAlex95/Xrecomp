/* nds_cpu.c -- the ARM946E-S (ARMv5TE) interpreter of the DS spike (XBA-223): ARM and Thumb, banked modes,
 * exceptions, CP15 (control, TCM regions, wait for interrupt), and the BIOS in C (HLE). Not cycle-accurate: one
 * cycle per instruction, as lifted code counts (nds_recomp_rt.h). Its semantics are the lifted code's: same register
 * results and flags for every encoding the lifter emits (gba_tools + nds_tools/lift.py), so a run may switch between
 * the two at any instruction boundary. */
#include <stdio.h>
#include <string.h>

#include "nds_internal.h"

#define CPU (m->h.cpu)
#define T_BIT 0x20u
#define I_BIT 0x80u

static int bank_of(uint32_t mode)
{
    switch (mode & 0x1F) {
    case 0x11: return 1; /* FIQ */
    case 0x12: return 2; /* IRQ */
    case 0x13: return 3; /* SVC */
    case 0x17: return 4; /* ABT */
    case 0x1B: return 5; /* UND */
    default: return 0;   /* USR / SYS */
    }
}

static void stop(nds_machine *m, const char *what, uint32_t pc, uint32_t op)
{
    if (!m->error[0])
        snprintf(m->error, sizeof m->error, "%s at 0x%08X (opcode 0x%08X)", what, pc, op);
}

uint32_t nds_cpu_cpsr(const nds_machine *m)
{
    const nds_cpu *c = &m->h.cpu;
    return c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28 | c->q << 27 | c->cpsr_ctl;
}

static void switch_mode(nds_machine *m, uint32_t newmode)
{
    uint32_t old = CPU.cpsr_ctl & 0x1F;
    int ob = bank_of(old), nb = bank_of(newmode);
    if (ob == nb)
        return;
    CPU.bank_r13[ob] = CPU.r[13];
    CPU.bank_r14[ob] = CPU.r[14];
    if (ob == 1) {
        memcpy(CPU.fiq_r8_12, &CPU.r[8], sizeof CPU.fiq_r8_12);
        memcpy(&CPU.r[8], CPU.usr_r8_12, sizeof CPU.usr_r8_12);
    } else if (nb == 1) {
        memcpy(CPU.usr_r8_12, &CPU.r[8], sizeof CPU.usr_r8_12);
        memcpy(&CPU.r[8], CPU.fiq_r8_12, sizeof CPU.fiq_r8_12);
    }
    CPU.r[13] = CPU.bank_r13[nb];
    CPU.r[14] = CPU.bank_r14[nb];
}

void nds_cpu_set_cpsr(nds_machine *m, uint32_t v)
{
    switch_mode(m, v & 0x1F);
    CPU.n = v >> 31;
    CPU.z = (v >> 30) & 1;
    CPU.c = (v >> 29) & 1;
    CPU.v = (v >> 28) & 1;
    CPU.q = (v >> 27) & 1;
    CPU.cpsr_ctl = v & 0xFF;
}

static void set_pc(nds_machine *m, uint32_t target, bool thumb)
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

static void exception(nds_machine *m, uint32_t mode, uint32_t vector, uint32_t lr)
{
    uint32_t cpsr = nds_cpu_cpsr(m);
    switch_mode(m, mode);
    CPU.spsr[bank_of(mode)] = cpsr;
    CPU.r[14] = lr;
    CPU.cpsr_ctl = (CPU.cpsr_ctl & ~0x3Fu & ~T_BIT) | mode | I_BIT;
    uint32_t base = (m->cp15_ctl & (1u << 13)) ? 0xFFFF0000u : 0;
    set_pc(m, base + vector, false);
    CPU.halted = false;
}

void nds_cpu_irq(nds_machine *m)
{
    uint32_t next = CPU.r[15] - ((CPU.cpsr_ctl & T_BIT) ? 4 : 8);
    exception(m, 0x12, 0x18, next + 4);
}

void nds_cpu_reset(nds_machine *m, uint32_t entry)
{
    memset(&CPU, 0, sizeof CPU);
    CPU.cpsr_ctl = 0xD3; /* SVC, IRQ and FIQ off */
    /* What the firmware leaves for a direct boot (GBATEK « DS Firmware Boot »). */
    CPU.bank_r13[0] = 0x03002F7C; /* USR / SYS */
    CPU.bank_r13[2] = 0x03003F80; /* IRQ */
    CPU.bank_r13[3] = 0x03003FC0; /* SVC */
    CPU.r[13] = 0x03003FC0;
    CPU.r[12] = CPU.r[14] = entry;
    set_pc(m, entry, entry & 1);
}

/* ---- memory through the bus ---- */

static inline uint32_t rd32(nds_machine *m, uint32_t a) { return nds_read32(m, a); }
static inline void wr32(nds_machine *m, uint32_t a, uint32_t v) { nds_write32(m, a, v); }

static inline uint32_t ror32(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

/* ---- CP15 ---- */

static void cp15_update(nds_machine *m)
{
    m->dtcm_base = m->cp15_dtcm & 0xFFFFF000u;
    m->dtcm_size = 512u << ((m->cp15_dtcm >> 1) & 0x1F);
    m->itcm_size = 512u << ((m->cp15_itcm >> 1) & 0x1F);
}

static uint32_t cp15_read(nds_machine *m, uint32_t cn, uint32_t cm, uint32_t op2)
{
    switch (cn) {
    case 0: return op2 == 1 ? 0x0F0D2112u : op2 == 2 ? 0x00140180u : 0x41059461u;
    case 1: return m->cp15_ctl;
    case 9:
        if (cm == 1)
            return op2 ? m->cp15_itcm : m->cp15_dtcm;
        return 0;
    case 13: return m->cp15_c13[op2 & 1];
    default: return m->cp15_misc[cn & 15];
    }
}

static void cp15_write(nds_machine *m, uint32_t cn, uint32_t cm, uint32_t op2, uint32_t v)
{
    switch (cn) {
    case 1:
        m->cp15_ctl = (v & 0x000FF085u) | 0x78u;
        cp15_update(m);
        break;
    case 7:
        if ((cm == 0 && op2 == 4) || (cm == 8 && op2 == 2))
            CPU.halted = true; /* wait for interrupt */
        break;                 /* cache and write buffer operations: nothing to do */
    case 9:
        if (cm == 1) {
            if (op2 == 0)
                m->cp15_dtcm = v;
            else if (op2 == 1)
                m->cp15_itcm = v;
            cp15_update(m);
        }
        break;
    case 13: m->cp15_c13[op2 & 1] = v; break;
    default: m->cp15_misc[cn & 15] = v; break; /* MPU regions, cache config: kept, not enforced */
    }
}

/* ---- the BIOS (HLE) ---- */

static void bios_return(nds_machine *m)
{
    uint32_t spsr = CPU.spsr[bank_of(CPU.cpsr_ctl)];
    uint32_t lr = CPU.r[14];
    nds_cpu_set_cpsr(m, spsr);
    set_pc(m, lr, (spsr & T_BIT) != 0);
}

static void bios_swi(nds_machine *m)
{
    uint32_t spsr = CPU.spsr[bank_of(CPU.cpsr_ctl)];
    uint32_t lr = CPU.r[14];
    uint32_t num = (spsr & T_BIT) ? nds_read16(m, lr - 2) & 0xFF : (rd32(m, lr - 4) >> 16) & 0xFF;
    switch (num) {
    case 0x03: /* WaitByLoop */
        m->h.now += CPU.r[0] * 4u;
        break;
    case 0x04: /* IntrWait (r0: discard old flags, r1: mask) */
    case 0x05: { /* VBlankIntrWait */
        uint32_t mask = num == 5 ? 1u : CPU.r[1];
        bool discard = num == 5 ? true : CPU.r[0] != 0;
        uint32_t flags_at = m->dtcm_base + 0x3FF8;
        if (m->swi_wait_first && discard) {
            nds_write32(m, flags_at, rd32(m, flags_at) & ~mask);
            m->swi_wait_first = false;
        }
        uint32_t f = rd32(m, flags_at);
        if (!(f & mask) || (m->swi_wait_first && discard)) {
            m->swi_wait_first = false;
            m->ime = 1;
            nds_irq_update(m);
            CPU.cpsr_ctl &= ~I_BIT;   /* the BIOS waits with IRQs on: the IRQ returns here and checks again */
            CPU.halted = true;
            return;                    /* PC stays at the SWI vector */
        }
        nds_write32(m, flags_at, f & ~mask);
        m->swi_wait_first = true;
        break;
    }
    case 0x06: /* Halt */
        CPU.halted = true;
        break;
    case 0x09: { /* Div */
        int32_t n = (int32_t)CPU.r[0], d = (int32_t)CPU.r[1];
        if (d == 0) {
            CPU.r[0] = n < 0 ? 0xFFFFFFFFu : 1;
            CPU.r[1] = (uint32_t)n;
        } else if (n == INT32_MIN && d == -1) {
            CPU.r[0] = (uint32_t)INT32_MIN;
            CPU.r[1] = 0;
        } else {
            CPU.r[0] = (uint32_t)(n / d);
            CPU.r[1] = (uint32_t)(n % d);
        }
        CPU.r[3] = (uint32_t)((int32_t)CPU.r[0] < 0 ? -(int32_t)CPU.r[0] : (int32_t)CPU.r[0]);
        break;
    }
    case 0x0B: { /* CpuSet */
        uint32_t s = CPU.r[0], d = CPU.r[1], cnt = CPU.r[2] & 0x1FFFFF;
        bool fill = (CPU.r[2] >> 24) & 1, word = (CPU.r[2] >> 26) & 1;
        for (uint32_t i = 0; i < cnt; i++) {
            if (word) {
                wr32(m, d, rd32(m, s));
                d += 4;
                if (!fill)
                    s += 4;
            } else {
                nds_write16(m, d, nds_read16(m, s));
                d += 2;
                if (!fill)
                    s += 2;
            }
        }
        break;
    }
    case 0x0C: { /* CpuFastSet */
        uint32_t s = CPU.r[0], d = CPU.r[1], cnt = ((CPU.r[2] & 0x1FFFFF) + 7) & ~7u;
        bool fill = (CPU.r[2] >> 24) & 1;
        for (uint32_t i = 0; i < cnt; i++) {
            wr32(m, d, rd32(m, s));
            d += 4;
            if (!fill)
                s += 4;
        }
        break;
    }
    case 0x0D: { /* Sqrt */
        uint32_t v = CPU.r[0], r = 0, bit = 1u << 30;
        while (bit > v)
            bit >>= 2;
        while (bit) {
            if (v >= r + bit) {
                v -= r + bit;
                r = (r >> 1) + bit;
            } else {
                r >>= 1;
            }
            bit >>= 2;
        }
        CPU.r[0] = r;
        break;
    }
    case 0x0F: /* IsDebugger */
        CPU.r[0] = 0;
        break;
    default:
        stop(m, "unsupported BIOS SWI", lr, num);
        return;
    }
    bios_return(m);
}

/* The BIOS IRQ handler (high vectors): r0-r3, r12, lr pushed, the user handler at DTCM + 0x3FFC called. */
static void bios_irq(nds_machine *m)
{
    uint32_t sp = CPU.r[13] - 24;
    uint32_t regs[6] = {CPU.r[0], CPU.r[1], CPU.r[2], CPU.r[3], CPU.r[12], CPU.r[14]};
    for (int i = 0; i < 6; i++)
        wr32(m, sp + 4u * i, regs[i]);
    CPU.r[13] = sp;
    CPU.r[0] = 0x04000000;
    CPU.r[14] = NDS_BIOS_IRQ_RET;
    uint32_t handler = rd32(m, m->dtcm_base + 0x3FFC);
    set_pc(m, handler, handler & 1);
}

static void bios_irq_return(nds_machine *m)
{
    uint32_t sp = CPU.r[13];
    CPU.r[0] = rd32(m, sp);
    CPU.r[1] = rd32(m, sp + 4);
    CPU.r[2] = rd32(m, sp + 8);
    CPU.r[3] = rd32(m, sp + 12);
    CPU.r[12] = rd32(m, sp + 16);
    CPU.r[14] = rd32(m, sp + 20);
    CPU.r[13] = sp + 24;
    uint32_t spsr = CPU.spsr[bank_of(CPU.cpsr_ctl)];
    uint32_t ret = CPU.r[14] - 4;
    nds_cpu_set_cpsr(m, spsr);
    set_pc(m, ret, (spsr & T_BIT) != 0);
}

/* ---- ARM ---- */

static bool cond_ok(const nds_machine *m, uint32_t c)
{
    const nds_cpu *p = &m->h.cpu;
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

static uint32_t shift_imm(nds_machine *m, uint32_t typ, uint32_t v, uint32_t amt, uint32_t *carry)
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

static uint32_t add_flags(nds_machine *m, uint32_t x, uint32_t y, uint32_t cin, bool s)
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

/* Writes PC from a load (ARMv5: bit 0 picks Thumb). */
static void load_pc(nds_machine *m, uint32_t v)
{
    set_pc(m, v, v & 1);
}

static void arm_dp(nds_machine *m, uint32_t op, uint32_t pc)
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
        if (s) { /* MOVS pc / SUBS pc: CPSR = SPSR */
            uint32_t spsr = CPU.spsr[bank_of(CPU.cpsr_ctl)];
            nds_cpu_set_cpsr(m, spsr);
            set_pc(m, res, (spsr & T_BIT) != 0);
        } else {
            set_pc(m, res, false); /* no interworking for data processing on ARMv5 */
        }
        return;
    }
    CPU.r[rd] = res;
}

static void arm_psr(nds_machine *m, uint32_t op, uint32_t pc)
{
    bool spsr = (op >> 22) & 1;
    if ((op & 0x0FBF0FFF) == 0x010F0000) { /* MRS */
        uint32_t rd = (op >> 12) & 15;
        CPU.r[rd] = spsr ? CPU.spsr[bank_of(CPU.cpsr_ctl)] : nds_cpu_cpsr(m);
        return;
    }
    uint32_t v = (op & (1u << 25)) ? ror32(op & 0xFF, (op >> 7) & 0x1E) : CPU.r[op & 15];
    uint32_t mask = 0;
    if (op & (1u << 16))
        mask |= 0x000000FF;
    if (op & (1u << 17))
        mask |= 0x0000FF00;
    if (op & (1u << 18))
        mask |= 0x00FF0000;
    if (op & (1u << 19))
        mask |= 0xFF000000;
    (void)pc;
    if (spsr) {
        int b = bank_of(CPU.cpsr_ctl);
        if (b)
            CPU.spsr[b] = (CPU.spsr[b] & ~mask) | (v & mask);
        return;
    }
    if ((CPU.cpsr_ctl & 0x1F) == 0x10)
        mask &= 0xFF000000; /* user mode: flags only */
    uint32_t cur = nds_cpu_cpsr(m);
    uint32_t nv = (cur & ~mask) | (v & mask);
    nv = (nv & ~T_BIT) | (cur & T_BIT); /* MSR does not change the state */
    nds_cpu_set_cpsr(m, nv);
}

static void arm_single(nds_machine *m, uint32_t op, uint32_t pc)
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
        uint32_t v = b ? nds_read8(m, ad) : ror32(rd32(m, ad), (ad & 3) * 8);
        if (wb)
            CPU.r[rn] = moved;
        if (rd == 15)
            load_pc(m, v);
        else
            CPU.r[rd] = v;
    } else {
        uint32_t v = rd == 15 ? pc + 12 : CPU.r[rd];
        if (b)
            nds_write8(m, ad, (uint8_t)v);
        else
            wr32(m, ad, v);
        if (wb)
            CPU.r[rn] = moved;
    }
}

static void arm_half(nds_machine *m, uint32_t op, uint32_t pc)
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
            v = ror32(nds_read16(m, ad), (ad & 1) * 8);
        else if (sh == 2)
            v = (uint32_t)(int32_t)(int8_t)nds_read8(m, ad);
        else
            v = (ad & 1) ? (uint32_t)(int32_t)(int8_t)nds_read8(m, ad) : (uint32_t)(int32_t)(int16_t)nds_read16(m, ad);
        if (wb)
            CPU.r[rn] = moved;
        if (rd == 15)
            load_pc(m, v);
        else
            CPU.r[rd] = v;
        return;
    }
    if (sh == 1) {
        nds_write16(m, ad, (uint16_t)(rd == 15 ? pc + 12 : CPU.r[rd]));
    } else if (sh == 2) { /* LDRD */
        uint32_t v0 = rd32(m, ad), v1 = rd32(m, ad + 4);
        if (wb)
            CPU.r[rn] = moved;
        CPU.r[rd & 14] = v0;
        CPU.r[(rd & 14) + 1] = v1;
        return;
    } else { /* STRD */
        wr32(m, ad, CPU.r[rd & 14]);
        wr32(m, ad + 4, CPU.r[(rd & 14) + 1]);
    }
    if (wb)
        CPU.r[rn] = moved;
}

/* LDM / STM with the S bit and no PC: the user bank's registers. */
static void user_bank(nds_machine *m, bool enter, uint32_t mode)
{
    if (enter) {
        switch_mode(m, 0x1F);
        CPU.cpsr_ctl = (CPU.cpsr_ctl & ~0x1Fu) | 0x1F;
    } else {
        switch_mode(m, mode);
        CPU.cpsr_ctl = (CPU.cpsr_ctl & ~0x1Fu) | mode;
    }
}

static void arm_block(nds_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t p = (op >> 24) & 1, u = (op >> 23) & 1, s = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, lst = op & 0xFFFF;
    int n = __builtin_popcount(lst);
    if (!lst) {
        stop(m, "LDM / STM with an empty list", pc, op);
        return;
    }
    uint32_t base = CPU.r[rn];
    uint32_t ad = u ? base + (p ? 4 : 0) : base - 4u * n + (p ? 0 : 4);
    uint32_t nb = u ? base + 4u * n : base - 4u * n;
    bool user = s && !(l && (lst & 0x8000)); /* user-bank transfer */
    uint32_t mode = CPU.cpsr_ctl & 0x1F;
    if (user)
        user_bank(m, true, mode);
    if (l) {
        if (w && !(lst >> rn & 1))
            CPU.r[rn] = nb;
        uint32_t pcv = 0;
        for (int i = 0; i < 16; i++) {
            if (!(lst >> i & 1))
                continue;
            uint32_t v = rd32(m, ad);
            ad += 4;
            if (i == 15)
                pcv = v;
            else
                CPU.r[i] = v;
        }
        if (user)
            user_bank(m, false, mode);
        if (lst & 0x8000) {
            if (s) { /* LDM {..., pc}^: CPSR = SPSR */
                uint32_t spsr = CPU.spsr[bank_of(CPU.cpsr_ctl)];
                nds_cpu_set_cpsr(m, spsr);
                set_pc(m, pcv, (spsr & T_BIT) != 0);
            } else {
                load_pc(m, pcv);
            }
        }
        return;
    }
    int first = __builtin_ctz(lst);
    for (int i = 0; i < 16; i++) {
        if (!(lst >> i & 1))
            continue;
        uint32_t v = i == 15 ? pc + 12 : (i == (int)rn && w && i != first) ? nb : CPU.r[i];
        wr32(m, ad, v);
        ad += 4;
    }
    if (user)
        user_bank(m, false, mode);
    if (w)
        CPU.r[rn] = nb;
}

static void arm_execute(nds_machine *m, uint32_t op, uint32_t pc)
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
        stop(m, "undefined ARM encoding", pc, op);
        return;
    }
    if (!cond_ok(m, cond))
        return;
    switch ((op >> 25) & 7) {
    case 0:
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
                uint32_t v = nds_read8(m, a);
                nds_write8(m, a, (uint8_t)CPU.r[rm]);
                CPU.r[rd] = v;
            } else {
                uint32_t v = ror32(rd32(m, a), (a & 3) * 8);
                wr32(m, a, CPU.r[rm]);
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
            stop(m, "undefined ARM encoding", pc, op);
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
    case 6:
        stop(m, "coprocessor load / store", pc, op);
        return;
    default:
        if (op & (1u << 24)) { /* SWI */
            exception(m, 0x13, 0x08, pc + 4);
            return;
        }
        if ((op & 0x10) && ((op >> 8) & 15) == 15) { /* MRC / MCR p15 */
            uint32_t cn = (op >> 16) & 15, cm = op & 15, op2 = (op >> 5) & 7, rd = (op >> 12) & 15;
            if (op & (1u << 20)) {
                uint32_t v = cp15_read(m, cn, cm, op2);
                if (rd == 15) {
                    CPU.n = v >> 31;
                    CPU.z = (v >> 30) & 1;
                    CPU.c = (v >> 29) & 1;
                    CPU.v = (v >> 28) & 1;
                } else {
                    CPU.r[rd] = v;
                }
            } else {
                cp15_write(m, cn, cm, op2, rd == 15 ? pc + 12 : CPU.r[rd]);
            }
            return;
        }
        stop(m, "coprocessor operation", pc, op);
        return;
    }
}

/* ---- Thumb ---- */

static void thumb_execute(nds_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t k = op >> 11;
    if (k <= 2) { /* LSL / LSR / ASR #imm */
        uint32_t rd = op & 7, rs = (op >> 3) & 7, amt = (op >> 6) & 31, c;
        uint32_t r = shift_imm(m, k, CPU.r[rs], amt, &c);
        CPU.n = r >> 31;
        CPU.z = r == 0;
        CPU.c = c;
        CPU.r[rd] = r;
        return;
    }
    if (k == 3) { /* ADD / SUB */
        uint32_t rd = op & 7, rs = (op >> 3) & 7, x = (op >> 6) & 7;
        uint32_t b = (op & 0x400) ? x : CPU.r[x];
        CPU.r[rd] = (op & 0x200) ? add_flags(m, CPU.r[rs], ~b, 1, true) : add_flags(m, CPU.r[rs], b, 0, true);
        return;
    }
    if (k <= 7) { /* MOV / CMP / ADD / SUB #imm8 */
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
        if (!(op & 0x400)) { /* ALU */
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
        /* hi register operations, BX / BLX */
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
    if (k == 9) { /* LDR rd, [pc, #imm] */
        CPU.r[(op >> 8) & 7] = rd32(m, ((pc + 4) & ~2u) + (op & 0xFF) * 4);
        return;
    }
    if (k == 0xA || k == 0xB) { /* register offset */
        uint32_t rd = op & 7, ad = CPU.r[(op >> 3) & 7] + CPU.r[(op >> 6) & 7];
        switch ((op >> 9) & 7) {
        case 0: wr32(m, ad, CPU.r[rd]); break;
        case 1: nds_write16(m, ad, (uint16_t)CPU.r[rd]); break;
        case 2: nds_write8(m, ad, (uint8_t)CPU.r[rd]); break;
        case 3: CPU.r[rd] = (uint32_t)(int32_t)(int8_t)nds_read8(m, ad); break;
        case 4: CPU.r[rd] = ror32(rd32(m, ad), (ad & 3) * 8); break;
        case 5: CPU.r[rd] = ror32(nds_read16(m, ad), (ad & 1) * 8); break;
        case 6: CPU.r[rd] = nds_read8(m, ad); break;
        default:
            CPU.r[rd] = (ad & 1) ? (uint32_t)(int32_t)(int8_t)nds_read8(m, ad)
                                 : (uint32_t)(int32_t)(int16_t)nds_read16(m, ad);
            break;
        }
        return;
    }
    if (k <= 0xF) { /* immediate offset, word / byte */
        uint32_t rd = op & 7, off = (op >> 6) & 31, rb = (op >> 3) & 7;
        bool byte = op & 0x1000;
        uint32_t ad = CPU.r[rb] + (byte ? off : off * 4);
        if (op & 0x800)
            CPU.r[rd] = byte ? nds_read8(m, ad) : ror32(rd32(m, ad), (ad & 3) * 8);
        else if (byte)
            nds_write8(m, ad, (uint8_t)CPU.r[rd]);
        else
            wr32(m, ad, CPU.r[rd]);
        return;
    }
    if (k <= 0x11) { /* halfword */
        uint32_t rd = op & 7, ad = CPU.r[(op >> 3) & 7] + ((op >> 6) & 31) * 2;
        if (op & 0x800)
            CPU.r[rd] = ror32(nds_read16(m, ad), (ad & 1) * 8);
        else
            nds_write16(m, ad, (uint16_t)CPU.r[rd]);
        return;
    }
    if (k <= 0x13) { /* SP relative */
        uint32_t rd = (op >> 8) & 7, ad = CPU.r[13] + (op & 0xFF) * 4;
        if (op & 0x800)
            CPU.r[rd] = ror32(rd32(m, ad), (ad & 3) * 8);
        else
            wr32(m, ad, CPU.r[rd]);
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
        if ((op & 0x600) == 0x400) { /* PUSH / POP */
            uint32_t lst = op & 0xFF, extra = (op >> 8) & 1;
            if (op & 0x800) {
                uint32_t ad = CPU.r[13];
                for (int i = 0; i < 8; i++)
                    if (lst >> i & 1) {
                        CPU.r[i] = rd32(m, ad);
                        ad += 4;
                    }
                uint32_t pcv = 0;
                if (extra) {
                    pcv = rd32(m, ad);
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
                        wr32(m, ad, CPU.r[i]);
                        ad += 4;
                    }
                if (extra)
                    wr32(m, ad, CPU.r[14]);
            }
            return;
        }
        stop(m, (op >> 8) == 0xBE ? "BKPT" : "undefined Thumb encoding", pc, op);
        return;
    }
    if (k == 0x18 || k == 0x19) { /* LDMIA / STMIA */
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
                    CPU.r[i] = rd32(m, ad);
                    ad += 4;
                }
        } else {
            int first = __builtin_ctz(lst);
            for (int i = 0; i < 8; i++)
                if (lst >> i & 1) {
                    wr32(m, ad, (i == (int)rb && i != first) ? nb : CPU.r[i]);
                    ad += 4;
                }
            CPU.r[rb] = nb;
        }
        return;
    }
    if (k == 0x1A || k == 0x1B) {
        uint32_t c = (op >> 8) & 15;
        if (c == 15) {
            exception(m, 0x13, 0x08, pc + 2);
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
    if (k == 0x1E) { /* BL / BLX prefix */
        CPU.r[14] = pc + 4 + (uint32_t)(((int32_t)(op << 21)) >> 9);
        return;
    }
    if (k == 0x1F) { /* BL suffix */
        uint32_t t = CPU.r[14] + (op & 0x7FF) * 2;
        CPU.r[14] = (pc + 2) | 1;
        set_pc(m, t, true);
        return;
    }
    if (k == 0x1D) { /* BLX suffix: to ARM */
        uint32_t t = (CPU.r[14] + (op & 0x7FF) * 2) & ~3u;
        CPU.r[14] = (pc + 2) | 1;
        set_pc(m, t, false);
        return;
    }
    stop(m, "undefined Thumb encoding", pc, op);
}

void nds_cpu_step(nds_machine *m)
{
    bool thumb = CPU.cpsr_ctl & T_BIT;
    uint32_t pc = CPU.r[15] - (thumb ? 4 : 8);
    m->h.now += 1;
    m->trace[m->trace_at++ & 63] = pc | thumb;
    if (pc >= 0xFFFF0000u) {
        if (pc == NDS_BIOS_SWI)
            bios_swi(m);
        else if (pc == NDS_BIOS_IRQ)
            bios_irq(m);
        else if (pc == NDS_BIOS_IRQ_RET)
            bios_irq_return(m);
        else
            stop(m, "jump into the BIOS (no image: HLE only)", pc, 0);
        return;
    }
    if (thumb) {
        uint32_t op = nds_read16(m, pc);
        CPU.r[15] = pc + 4;
        m->branched = false;
        thumb_execute(m, op, pc);
        if (!m->branched)
            CPU.r[15] = pc + 6;
    } else {
        uint32_t op = rd32(m, pc);
        CPU.r[15] = pc + 8;
        m->branched = false;
        arm_execute(m, op, pc);
        if (!m->branched)
            CPU.r[15] = pc + 12;
    }
}
