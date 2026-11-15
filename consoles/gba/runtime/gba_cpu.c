/* ARM7TDMI interpreter (XBA-152): the full ARMv4T instruction set, ARM and Thumb states, all seven modes with
 * their banked registers, CPSR / SPSR, IRQ / SWI / undefined exceptions. Our own code, written from the ARM
 * Architecture Reference Manual (ARMv4T) and GBATEK « ARM CPU Reference ».
 *
 * Pipeline: between instructions r[15] holds the address of the next instruction + 8 (ARM) or + 4 (Thumb), so
 * an instruction reads PC as its own address + 8 / + 4 without any fix-up; the few cases that see + 12 (ARM
 * register-specified shifts reading PC, STR / STM of PC) add 4. An instruction that writes PC sets `branched`.
 *
 * Cycles: each instruction costs its opcode fetch (sequential), its data accesses (first one non-sequential,
 * the rest of an LDM / STM sequential), its internal cycles (multiplies by operand size, register shifts,
 * loads) and, after a branch, the pipeline refill (one non-sequential + one sequential fetch). */
#include "gba_internal.h"

#define R (m->cpu.r)

static inline uint32_t ror32(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

static inline bool is_thumb(const gba_machine *m) { return (m->cpu.cpsr_ctl & CPSR_T) != 0; }

static inline int bank_of(uint32_t mode)
{
    switch (mode) {
    case MODE_FIQ: return BANK_FIQ;
    case MODE_IRQ: return BANK_IRQ;
    case MODE_SVC: return BANK_SVC;
    case MODE_ABT: return BANK_ABT;
    case MODE_UND: return BANK_UND;
    default: return BANK_USR; /* USR, SYS and the invalid encodings */
    }
}

/* ---- CPSR / modes --------------------------------------------------------------------------------------- */

uint32_t gba_get_cpsr(const gba_machine *m)
{
    const gba_cpu *c = &m->cpu;
    return c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28 | c->cpsr_ctl;
}

void gba_set_mode(gba_machine *m, uint32_t mode)
{
    gba_cpu *c = &m->cpu;
    int ob = bank_of(c->cpsr_ctl & 0x1F), nb = bank_of(mode);
    if (ob != nb) {
        c->bank_r13[ob] = c->r[13];
        c->bank_r14[ob] = c->r[14];
        c->r[13] = c->bank_r13[nb];
        c->r[14] = c->bank_r14[nb];
        if (ob == BANK_FIQ || nb == BANK_FIQ) {
            uint32_t *save = ob == BANK_FIQ ? c->fiq_r8_12 : c->usr_r8_12;
            const uint32_t *load = nb == BANK_FIQ ? c->fiq_r8_12 : c->usr_r8_12;
            for (int i = 0; i < 5; i++) {
                save[i] = c->r[8 + i];
                c->r[8 + i] = load[i];
            }
        }
    }
    c->cpsr_ctl = (c->cpsr_ctl & ~0x1Fu) | (mode & 0x1F);
}

void gba_set_cpsr(gba_machine *m, uint32_t v)
{
    gba_cpu *c = &m->cpu;
    c->n = v >> 31;
    c->z = (v >> 30) & 1;
    c->c = (v >> 29) & 1;
    c->v = (v >> 28) & 1;
    gba_set_mode(m, v & 0x1F);
    c->cpsr_ctl = v & 0xFF;
}

static inline bool has_spsr(const gba_machine *m)
{
    return bank_of(m->cpu.cpsr_ctl & 0x1F) != BANK_USR;
}

static inline uint32_t get_spsr(const gba_machine *m)
{
    return has_spsr(m) ? m->cpu.spsr[bank_of(m->cpu.cpsr_ctl & 0x1F)] : gba_get_cpsr(m);
}

/* ---- branches and exceptions ---------------------------------------------------------------------------- */

static inline void refill(gba_machine *m, uint32_t addr)
{
    int r = (addr >> 24) & 15;
    if (is_thumb(m))
        m->now += m->wait16[r][0] + m->wait16[r][1];
    else
        m->now += m->wait32[r][0] + m->wait32[r][1];
}

/* PC write in the current state (no interworking). */
static inline void write_pc(gba_machine *m, uint32_t addr)
{
    if (is_thumb(m))
        R[15] = (addr & ~1u) + 4;
    else
        R[15] = (addr & ~3u) + 8;
    m->cpu.branched = true;
}

void gba_cpu_jump(gba_machine *m, uint32_t addr, bool thumb)
{
    if (thumb)
        m->cpu.cpsr_ctl |= CPSR_T;
    else
        m->cpu.cpsr_ctl &= ~CPSR_T;
    write_pc(m, addr);
    m->cpu.branched = false;
}

/* BX semantics: bit 0 selects the state. */
static inline void branch_exchange(gba_machine *m, uint32_t addr)
{
    if (addr & 1)
        m->cpu.cpsr_ctl |= CPSR_T;
    else
        m->cpu.cpsr_ctl &= ~CPSR_T;
    write_pc(m, addr);
}

uint32_t gba_cpu_pc(const gba_machine *m)
{
    return m->cpu.r[15] - (is_thumb(m) ? 4 : 8);
}

static void exception(gba_machine *m, uint32_t mode, uint32_t vector, uint32_t lr)
{
    uint32_t old = gba_get_cpsr(m);
    gba_set_mode(m, mode);
    m->cpu.spsr[bank_of(mode)] = old;
    R[14] = lr;
    m->cpu.cpsr_ctl = (m->cpu.cpsr_ctl & ~CPSR_T) | CPSR_I;
    if (mode == MODE_FIQ)
        m->cpu.cpsr_ctl |= CPSR_F;
    write_pc(m, vector);
}

static void take_irq(gba_machine *m)
{
    /* LR = next instruction + 4 in both states, so the handler returns with SUBS PC, LR, #4. */
    exception(m, MODE_IRQ, 0x18, gba_cpu_pc(m) + 4);
    refill(m, 0x18);
    m->cpu.branched = false;
}

static void do_swi(gba_machine *m, uint32_t pc, uint32_t num)
{
    uint32_t next = pc + (is_thumb(m) ? 2 : 4);
    if (m->builtin_bios && num != 0x04 && num != 0x05) {
        /* Built-in BIOS: everything but the interrupt waits runs in C, without entering the handler. */
        m->now += 3 * 4; /* entry + return, roughly */
        gba_bios_hle_swi(m, num);
        m->bios_latch = 0xE3A02004; /* what a SWI through the BIOS leaves in the latch ([0x188+8]) */
        return;
    }
    exception(m, MODE_SVC, 0x08, next);
}

static void undefined(gba_machine *m, uint32_t pc)
{
    exception(m, MODE_UND, 0x04, pc + (is_thumb(m) ? 2 : 4));
}

/* ---- ALU helpers ---------------------------------------------------------------------------------------- */

static inline void set_nz(gba_machine *m, uint32_t res)
{
    m->cpu.n = res >> 31;
    m->cpu.z = res == 0;
}

/* a + b + cin; with s, sets NZCV. Subtractions pass ~b (SUB: cin = 1, SBC: cin = C). */
static inline uint32_t adc(gba_machine *m, uint32_t a, uint32_t b, uint32_t cin, bool s)
{
    uint64_t r = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)r;
    if (s) {
        set_nz(m, res);
        m->cpu.c = (uint32_t)(r >> 32);
        m->cpu.v = (~(a ^ b) & (a ^ res)) >> 31;
    }
    return res;
}

/* Shift by an immediate amount (encodings: LSR/ASR #0 = #32, ROR #0 = RRX). */
static inline uint32_t shift_imm(const gba_machine *m, uint32_t type, uint32_t v, uint32_t amt, uint32_t *carry)
{
    switch (type) {
    case 0:
        if (amt == 0) {
            *carry = m->cpu.c;
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
            return m->cpu.c << 31 | v >> 1;
        }
        *carry = (v >> (amt - 1)) & 1;
        return ror32(v, amt);
    }
}

/* Shift by a register's bottom byte. */
static inline uint32_t shift_reg(const gba_machine *m, uint32_t type, uint32_t v, uint32_t amt, uint32_t *carry)
{
    if (amt == 0) {
        *carry = m->cpu.c;
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
        return ror32(v, amt);
    }
}

/* Internal cycles of a multiply by rs (m = 1..4 by the number of significant bytes). */
static inline int mul_cycles(uint32_t rs, bool sign)
{
    if (sign && (int32_t)rs < 0)
        rs = ~rs;
    if ((rs >> 8) == 0)
        return 1;
    if ((rs >> 16) == 0)
        return 2;
    if ((rs >> 24) == 0)
        return 3;
    return 4;
}

static inline bool cond_ok(const gba_machine *m, uint32_t cond)
{
    const gba_cpu *c = &m->cpu;
    switch (cond) {
    case 0x0: return c->z;
    case 0x1: return !c->z;
    case 0x2: return c->c;
    case 0x3: return !c->c;
    case 0x4: return c->n;
    case 0x5: return !c->n;
    case 0x6: return c->v;
    case 0x7: return !c->v;
    case 0x8: return c->c && !c->z;
    case 0x9: return !c->c || c->z;
    case 0xA: return c->n == c->v;
    case 0xB: return c->n != c->v;
    case 0xC: return !c->z && c->n == c->v;
    case 0xD: return c->z || c->n != c->v;
    case 0xE: return true;
    default: return false; /* NV: never on ARMv4 */
    }
}

/* Misaligned word load: the word at the aligned address, rotated right by 8 x the low two bits. */
static inline uint32_t load32_rot(gba_machine *m, uint32_t addr, bool seq)
{
    return ror32(gba_read32(m, addr, seq), (addr & 3) * 8);
}

/* ---- ARM ------------------------------------------------------------------------------------------------ */

static void arm_data_processing(gba_machine *m, uint32_t op)
{
    uint32_t opc = (op >> 21) & 15;
    bool s = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t op2, sc, a = R[rn];

    if (op & (1u << 25)) {
        uint32_t rot = (op >> 7) & 0x1E;
        op2 = ror32(op & 0xFF, rot);
        sc = rot ? op2 >> 31 : m->cpu.c;
    } else if (op & 0x10) {
        /* Register-specified shift: one more internal cycle, and PC reads as + 12. */
        uint32_t rm = op & 15;
        uint32_t vm = R[rm] + (rm == 15 ? 4 : 0);
        if (rn == 15)
            a += 4;
        op2 = shift_reg(m, (op >> 5) & 3, vm, R[(op >> 8) & 15] & 0xFF, &sc);
        m->now += 1;
    } else {
        op2 = shift_imm(m, (op >> 5) & 3, R[op & 15], (op >> 7) & 31, &sc);
    }

    uint32_t res;
    bool logical = false, write = true;
    switch (opc) {
    case 0x0: res = a & op2; logical = true; break;                 /* AND */
    case 0x1: res = a ^ op2; logical = true; break;                 /* EOR */
    case 0x2: res = adc(m, a, ~op2, 1, s && rd != 15); break;       /* SUB */
    case 0x3: res = adc(m, op2, ~a, 1, s && rd != 15); break;       /* RSB */
    case 0x4: res = adc(m, a, op2, 0, s && rd != 15); break;        /* ADD */
    case 0x5: res = adc(m, a, op2, m->cpu.c, s && rd != 15); break; /* ADC */
    case 0x6: res = adc(m, a, ~op2, m->cpu.c, s && rd != 15); break; /* SBC */
    case 0x7: res = adc(m, op2, ~a, m->cpu.c, s && rd != 15); break; /* RSC */
    case 0x8: res = a & op2; logical = true; write = false; break;  /* TST */
    case 0x9: res = a ^ op2; logical = true; write = false; break;  /* TEQ */
    case 0xA: res = adc(m, a, ~op2, 1, true); write = false; break; /* CMP */
    case 0xB: res = adc(m, a, op2, 0, true); write = false; break;  /* CMN */
    case 0xC: res = a | op2; logical = true; break;                 /* ORR */
    case 0xD: res = op2; logical = true; break;                     /* MOV */
    case 0xE: res = a & ~op2; logical = true; break;                /* BIC */
    default: res = ~op2; logical = true; break;                     /* MVN */
    }

    if (logical && s && (rd != 15 || !write)) {
        set_nz(m, res);
        m->cpu.c = sc;
    }
    if (!write) {
        if (rd == 15) /* TSTP / TEQP / CMPP / CMNP (ARMv2 leftovers): CPSR = SPSR */
            gba_set_cpsr(m, get_spsr(m));
        return;
    }
    if (rd == 15) {
        if (s) /* MOVS PC, ... : return from exception, CPSR = SPSR (may switch to Thumb) */
            gba_set_cpsr(m, get_spsr(m));
        write_pc(m, res);
    } else {
        R[rd] = res;
    }
}

static void arm_psr(gba_machine *m, uint32_t op)
{
    bool spsr = (op >> 22) & 1;
    if (!(op & (1u << 21))) { /* MRS */
        R[(op >> 12) & 15] = spsr ? get_spsr(m) : gba_get_cpsr(m);
        return;
    }
    uint32_t val;
    if (op & (1u << 25))
        val = ror32(op & 0xFF, (op >> 7) & 0x1E);
    else
        val = R[op & 15];
    uint32_t mask = 0;
    if (op & (1u << 19)) mask |= 0xFF000000u;
    if (op & (1u << 18)) mask |= 0x00FF0000u;
    if (op & (1u << 17)) mask |= 0x0000FF00u;
    if (op & (1u << 16)) mask |= 0x000000FFu;
    if (spsr) {
        if (has_spsr(m)) {
            uint32_t *p = &m->cpu.spsr[bank_of(m->cpu.cpsr_ctl & 0x1F)];
            *p = (*p & ~mask) | (val & mask);
        }
        return;
    }
    if ((m->cpu.cpsr_ctl & 0x1F) == MODE_USR)
        mask &= 0xFF000000u;
    uint32_t old = gba_get_cpsr(m);
    uint32_t nv = (old & ~mask) | (val & mask);
    nv = (nv & ~CPSR_T) | (old & CPSR_T); /* MSR does not switch state */
    gba_set_cpsr(m, nv);
}

static void arm_multiply(gba_machine *m, uint32_t op)
{
    uint32_t rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    bool s = (op >> 20) & 1, acc = (op >> 21) & 1;
    uint32_t vs = R[rs];
    uint32_t res = R[rm] * vs;
    m->now += mul_cycles(vs, true);
    if (acc) {
        res += R[rn];
        m->now += 1;
    }
    R[rd] = res;
    if (s)
        set_nz(m, res); /* C is meaningless after a multiply on ARMv4: left as it was */
}

static void arm_multiply_long(gba_machine *m, uint32_t op)
{
    uint32_t hi = (op >> 16) & 15, lo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    bool s = (op >> 20) & 1, acc = (op >> 21) & 1, sign = (op >> 22) & 1;
    uint32_t vs = R[rs];
    uint64_t res;
    if (sign)
        res = (uint64_t)((int64_t)(int32_t)R[rm] * (int32_t)vs);
    else
        res = (uint64_t)R[rm] * vs;
    m->now += mul_cycles(vs, sign) + 1;
    if (acc) {
        res += (uint64_t)R[hi] << 32 | R[lo];
        m->now += 1;
    }
    R[lo] = (uint32_t)res;
    R[hi] = (uint32_t)(res >> 32);
    if (s) {
        m->cpu.n = (uint32_t)(res >> 63);
        m->cpu.z = res == 0;
    }
}

static void arm_swap(gba_machine *m, uint32_t op)
{
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
    uint32_t addr = R[rn], src = R[rm], tmp;
    if (op & (1u << 22)) {
        tmp = gba_read8(m, addr, false);
        gba_write8(m, addr, (uint8_t)src, false);
    } else {
        tmp = load32_rot(m, addr, false);
        gba_write32(m, addr, src, false);
    }
    m->now += 1;
    if (rd == 15)
        write_pc(m, tmp);
    else
        R[rd] = tmp;
}

static void arm_halfword(gba_machine *m, uint32_t op)
{
    bool p = (op >> 24) & 1, u = (op >> 23) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
    uint32_t off = (op & (1u << 22)) ? ((op >> 4) & 0xF0) | (op & 0xF) : R[op & 15];
    uint32_t base = R[rn];
    uint32_t moved = u ? base + off : base - off;
    uint32_t addr = p ? moved : base;

    if (l) {
        uint32_t v;
        switch (sh) {
        case 1: /* LDRH: a misaligned halfword comes back rotated */
            v = ror32(gba_read16(m, addr, false), (addr & 1) * 8);
            break;
        case 2: /* LDRSB */
            v = (uint32_t)(int32_t)(int8_t)gba_read8(m, addr, false);
            break;
        default: /* LDRSH: misaligned behaves as LDRSB */
            if (addr & 1)
                v = (uint32_t)(int32_t)(int8_t)gba_read8(m, addr, false);
            else
                v = (uint32_t)(int32_t)(int16_t)gba_read16(m, addr, false);
            break;
        }
        m->now += 1;
        if (!p || w)
            R[rn] = moved;
        if (rd == 15)
            write_pc(m, v);
        else
            R[rd] = v;
    } else {
        if (sh == 1)
            gba_write16(m, addr, (uint16_t)(R[rd] + (rd == 15 ? 4 : 0)), false);
        /* sh 2 / 3 stores are LDRD / STRD on ARMv5: no-ops on the ARM7TDMI */
        if (!p || w)
            R[rn] = moved;
    }
}

static void arm_single_transfer(gba_machine *m, uint32_t op)
{
    bool p = (op >> 24) & 1, u = (op >> 23) & 1, b = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    uint32_t off;
    if (op & (1u << 25)) {
        uint32_t dummy;
        off = shift_imm(m, (op >> 5) & 3, R[op & 15], (op >> 7) & 31, &dummy);
    } else {
        off = op & 0xFFF;
    }
    uint32_t base = R[rn];
    uint32_t moved = u ? base + off : base - off;
    uint32_t addr = p ? moved : base;

    if (l) {
        uint32_t v = b ? gba_read8(m, addr, false) : load32_rot(m, addr, false);
        m->now += 1;
        if (!p || w)
            R[rn] = moved;
        if (rd == 15)
            write_pc(m, v);
        else
            R[rd] = v;
    } else {
        uint32_t v = R[rd] + (rd == 15 ? 4 : 0);
        if (b)
            gba_write8(m, addr, (uint8_t)v, false);
        else
            gba_write32(m, addr, v, false);
        if (!p || w)
            R[rn] = moved;
    }
}

static void arm_block_transfer(gba_machine *m, uint32_t op)
{
    bool p = (op >> 24) & 1, u = (op >> 23) & 1, s = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
    uint32_t rn = (op >> 16) & 15;
    uint32_t list = op & 0xFFFF;
    uint32_t size;
    if (list == 0) { /* ARMv4 quirk: an empty list moves PC and steps the base by 0x40 */
        list = 1u << 15;
        size = 0x40;
    } else {
        size = 4 * (uint32_t)__builtin_popcount(list);
    }
    uint32_t base = R[rn];
    uint32_t addr = u ? base + (p ? 4 : 0) : base - size + (p ? 0 : 4);
    uint32_t new_base = u ? base + size : base - size;

    /* S bit without PC in an LDM (or in any STM): the user-mode registers. */
    bool user_bank = s && (!l || !(list & 0x8000));
    uint32_t mode = m->cpu.cpsr_ctl & 0x1F;
    if (user_bank)
        gba_set_mode(m, MODE_SYS);

    bool seq = false;
    if (l) {
        if (w && !(list & (1u << rn)))
            R[rn] = new_base;
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i)))
                continue;
            uint32_t v = gba_read32(m, addr & ~3u, seq);
            seq = true;
            addr += 4;
            if (i == 15) {
                if (s) /* LDM ..., {.., PC}^ : CPSR = SPSR */
                    gba_set_cpsr(m, get_spsr(m));
                write_pc(m, v);
            } else {
                R[i] = v;
            }
        }
        m->now += 1;
    } else {
        uint32_t first = (uint32_t)__builtin_ctz(list);
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i)))
                continue;
            uint32_t v = R[i];
            if (i == 15)
                v += 4;
            else if ((uint32_t)i == rn && w && (uint32_t)i != first)
                v = new_base; /* the base was already written back after the first transfer */
            gba_write32(m, addr & ~3u, v, seq);
            seq = true;
            addr += 4;
        }
        if (w)
            R[rn] = new_base;
    }
    if (user_bank)
        gba_set_mode(m, mode);
}

static inline __attribute__((always_inline)) void arm_execute(gba_machine *m, uint32_t op, uint32_t pc)
{
    if (op >> 28 != 0xE && !cond_ok(m, op >> 28))
        return;

    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x0FC000F0) == 0x00000090) {
            arm_multiply(m, op);
        } else if ((op & 0x0F8000F0) == 0x00800090) {
            arm_multiply_long(m, op);
        } else if ((op & 0x0FB00FF0) == 0x01000090) {
            arm_swap(m, op);
        } else if ((op & 0x0FFFFFF0) == 0x012FFF10) {
            branch_exchange(m, R[op & 15]);
        } else if ((op & 0x90) == 0x90 && (op & 0x60)) {
            arm_halfword(m, op);
        } else if ((op & 0x01900000) == 0x01000000) {
            /* TST/TEQ/CMP/CMN without S: the PSR transfers */
            if ((op & 0x0FBF0FFF) == 0x010F0000 || (op & 0x0DB0F000) == 0x0120F000)
                arm_psr(m, op);
            else
                undefined(m, pc);
        } else {
            arm_data_processing(m, op);
        }
        break;
    case 1:
        if ((op & 0x01900000) == 0x01000000) {
            if ((op & 0x0FB0F000) == 0x0320F000)
                arm_psr(m, op);
            else
                undefined(m, pc);
        } else {
            arm_data_processing(m, op);
        }
        break;
    case 2:
        arm_single_transfer(m, op);
        break;
    case 3:
        if (op & 0x10) {
            if (op == GBA_HLE_TRAP && m->builtin_bios && pc < GBA_BIOS_SIZE)
                gba_bios_hle_swi(m, R[12]);
            else
                undefined(m, pc);
        } else {
            arm_single_transfer(m, op);
        }
        break;
    case 4:
        arm_block_transfer(m, op);
        break;
    case 5: {
        uint32_t target = R[15] + (uint32_t)(((int32_t)(op << 8)) >> 6);
        if (op & (1u << 24))
            R[14] = pc + 4;
        write_pc(m, target);
        break;
    }
    case 6:
        undefined(m, pc); /* coprocessor transfers: no coprocessor on the GBA */
        break;
    default:
        if (op & (1u << 24))
            do_swi(m, pc, (op >> 16) & 0xFF);
        else
            undefined(m, pc);
        break;
    }
}

/* ---- Thumb ---------------------------------------------------------------------------------------------- */

static void thumb_alu(gba_machine *m, uint32_t op)
{
    uint32_t rd = op & 7, rs = (op >> 3) & 7;
    uint32_t a = R[rd], b = R[rs], res, carry;
    switch ((op >> 6) & 15) {
    case 0x0: res = a & b; set_nz(m, res); R[rd] = res; break;              /* AND */
    case 0x1: res = a ^ b; set_nz(m, res); R[rd] = res; break;              /* EOR */
    case 0x2: case 0x3: case 0x4: case 0x7:                                 /* LSL LSR ASR ROR */
        {
            static const uint8_t type[16] = {[2] = 0, [3] = 1, [4] = 2, [7] = 3};
            res = shift_reg(m, type[(op >> 6) & 15], a, b & 0xFF, &carry);
            set_nz(m, res);
            m->cpu.c = carry;
            R[rd] = res;
            m->now += 1;
        }
        break;
    case 0x5: R[rd] = adc(m, a, b, m->cpu.c, true); break;                  /* ADC */
    case 0x6: R[rd] = adc(m, a, ~b, m->cpu.c, true); break;                 /* SBC */
    case 0x8: set_nz(m, a & b); break;                                      /* TST */
    case 0x9: R[rd] = adc(m, 0, ~b, 1, true); break;                        /* NEG */
    case 0xA: adc(m, a, ~b, 1, true); break;                                /* CMP */
    case 0xB: adc(m, a, b, 0, true); break;                                 /* CMN */
    case 0xC: res = a | b; set_nz(m, res); R[rd] = res; break;              /* ORR */
    case 0xD:                                                               /* MUL */
        m->now += mul_cycles(a, true);
        res = a * b;
        set_nz(m, res);
        R[rd] = res;
        break;
    case 0xE: res = a & ~b; set_nz(m, res); R[rd] = res; break;             /* BIC */
    default: res = ~b; set_nz(m, res); R[rd] = res; break;                  /* MVN */
    }
}

static void thumb_hireg(gba_machine *m, uint32_t op)
{
    uint32_t rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15;
    switch ((op >> 8) & 3) {
    case 0: {
        uint32_t res = R[rd] + R[rs];
        if (rd == 15)
            write_pc(m, res);
        else
            R[rd] = res;
        break;
    }
    case 1:
        adc(m, R[rd], ~R[rs], 1, true);
        break;
    case 2:
        if (rd == 15)
            write_pc(m, R[rs]);
        else
            R[rd] = R[rs];
        break;
    default:
        branch_exchange(m, R[rs]);
        break;
    }
}

static void thumb_push_pop(gba_machine *m, uint32_t op)
{
    uint32_t list = op & 0xFF;
    bool extra = (op >> 8) & 1; /* LR on push, PC on pop */
    if (op & 0x800) { /* POP */
        uint32_t addr = R[13];
        bool seq = false;
        if (!list && !extra) { /* empty list: PC, SP + 0x40 */
            write_pc(m, gba_read32(m, addr & ~3u, false));
            R[13] = addr + 0x40;
            m->now += 1;
            return;
        }
        for (int i = 0; i < 8; i++) {
            if (list & (1u << i)) {
                R[i] = gba_read32(m, addr & ~3u, seq);
                seq = true;
                addr += 4;
            }
        }
        if (extra) {
            write_pc(m, gba_read32(m, addr & ~3u, seq));
            addr += 4;
        }
        R[13] = addr;
        m->now += 1;
    } else { /* PUSH */
        if (!list && !extra) {
            R[13] -= 0x40;
            gba_write32(m, R[13] & ~3u, R[15] + 2, false);
            return;
        }
        uint32_t n = (uint32_t)__builtin_popcount(list) + extra;
        uint32_t addr = R[13] - 4 * n;
        R[13] = addr;
        bool seq = false;
        for (int i = 0; i < 8; i++) {
            if (list & (1u << i)) {
                gba_write32(m, addr & ~3u, R[i], seq);
                seq = true;
                addr += 4;
            }
        }
        if (extra)
            gba_write32(m, addr & ~3u, R[14], seq);
    }
}

static void thumb_ldm_stm(gba_machine *m, uint32_t op)
{
    uint32_t rb = (op >> 8) & 7, list = op & 0xFF;
    uint32_t addr = R[rb];
    bool seq = false;
    if (!list) { /* empty list: PC, base + 0x40 */
        if (op & 0x800) {
            write_pc(m, gba_read32(m, addr & ~3u, false));
            m->now += 1;
        } else {
            gba_write32(m, addr & ~3u, R[15] + 2, false);
        }
        R[rb] = addr + 0x40;
        return;
    }
    uint32_t new_base = addr + 4 * (uint32_t)__builtin_popcount(list);
    if (op & 0x800) { /* LDMIA */
        if (!(list & (1u << rb)))
            R[rb] = new_base;
        for (int i = 0; i < 8; i++) {
            if (list & (1u << i)) {
                R[i] = gba_read32(m, addr & ~3u, seq);
                seq = true;
                addr += 4;
            }
        }
        m->now += 1;
    } else { /* STMIA */
        uint32_t first = (uint32_t)__builtin_ctz(list);
        for (int i = 0; i < 8; i++) {
            if (list & (1u << i)) {
                uint32_t v = ((uint32_t)i == rb && (uint32_t)i != first) ? new_base : R[i];
                gba_write32(m, addr & ~3u, v, seq);
                seq = true;
                addr += 4;
            }
        }
        R[rb] = new_base;
    }
}

static inline __attribute__((always_inline)) void thumb_execute(gba_machine *m, uint32_t op, uint32_t pc)
{
    uint32_t carry;
    switch (op >> 11) {
    case 0x00: case 0x01: case 0x02: { /* LSL / LSR / ASR #imm */
        uint32_t rd = op & 7, res = shift_imm(m, op >> 11, R[(op >> 3) & 7], (op >> 6) & 31, &carry);
        set_nz(m, res);
        m->cpu.c = carry;
        R[rd] = res;
        break;
    }
    case 0x03: { /* ADD / SUB, register or 3-bit immediate */
        uint32_t rd = op & 7, a = R[(op >> 3) & 7];
        uint32_t b = (op & 0x400) ? (op >> 6) & 7 : R[(op >> 6) & 7];
        R[rd] = (op & 0x200) ? adc(m, a, ~b, 1, true) : adc(m, a, b, 0, true);
        break;
    }
    case 0x04: { /* MOV #imm8 */
        uint32_t rd = (op >> 8) & 7;
        R[rd] = op & 0xFF;
        set_nz(m, R[rd]);
        break;
    }
    case 0x05: adc(m, R[(op >> 8) & 7], ~(op & 0xFF), 1, true); break; /* CMP */
    case 0x06: R[(op >> 8) & 7] = adc(m, R[(op >> 8) & 7], op & 0xFF, 0, true); break;     /* ADD */
    case 0x07: R[(op >> 8) & 7] = adc(m, R[(op >> 8) & 7], ~(op & 0xFF), 1, true); break;  /* SUB */
    case 0x08:
        if (op & 0x400)
            thumb_hireg(m, op);
        else
            thumb_alu(m, op);
        break;
    case 0x09: /* LDR Rd, [PC, #imm8 * 4] */
        R[(op >> 8) & 7] = gba_read32(m, (R[15] & ~2u) + (op & 0xFF) * 4, false);
        m->now += 1;
        break;
    case 0x0A: case 0x0B: {
        uint32_t rd = op & 7, addr = R[(op >> 3) & 7] + R[(op >> 6) & 7];
        switch ((op >> 9) & 7) {
        case 0: gba_write32(m, addr, R[rd], false); break;                          /* STR */
        case 1: gba_write16(m, addr, (uint16_t)R[rd], false); break;               /* STRH */
        case 2: gba_write8(m, addr, (uint8_t)R[rd], false); break;                       /* STRB */
        case 3: R[rd] = (uint32_t)(int32_t)(int8_t)gba_read8(m, addr, false); m->now += 1; break; /* LDRSB */
        case 4: R[rd] = load32_rot(m, addr, false); m->now += 1; break;                  /* LDR */
        case 5: R[rd] = ror32(gba_read16(m, addr, false), (addr & 1) * 8); m->now += 1; break; /* LDRH */
        case 6: R[rd] = gba_read8(m, addr, false); m->now += 1; break;                   /* LDRB */
        default:                                                                        /* LDRSH */
            if (addr & 1)
                R[rd] = (uint32_t)(int32_t)(int8_t)gba_read8(m, addr, false);
            else
                R[rd] = (uint32_t)(int32_t)(int16_t)gba_read16(m, addr, false);
            m->now += 1;
            break;
        }
        break;
    }
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: { /* LDR / STR / LDRB / STRB #imm5 */
        uint32_t rd = op & 7, off = (op >> 6) & 31, base = R[(op >> 3) & 7];
        bool byte = op & 0x1000;
        uint32_t addr = base + (byte ? off : off * 4);
        if (op & 0x800) {
            R[rd] = byte ? gba_read8(m, addr, false) : load32_rot(m, addr, false);
            m->now += 1;
        } else if (byte) {
            gba_write8(m, addr, (uint8_t)R[rd], false);
        } else {
            gba_write32(m, addr, R[rd], false);
        }
        break;
    }
    case 0x10: case 0x11: { /* LDRH / STRH #imm5 * 2 */
        uint32_t rd = op & 7, addr = R[(op >> 3) & 7] + ((op >> 6) & 31) * 2;
        if (op & 0x800) {
            R[rd] = ror32(gba_read16(m, addr, false), (addr & 1) * 8);
            m->now += 1;
        } else {
            gba_write16(m, addr, (uint16_t)R[rd], false);
        }
        break;
    }
    case 0x12: case 0x13: { /* LDR / STR [SP, #imm8 * 4] */
        uint32_t rd = (op >> 8) & 7, addr = R[13] + (op & 0xFF) * 4;
        if (op & 0x800) {
            R[rd] = load32_rot(m, addr, false);
            m->now += 1;
        } else {
            gba_write32(m, addr, R[rd], false);
        }
        break;
    }
    case 0x14: R[(op >> 8) & 7] = (R[15] & ~2u) + (op & 0xFF) * 4; break; /* ADD Rd, PC, #imm */
    case 0x15: R[(op >> 8) & 7] = R[13] + (op & 0xFF) * 4; break;         /* ADD Rd, SP, #imm */
    case 0x16: case 0x17:
        if ((op & 0xF00) == 0x000) { /* ADD SP, #+-imm7 * 4 */
            uint32_t off = (op & 0x7F) * 4;
            R[13] = (op & 0x80) ? R[13] - off : R[13] + off;
        } else if ((op & 0x600) == 0x400) {
            thumb_push_pop(m, op);
        } else {
            undefined(m, pc);
        }
        break;
    case 0x18: case 0x19:
        thumb_ldm_stm(m, op);
        break;
    case 0x1A: case 0x1B: { /* Bcond / SWI */
        uint32_t cond = (op >> 8) & 15;
        if (cond == 15)
            do_swi(m, pc, op & 0xFF);
        else if (cond == 14)
            undefined(m, pc);
        else if (cond_ok(m, cond))
            write_pc(m, R[15] + (uint32_t)((int32_t)(int8_t)(op & 0xFF) * 2));
        break;
    }
    case 0x1C: /* B */
        write_pc(m, R[15] + (uint32_t)(((int32_t)(op << 21)) >> 20));
        break;
    case 0x1E: /* BL, first half: LR = PC + (offset << 12) */
        R[14] = R[15] + (uint32_t)(((int32_t)(op << 21)) >> 9);
        break;
    case 0x1F: { /* BL, second half */
        uint32_t target = R[14] + (op & 0x7FF) * 2;
        R[14] = (pc + 2) | 1;
        write_pc(m, target);
        break;
    }
    default:
        undefined(m, pc); /* 0x1D: BLX on ARMv5 */
        break;
    }
}

/* ---- run loop ------------------------------------------------------------------------------------------- */

/* Sequential opcode fetches from the cartridge ROM and IWRAM, the regions code runs from, inline: what gba_fetch16 /
 * gba_fetch32 (gba_bus.c) do there with seq = true, whose cost is the fetch16 / fetch32 table that lifted code also
 * charges (gba_update_waitstates: prefetch included). Everything else goes through the bus functions. */
static inline __attribute__((always_inline)) uint32_t fetch16_seq(gba_machine *m, uint32_t pc)
{
    uint32_t r = (pc >> 24) & 15, a = pc & 0x1FFFFFEu, v;
    if (r - 8u <= 5u && a < m->rom_size) {
        m->now += m->fetch16[r];
        v = rd16(&m->rom[a]);
    } else if (r == 3) {
        m->now += 1;
        v = rd16(&m->iwram[pc & (GBA_IWRAM_SIZE - 1)]);
    } else {
        return gba_fetch16(m, pc, true);
    }
    m->open_bus = v * 0x00010001u;
    return v;
}

static inline __attribute__((always_inline)) uint32_t fetch32_seq(gba_machine *m, uint32_t pc)
{
    uint32_t r = (pc >> 24) & 15, a = pc & 0x1FFFFFCu, v;
    if (r - 8u <= 5u && a < m->rom_size) {
        m->now += m->fetch32[r];
        v = rd32(&m->rom[a]);
    } else if (r == 3) {
        m->now += 1;
        v = rd32(&m->iwram[pc & (GBA_IWRAM_SIZE - 1)]);
    } else {
        return gba_fetch32(m, pc, true);
    }
    m->open_bus = v;
    return v;
}

/* Inlined into the run loop with both decoders: a call per instruction (and the callee-saved registers each one
 * saves) was a large share of the interpreter's time. gba_cpu_step keeps an out-of-line copy. */
static inline __attribute__((always_inline)) void step(gba_machine *m)
{
    if (m->irq_line && !(m->cpu.cpsr_ctl & CPSR_I)) {
        take_irq(m);
        return;
    }
    m->cpu.branched = false;
    if (m->cpu.cpsr_ctl & CPSR_T) {
        uint32_t pc = R[15] - 4;
        uint32_t op = fetch16_seq(m, pc);
        thumb_execute(m, op, pc);
        if (!m->cpu.branched)
            R[15] += 2;
        else
            refill(m, R[15]);
    } else {
        uint32_t pc = R[15] - 8;
        uint32_t op = fetch32_seq(m, pc);
        arm_execute(m, op, pc);
        if (!m->cpu.branched)
            R[15] += 4;
        else
            refill(m, R[15]);
    }
}

void gba_cpu_step(gba_machine *m)
{
    step(m);
}

void gba_cpu_run(gba_machine *m)
{
    if (m->rc) {
        gba_recomp_run(m);
        return;
    }
    while (m->now < m->next_event) {
        if (m->cpu.halted || m->cpu.stopped) {
            m->now = m->next_event;
            return;
        }
        step(m);
    }
}

void gba_cpu_reset(gba_machine *m, uint32_t pc, uint32_t cpsr)
{
    gba_cpu *c = &m->cpu;
    for (int i = 0; i < 16; i++)
        c->r[i] = 0;
    for (int i = 0; i < BANK_COUNT; i++)
        c->bank_r13[i] = c->bank_r14[i] = c->spsr[i] = 0;
    for (int i = 0; i < 5; i++)
        c->fiq_r8_12[i] = c->usr_r8_12[i] = 0;
    c->cpsr_ctl = MODE_SVC; /* bank layout matches before the switch below */
    c->n = c->z = c->c = c->v = 0;
    gba_set_cpsr(m, cpsr);
    c->halted = c->stopped = false;
    gba_cpu_jump(m, pc, (cpsr & CPSR_T) != 0);
}
