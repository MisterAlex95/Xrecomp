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
 * loads) and, after a branch, the pipeline refill (one non-sequential + one sequential fetch). In the cartridge with
 * WAITCNT bit 14 set, opcode fetches go through the prefetch buffer (gba_recomp_rt.h rc_pf_fetch).
 *
 * step() runs one instruction; the run loops run predecoded blocks of it (« predecoded blocks » below, XBA-208),
 * cycle for cycle the same, with step()'s loop kept as the reference. */
#include "gba_internal.h"

#include <stdlib.h>
#include <string.h>

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

#define HEAD(m) ((gba_rc_head *)(void *)(m))

static inline void refill(gba_machine *m, uint32_t addr)
{
    m->now += rc_refill_cost(HEAD(m), m->now, addr, is_thumb(m));
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
    /* The entry costs like a branch (2S + 1N): the opcode fetch of the instruction it replaces, then the refill. */
    m->now += rc_fetch_cost(HEAD(m), m->now, gba_cpu_pc(m), is_thumb(m));
    /* LR = next instruction + 4 in both states, so the handler returns with SUBS PC, LR, #4. */
    exception(m, MODE_IRQ, 0x18, gba_cpu_pc(m) + 4);
    refill(m, 0x18);
    m->cpu.branched = false;
}

static void do_swi(gba_machine *m, uint32_t pc, uint32_t num)
{
    uint32_t next = pc + (is_thumb(m) ? 2 : 4);
    if (m->builtin_bios && num == 0x00) {
        /* Built-in BIOS: SoftReset in C at once (the other SWIs go through its dispatcher to their HLE stubs). */
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

/* C after UMULL / SMULL / UMLAL / SMLAL with S (unpredictable on ARMv4, but the ARM7TDMI's is deterministic). The
 * multiplier recodes rs as radix-4 Booth digits, 4 per internal cycle of mul_cycles; the multiplicand rm goes through
 * a carry-save adder array as rm, 2 * rm, ~rm or ~(2 * rm) (a negation is an inversion plus a carry in). The flag is
 * bit 31 of the last cycle's last addend: the digit of rs bits 8n, 8n - 1, 8n - 2 (bit 32 = the sign, or 0 for
 * unsigned). Checked against the hardware results of the mGBA suite (4 cycles); with early termination it is the
 * same rule, not measured. MUL / MLA keep C (the 32-bit case is not modelled). gba_recomp_rt.h: rc_mull_carry. */
static inline bool mull_carry(uint32_t rm, uint32_t rs, bool sign)
{
    int n = mul_cycles(rs, sign);
    uint64_t x = sign ? (uint64_t)(int64_t)(int32_t)rs : rs;
    uint32_t a;
    switch ((x >> (8 * n - 2)) & 7) {
    case 1: case 2: a = rm; break;
    case 3: a = rm << 1; break;
    case 4: a = ~(rm << 1); break;
    case 5: case 6: a = ~rm; break;
    default: a = 0; break;
    }
    return a >> 31;
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

static inline __attribute__((always_inline)) void arm_data_processing(gba_machine *m, uint32_t op)
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
    uint32_t vs = R[rs], vm = R[rm];
    uint64_t res;
    if (sign)
        res = (uint64_t)((int64_t)(int32_t)vm * (int32_t)vs);
    else
        res = (uint64_t)vm * vs;
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
        m->cpu.c = mull_carry(vm, vs, sign);
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
            seq = ((addr + 4) >> 24) == (addr >> 24); /* into another region: non-sequential */
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
            seq = ((addr + 4) >> 24) == (addr >> 24); /* into another region: non-sequential */
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
                gba_bios_hle_swi(m, pc - GBA_HLE_STUBS < 0x800u ? (pc - GBA_HLE_STUBS) >> 3 : R[12]);
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
                seq = ((addr + 4) >> 24) == (addr >> 24); /* into another region: non-sequential */
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
                seq = ((addr + 4) >> 24) == (addr >> 24); /* into another region: non-sequential */
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
                seq = ((addr + 4) >> 24) == (addr >> 24); /* into another region: non-sequential */
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
                seq = ((addr + 4) >> 24) == (addr >> 24); /* into another region: non-sequential */
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

/* Sequential opcode fetches from the cartridge ROM, IWRAM and the BIOS, the regions code runs from, inline: what gba_fetch16 /
 * gba_fetch32 (gba_bus.c) do there with seq = true, whose cost is the fetch16 / fetch32 table that lifted code also
 * charges (gba_update_waitstates: prefetch included). Everything else goes through the bus functions. */
static inline __attribute__((always_inline)) uint32_t fetch16_seq(gba_machine *m, uint32_t pc)
{
    uint32_t r = (pc >> 24) & 15, a = pc & 0x1FFFFFEu, v;
    if (r - 8u <= 5u && a < m->rom_size) {
        m->now += m->prefetch ? rc_pf_fetch(HEAD(m), m->now, pc + 4, true, m->wait16[r][0]) : m->fetch16[r];
        v = rd16(&m->rom[a]);
        m->open_bus = gba_rom_half(m, pc + 4) * 0x00010001u; /* the prefetched opcode (gba_prefetch_latch) */
    } else if (r == 3) {
        m->now += 1;
        v = rd16(&m->iwram[pc & (GBA_IWRAM_SIZE - 1)]);
        m->open_bus = gba_prefetch_latch(m, pc, true);
    } else if (pc < GBA_BIOS_SIZE) {
        m->now += m->fetch16[0];
        v = rd16(&m->bios[pc]);
        m->bios_latch = rd32(&m->bios[(pc + 4) & (GBA_BIOS_SIZE - 4)]); /* as gba_fetch16 */
        m->open_bus = m->bios_latch;
    } else {
        return gba_fetch16(m, pc, true);
    }
    return v;
}

static inline __attribute__((always_inline)) uint32_t fetch32_seq(gba_machine *m, uint32_t pc)
{
    uint32_t r = (pc >> 24) & 15, a = pc & 0x1FFFFFCu, v;
    if (r - 8u <= 5u && a < m->rom_size) {
        m->now += m->prefetch ? rc_pf_fetch(HEAD(m), m->now, pc + 8, false, m->wait32[r][0]) : m->fetch32[r];
        v = rd32(&m->rom[a]);
        m->open_bus = gba_rom_half(m, pc + 8) | (uint32_t)gba_rom_half(m, pc + 10) << 16;
    } else if (r == 3) {
        m->now += 1;
        v = rd32(&m->iwram[pc & (GBA_IWRAM_SIZE - 1)]);
        m->open_bus = rd32(&m->iwram[(pc + 8) & (GBA_IWRAM_SIZE - 4)]);
    } else if (pc < GBA_BIOS_SIZE) {
        m->now += m->fetch32[0];
        v = rd32(&m->bios[pc]);
        m->bios_latch = rd32(&m->bios[(pc + 8) & (GBA_BIOS_SIZE - 4)]); /* the prefetched opcode, as gba_fetch32 */
        m->open_bus = m->bios_latch;
    } else {
        return gba_fetch32(m, pc, true);
    }
    return v;
}

/* After an instruction that took cycles beyond its opcode fetch (data accesses, internal cycles) and did not branch,
 * the next opcode fetch is non-sequential: its cycles beyond the sequential fetch, charged at the end of the
 * instruction (lifted code: RC_NEXTRA). With the cartridge's prefetch buffer on, the next fetch goes through it
 * (rc_pf_fetch, which costs a non-sequential access when it misses). Fitted to the mGBA suite's timing tests (XBA-201). */
static inline uint32_t nfetch_extra(const gba_machine *m, uint32_t pc, bool thumb)
{
    uint32_t r = (pc >> 24) & 15;
    if (r - 8u <= 5u && m->prefetch)
        return 0;
    return thumb ? (uint32_t)m->wait16[r][0] - m->fetch16[r] : (uint32_t)m->wait32[r][0] - m->fetch32[r];
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
        uint64_t t0 = m->now;
        thumb_execute(m, op, pc);
        if (!m->cpu.branched) {
            R[15] += 2;
            if (m->now != t0)
                m->now += nfetch_extra(m, pc, true);
        } else {
            refill(m, R[15]);
        }
    } else {
        uint32_t pc = R[15] - 8;
        uint32_t op = fetch32_seq(m, pc);
        uint64_t t0 = m->now;
        arm_execute(m, op, pc);
        if (!m->cpu.branched) {
            R[15] += 4;
            if (m->now != t0)
                m->now += nfetch_extra(m, pc, false);
        } else {
            refill(m, R[15]);
        }
    }
}

void gba_cpu_step(gba_machine *m)
{
    step(m);
}

static inline bool irq_due(const gba_machine *m);

/* gba_cpu_step_handed one step() at a time: the reference loop, and what the blocks cannot run. */
static uint32_t handed_steps(gba_machine *m, uint32_t *left, bool *chained, uint32_t iw_lo, uint32_t iw_hi)
{
    uint32_t n = 0;
    for (;;) {
        uint32_t t = m->cpu.cpsr_ctl & CPSR_T, pc = R[15] - (t ? 4u : 8u);
        step(m);
        n++;
        uint32_t nt = m->cpu.cpsr_ctl & CPSR_T, npc = R[15] - (nt ? 4u : 8u);
        if (*left <= 1 || nt != t || npc != pc + (t ? 2u : 4u)) {
            *chained = false;
            return n;
        }
        (*left)--;
        *chained = true;
        /* gba_recomp_run's checks before it hands the next instruction over, the simple case */
        if (m->now >= m->next_event || m->cpu.halted || m->cpu.stopped || irq_due(m) ||
            (npc - 0x08000000u >= 0x06000000u && npc - iw_lo >= iw_hi - iw_lo))
            return n;
    }
}

/* ---- predecoded blocks (XBA-208) ------------------------------------------------------------------------- *
 *
 * gba_cpu_run (and the recompiler's loop for code no pack holds) runs straight runs of code from the cartridge ROM,
 * IWRAM, EWRAM or the BIOS decoded once into ops: the frequent instructions with their operands extracted, the
 * condition apart, PC-relative values and branch targets computed; every other instruction is a generic op that
 * runs the decoders above (arm_execute / thumb_execute) as step() does. A block ends after an unconditional
 * control transfer, at a 256-byte page end or after BLK_MAX_OPS ops. A run goes on into the next block after a taken
 * branch in the same state and region (a loop back to the block's start without a lookup). The flags live in locals
 * during a run, the registers in the machine.
 *
 * The run loop's per-instruction checks hold inside a run: an op starts only while the clock is below the next
 * event (as `now < next_event` before each step()). The IRQ line, a halt, the next event, the wait-state tables and
 * the code only change through an IO register write, a DMA, a write to code or a generic op, which all bump
 * m->blk_attn: the run stops after that instruction, and the run loop makes its checks. Each instruction costs what
 * step() charges, in the same order: the opcode fetch (the region's sequential fetch, through the prefetch buffer in
 * the cartridge when it is on), the data accesses, the internal cycles, the next fetch's non-sequential extra
 * (nfetch_extra) and the refill after a branch. EWRAM / IWRAM accesses skip the bus (same cycles, no other effect)
 * while no DMA is due; the others go through it with the clock and r15 where step() has them (the bus reads r15 for
 * the BIOS's read protection and the prefetch buffer). The opcode fetch's latches (open bus, BIOS latch) are set
 * before a bus access and at the run's end, from memory that has not changed since the fetch.
 *
 * Invalidation: RAM blocks belong to a 256-byte page with a generation; a write to a RAM halfword some block was
 * decoded from (the code map, a bit per halfword, checked by every bus write and cheat poke) bumps its page's
 * generation, clears the page's bits and stops a running block after that instruction (self-modifying code, routines
 * copied to IWRAM). Lifted code's inline EWRAM / IWRAM stores skip the bus and the code map (XBA-281): after lifted
 * code ran (m->rc_ran), the next run from blocks bumps the blocks' epoch, and a RAM block validated in an older epoch
 * has its opcodes compared with RAM before it runs (decoded again when they differ). Writes behind the bus (save
 * state loads, the BIOS's RAM resets, cartridge patches) flush everything. Between the run loop's stops the machine is byte-identical to step()'s loop, which stays as the
 * reference (gba_cpu_set_reference, gba_run --lockstep-ref). The blocks are host state, outside save states. */

#define BLK_PAGE_SHIFT 8
#define BLK_RAM_SPACE (GBA_IWRAM_SIZE + GBA_EWRAM_SIZE) /* gba_code_write's index space */
#define BLK_PAGES (BLK_RAM_SPACE >> BLK_PAGE_SHIFT)
#define BLK_HASH 65536u
#define BLK_MAX_OPS 64
#define BLK_POOL (BLK_HASH * 16u)
#define BLK_NOPAGE 0xFFFFu
#define BLK_EMPTY 2u /* no block: an ARM key is word aligned, a Thumb key odd */

enum { RG_NONE, RG_CART, RG_IWRAM, RG_BIOS, RG_EWRAM };

typedef struct blk_op {
    uint8_t k;      /* TK_* / AK_* */
    uint8_t cond;   /* ARM: the condition; Thumb Bcond: its condition */
    uint8_t a, b, c, d;
    uint8_t x, y;
    uint32_t imm;
    uint32_t raw;   /* the opcode (generic ops) */
} blk_op;

typedef struct gba_block {
    uint32_t key;   /* pc | thumb */
    uint32_t gen;   /* the page's generation when decoded */
    uint32_t vep;   /* RAM blocks: the epoch its opcodes were last known to match RAM in (gba_blocks.epoch) */
    uint16_t page;  /* RAM page, BLK_NOPAGE for ROM / BIOS */
    uint16_t n;
    uint8_t region;
    blk_op *ops;
} gba_block;

struct gba_blocks {
    gba_block tab[BLK_HASH];
    uint32_t used;
    uint32_t gen[BLK_PAGES];
    uint32_t epoch; /* bumped when lifted code may have stored into RAM since (m->rc_ran) */
    uint8_t code[BLK_RAM_SPACE / 16];   /* a bit per halfword (m->code_map) */
    blk_op pool[BLK_POOL];
};

void gba_cpu_flush_blocks(gba_machine *m)
{
    struct gba_blocks *bs = m->blocks;
    if (!bs)
        return;
    for (uint32_t i = 0; i < BLK_HASH; i++)
        bs->tab[i].key = BLK_EMPTY;
    bs->used = 0;
    m->blk_attn++;
    memset(bs->code, 0, sizeof bs->code);
}

void gba_cpu_free(gba_machine *m)
{
    free(m->blocks);
    m->blocks = NULL;
    m->code_map = NULL;
}

void gba_cpu_code_written(gba_machine *m, uint32_t idx)
{
    struct gba_blocks *bs = m->blocks;
    uint32_t page = idx >> BLK_PAGE_SHIFT;
    bs->gen[page]++;
    memset(&bs->code[page << (BLK_PAGE_SHIFT - 4)], 0, 1u << (BLK_PAGE_SHIFT - 4));
    m->blk_attn++;
}

void gba_cpu_set_reference(gba_machine *m, int on)
{
    m->interp_ref = on != 0;
}

/* The region an instruction at pc is fetched from, as fetch16_seq / fetch32_seq / gba_fetch16 / gba_fetch32 tell
 * them apart; RG_NONE: not run from blocks (palette, VRAM, OAM, past the ROM's end, unmapped). */
static inline int blk_region(const gba_machine *m, uint32_t pc, bool thumb)
{
    uint32_t r = (pc >> 24) & 15, a = pc & (thumb ? 0x1FFFFFEu : 0x1FFFFFCu);
    if (r - 8u <= 5u && a < m->rom_size)
        return RG_CART;
    if (r == 3)
        return RG_IWRAM;
    if (pc < GBA_BIOS_SIZE)
        return RG_BIOS;
    if ((pc >> 24) == 2)
        return RG_EWRAM;
    return RG_NONE;
}

static inline uint32_t iwram_half(const gba_machine *m, uint32_t addr)
{
    return rd16(&m->iwram[addr & (GBA_IWRAM_SIZE - 2)]);
}

/* What the opcode fetch of the instruction at pc leaves in the latches (fetch16_seq / fetch32_seq and the bus's
 * gba_fetch16 / gba_fetch32 for EWRAM). */
static __attribute__((noinline)) void blk_latch(gba_machine *m, uint32_t pc, bool thumb, int region)
{
    switch (region) {
    case RG_CART:
        m->open_bus = thumb ? gba_rom_half(m, pc + 4) * 0x00010001u
                            : gba_rom_half(m, pc + 8) | (uint32_t)gba_rom_half(m, pc + 10) << 16;
        break;
    case RG_IWRAM:
        if (!thumb)
            m->open_bus = rd32(&m->iwram[(pc + 8) & (GBA_IWRAM_SIZE - 4)]);
        else if (!(pc & 2))
            m->open_bus = iwram_half(m, pc + 4) | iwram_half(m, pc + 2) << 16;
        else
            m->open_bus = iwram_half(m, pc + 2) | iwram_half(m, pc + 4) << 16;
        break;
    case RG_BIOS:
        m->bios_latch = rd32(&m->bios[(pc + (thumb ? 4u : 8u)) & (GBA_BIOS_SIZE - 4)]);
        m->open_bus = m->bios_latch;
        break;
    default:
        m->open_bus = gba_prefetch_latch(m, pc, thumb);
        break;
    }
}

/* The CPU loop's stop: the next event, or gba_run_cycles's cap. */
static inline uint64_t cpu_limit(const gba_machine *m)
{
    return m->next_event < m->cpu_cap ? m->next_event : m->cpu_cap;
}

/* ---- decoding ---- */

enum {
    TK_GEN,
    TK_LSL_I, TK_LSR_I, TK_ASR_I,
    TK_ADD_R, TK_SUB_R, TK_ADD_I3, TK_SUB_I3,
    TK_MOV_I8, TK_CMP_I8, TK_ADD_I8, TK_SUB_I8,
    TK_AND, TK_EOR, TK_LSL, TK_LSR, TK_ASR, TK_ADC, TK_SBC, TK_ROR,
    TK_TST, TK_NEG, TK_CMP, TK_CMN, TK_ORR, TK_MUL, TK_BIC, TK_MVN,
    TK_HADD, TK_HCMP, TK_HMOV, TK_BX,
    TK_LDR_PC,
    TK_STR_R, TK_STRH_R, TK_STRB_R, TK_LDRSB_R, TK_LDR_R, TK_LDRH_R, TK_LDRB_R, TK_LDRSH_R,
    TK_STR_I, TK_LDR_I, TK_STRB_I, TK_LDRB_I, TK_STRH_I, TK_LDRH_I, TK_STR_SP, TK_LDR_SP,
    TK_ADD_PC, TK_ADD_SP, TK_ADJ_SP,
    TK_BCOND, TK_B, TK_BL1, TK_BL2,
    TK_PUSH, TK_POP, TK_POP_PC, TK_LDMIA, TK_STMIA,
    TK_COUNT
};

enum {
    AK_GEN,
    /* data processing, immediate operand: AK_DPI + opcode; register operand shifted by an immediate: AK_DPR + opcode */
    AK_DPI,
    AK_DPR = AK_DPI + 16,
    AK_LDR = AK_DPR + 16, AK_STR, AK_LDRB, AK_STRB,
    AK_B, AK_BL, AK_BX,
    AK_LDM, AK_STM, AK_LDRH, AK_STRH, AK_LDRSB, AK_LDRSH, AK_MRS,
    AK_COUNT
};

/* Thumb: the op for opcode `op` at pc; *ends: the block stops after it (an unconditional control transfer). */
static void decode_thumb(blk_op *o, uint32_t op, uint32_t pc, bool *ends)
{
    memset(o, 0, sizeof *o);
    o->raw = op;
    o->k = TK_GEN;
    *ends = false;
    switch (op >> 11) {
    case 0x00: case 0x01: case 0x02:
        o->k = (uint8_t)(TK_LSL_I + (op >> 11));
        o->a = op & 7;
        o->b = (op >> 3) & 7;
        o->x = (op >> 6) & 31;
        break;
    case 0x03:
        o->k = (uint8_t)((op & 0x400) ? ((op & 0x200) ? TK_SUB_I3 : TK_ADD_I3) : ((op & 0x200) ? TK_SUB_R : TK_ADD_R));
        o->a = op & 7;
        o->b = (op >> 3) & 7;
        o->c = (op >> 6) & 7;
        break;
    case 0x04: case 0x05: case 0x06: case 0x07:
        o->k = (uint8_t)(TK_MOV_I8 + ((op >> 11) - 4));
        o->a = (op >> 8) & 7;
        o->imm = op & 0xFF;
        break;
    case 0x08:
        if (!(op & 0x400)) {
            o->k = (uint8_t)(TK_AND + ((op >> 6) & 15));
            o->a = op & 7;
            o->b = (op >> 3) & 7;
        } else {
            uint32_t rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15;
            o->a = (uint8_t)rd;
            o->b = (uint8_t)rs;
            switch ((op >> 8) & 3) {
            case 0:
                if (rd != 15 && rs != 15)
                    o->k = TK_HADD;
                else if (rd == 15)
                    *ends = true;
                break;
            case 1:
                if (rd != 15 && rs != 15)
                    o->k = TK_HCMP;
                break;
            case 2:
                if (rd != 15 && rs != 15)
                    o->k = TK_HMOV;
                else if (rd == 15)
                    *ends = true;
                break;
            default:
                if (rs != 15)
                    o->k = TK_BX;
                *ends = true;
                break;
            }
        }
        break;
    case 0x09:
        o->k = TK_LDR_PC;
        o->a = (op >> 8) & 7;
        o->imm = ((pc + 4) & ~2u) + (op & 0xFF) * 4;
        break;
    case 0x0A: case 0x0B:
        o->k = (uint8_t)(TK_STR_R + ((op >> 9) & 7));
        o->a = op & 7;
        o->b = (op >> 3) & 7;
        o->c = (op >> 6) & 7;
        break;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: {
        bool byte = op & 0x1000, load = op & 0x800;
        o->k = (uint8_t)(byte ? (load ? TK_LDRB_I : TK_STRB_I) : (load ? TK_LDR_I : TK_STR_I));
        o->a = op & 7;
        o->b = (op >> 3) & 7;
        o->imm = byte ? (op >> 6) & 31 : ((op >> 6) & 31) * 4;
        break;
    }
    case 0x10: case 0x11:
        o->k = (op & 0x800) ? TK_LDRH_I : TK_STRH_I;
        o->a = op & 7;
        o->b = (op >> 3) & 7;
        o->imm = ((op >> 6) & 31) * 2;
        break;
    case 0x12: case 0x13:
        o->k = (op & 0x800) ? TK_LDR_SP : TK_STR_SP;
        o->a = (op >> 8) & 7;
        o->imm = (op & 0xFF) * 4;
        break;
    case 0x14:
        o->k = TK_ADD_PC;
        o->a = (op >> 8) & 7;
        o->imm = ((pc + 4) & ~2u) + (op & 0xFF) * 4;
        break;
    case 0x15:
        o->k = TK_ADD_SP;
        o->a = (op >> 8) & 7;
        o->imm = (op & 0xFF) * 4;
        break;
    case 0x16: case 0x17:
        if ((op & 0xF00) == 0x000) {
            uint32_t off = (op & 0x7F) * 4;
            o->k = TK_ADJ_SP;
            o->imm = (op & 0x80) ? 0u - off : off;
        } else if ((op & 0x600) == 0x400) { /* PUSH / POP (x: LR / PC too, y: registers moved) */
            uint32_t list = op & 0xFF, extra = (op >> 8) & 1;
            if (!list && !extra) { /* empty list: generic (POP moves PC) */
                *ends = (op & 0x800) != 0;
            } else {
                o->k = (op & 0x800) ? (extra ? TK_POP_PC : TK_POP) : TK_PUSH;
                o->imm = list;
                o->x = (uint8_t)extra;
                o->y = (uint8_t)__builtin_popcount(list);
                *ends = o->k == TK_POP_PC;
            }
        } else {
            *ends = true; /* undefined */
        }
        break;
    case 0x18: case 0x19: /* LDMIA / STMIA; an empty list (generic) moves PC */
        if (!(op & 0xFF)) {
            *ends = (op & 0x800) != 0;
        } else {
            o->k = (op & 0x800) ? TK_LDMIA : TK_STMIA;
            o->b = (op >> 8) & 7;
            o->imm = op & 0xFF;
            o->y = (uint8_t)__builtin_popcount(op & 0xFF);
        }
        break;
    case 0x1A: case 0x1B: {
        uint32_t cond = (op >> 8) & 15;
        if (cond >= 14) {
            *ends = true; /* SWI, undefined */
        } else {
            o->k = TK_BCOND;
            o->cond = (uint8_t)cond;
            o->imm = pc + 4 + (uint32_t)((int32_t)(int8_t)(op & 0xFF) * 2);
        }
        break;
    }
    case 0x1C:
        o->k = TK_B;
        o->imm = pc + 4 + (uint32_t)(((int32_t)(op << 21)) >> 20);
        *ends = true;
        break;
    case 0x1E:
        o->k = TK_BL1;
        o->imm = pc + 4 + (uint32_t)(((int32_t)(op << 21)) >> 9);
        break;
    case 0x1F:
        o->k = TK_BL2;
        o->imm = (op & 0x7FF) * 2;
        *ends = true;
        break;
    default:
        *ends = true; /* 0x1D: undefined */
        break;
    }
}

/* ARM: the op for opcode `op` at pc. */
static void decode_arm(blk_op *o, uint32_t op, uint32_t pc, bool *ends)
{
    memset(o, 0, sizeof *o);
    o->raw = op;
    o->k = AK_GEN;
    o->cond = (uint8_t)(op >> 28);
    bool al = o->cond == 0xE;
    *ends = false;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x0FC000F0) == 0x00000090 || (op & 0x0F8000F0) == 0x00800090) {
            /* multiplies: generic */
        } else if ((op & 0x0FB00FF0) == 0x01000090) {
            if (rd == 15)
                *ends = al; /* SWP into PC */
        } else if ((op & 0x0FFFFFF0) == 0x012FFF10) {
            if (rm != 15) {
                o->k = AK_BX;
                o->c = (uint8_t)rm;
            }
            *ends = al;
        } else if ((op & 0x90) == 0x90 && (op & 0x60)) {
            /* halfword / signed transfers: x = P | U << 1 | W << 2 | immediate offset << 3 */
            bool p = (op >> 24) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1, imm = (op >> 22) & 1;
            uint32_t sh = (op >> 5) & 3;
            if (l && rd == 15) {
                *ends = al; /* halfword load into PC */
            } else if (rd != 15 && (l || sh == 1) && !(rn == 15 && (!p || w)) && (imm || rm != 15)) {
                o->k = (uint8_t)(!l ? AK_STRH : sh == 1 ? AK_LDRH : sh == 2 ? AK_LDRSB : AK_LDRSH);
                o->a = (uint8_t)rd;
                o->b = (uint8_t)rn;
                o->c = (uint8_t)rm;
                o->x = (uint8_t)((p ? 1 : 0) | (((op >> 23) & 1) ? 2 : 0) | (w ? 4 : 0) | (imm ? 8 : 0));
                o->imm = ((op >> 4) & 0xF0) | (op & 0xF);
            }
        } else if ((op & 0x01900000) == 0x01000000) {
            /* PSR transfers (MSR / MRS) or undefined */
            if ((op & 0x0FBF0FFF) == 0x010F0000) {
                if (rd != 15) {
                    o->k = AK_MRS;
                    o->a = (uint8_t)rd;
                    o->x = (op >> 22) & 1;
                }
            } else if ((op & 0x0DB0F000) != 0x0120F000) {
                *ends = al;
            }
        } else if (rd == 15) {
            *ends = al; /* writes PC (or CPSR = SPSR: TSTP and the like) */
        } else if (!(op & 0x10) && rn != 15 && rm != 15) {
            o->k = (uint8_t)(AK_DPR + ((op >> 21) & 15));
            o->a = (uint8_t)rd;
            o->b = (uint8_t)rn;
            o->c = (uint8_t)rm;
            o->d = (op >> 5) & 3;
            o->x = (op >> 7) & 31;
            o->y = (op >> 20) & 1;
        }
        break;
    case 1:
        if ((op & 0x01900000) == 0x01000000) {
            if ((op & 0x0FB0F000) != 0x0320F000)
                *ends = al; /* undefined */
        } else if (rd == 15) {
            *ends = al;
        } else if (rn != 15) {
            uint32_t rot = (op >> 7) & 0x1E;
            o->k = (uint8_t)(AK_DPI + ((op >> 21) & 15));
            o->a = (uint8_t)rd;
            o->b = (uint8_t)rn;
            o->imm = ror32(op & 0xFF, rot);
            o->x = rot != 0;             /* the shifter's carry out is bit 31 of the operand, else C */
            o->d = (uint8_t)(o->imm >> 31);
            o->y = (op >> 20) & 1;
        }
        break;
    case 2:
    case 3: {
        if (((op >> 25) & 7) == 3 && (op & 0x10)) {
            *ends = al; /* undefined (the built-in BIOS's HLE trap among them) */
            break;
        }
        bool p = (op >> 24) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1, b = (op >> 22) & 1;
        bool reg = (op >> 25) & 1;
        if (rd == 15) {
            if (l)
                *ends = al;
            break; /* load into PC, or a store of PC + 12: generic */
        }
        if (rn == 15 && (!p || w))
            break;
        if (reg && rm == 15)
            break;
        o->k = (uint8_t)(l ? (b ? AK_LDRB : AK_LDR) : (b ? AK_STRB : AK_STR));
        o->a = (uint8_t)rd;
        o->b = (uint8_t)rn;
        o->x = (uint8_t)((p ? 1 : 0) | (((op >> 23) & 1) ? 2 : 0) | (w ? 4 : 0) | (reg ? 8 : 0));
        if (reg) {
            o->c = (uint8_t)rm;
            o->d = (op >> 5) & 3;
            o->y = (op >> 7) & 31;
        } else {
            o->imm = op & 0xFFF;
        }
        break;
    }
    case 4: { /* LDM / STM (x = P | U << 1 | W << 2, y: registers); with PC, the S bit or an empty list: generic */
        uint32_t list = op & 0xFFFF;
        bool l = (op >> 20) & 1;
        if (!(op & (1u << 22)) && list && !(list & 0x8000) && rn != 15) {
            o->k = l ? AK_LDM : AK_STM;
            o->b = (uint8_t)rn;
            o->imm = list;
            o->x = (uint8_t)(((op >> 24) & 1) | (((op >> 23) & 1) << 1) | (((op >> 21) & 1) << 2));
            o->y = (uint8_t)__builtin_popcount(list);
        } else if (l && ((list & 0x8000) || !list)) {
            *ends = al; /* moves PC */
        }
        break;
    }
    case 5:
        o->k = (op & (1u << 24)) ? AK_BL : AK_B;
        o->imm = pc + 8 + (uint32_t)(((int32_t)(op << 8)) >> 6);
        *ends = al;
        break;
    default: /* coprocessor (undefined), SWI */
        *ends = al;
        break;
    }
}

static gba_block *blk_decode(gba_machine *m, gba_block *b, uint32_t pc, bool thumb, int region, uint32_t maxops)
{
    struct gba_blocks *bs = m->blocks;
    if (bs->used + BLK_MAX_OPS > BLK_POOL)
        gba_cpu_flush_blocks(m); /* the slot b stays valid */
    blk_op *ops = &bs->pool[bs->used];
    uint32_t w = thumb ? 2 : 4, n = 0, idx0 = 0;
    bool ram = region == RG_IWRAM || region == RG_EWRAM;
    if (region == RG_IWRAM)
        idx0 = GBA_CODE_IWRAM(pc);
    else if (region == RG_EWRAM)
        idx0 = GBA_CODE_EWRAM(pc);
    for (uint32_t a = pc; n < maxops && ((a ^ pc) >> BLK_PAGE_SHIFT) == 0; a += w) {
        uint32_t op;
        switch (region) {
        case RG_CART: {
            uint32_t o = a & 0x1FFFFFFu;
            if (o + w > m->rom_size)
                goto done;
            op = thumb ? rd16(&m->rom[o]) : rd32(&m->rom[o]);
            break;
        }
        case RG_IWRAM:
            op = thumb ? rd16(&m->iwram[a & (GBA_IWRAM_SIZE - 2)]) : rd32(&m->iwram[a & (GBA_IWRAM_SIZE - 4)]);
            break;
        case RG_EWRAM:
            op = thumb ? rd16(&m->ewram[a & (GBA_EWRAM_SIZE - 2)]) : rd32(&m->ewram[a & (GBA_EWRAM_SIZE - 4)]);
            break;
        default:
            op = thumb ? rd16(&m->bios[a]) : rd32(&m->bios[a]);
            break;
        }
        bool ends;
        if (thumb)
            decode_thumb(&ops[n], op, a, &ends);
        else
            decode_arm(&ops[n], op, a, &ends);
        if (ram) {
            uint32_t i = idx0 + (a - pc);
            bs->code[i >> 4] |= (uint8_t)((thumb ? 1u : 3u) << ((i >> 1) & 7));
        }
        n++;
        if (ends)
            break;
    }
done:
    if (n == 0)
        return NULL;
    bs->used += n;
    b->key = pc | (thumb ? 1u : 0u);
    b->n = (uint16_t)n;
    b->region = (uint8_t)region;
    b->ops = ops;
    b->page = ram ? (uint16_t)(idx0 >> BLK_PAGE_SHIFT) : BLK_NOPAGE;
    b->gen = ram ? bs->gen[b->page] : 0;
    b->vep = bs->epoch;
    return b;
}

/* A RAM block of a past epoch: RAM still holds the opcodes it was decoded from. */
static bool blk_same(const gba_machine *m, const gba_block *b)
{
    bool thumb = (b->key & 1) != 0;
    uint32_t pc = b->key & ~1u, w = thumb ? 2 : 4;
    const uint8_t *base = b->region == RG_IWRAM ? m->iwram : m->ewram;
    uint32_t mask = b->region == RG_IWRAM ? GBA_IWRAM_SIZE - 1 : GBA_EWRAM_SIZE - 1;
    for (uint32_t i = 0; i < b->n; i++) {
        uint32_t a = (pc + i * w) & mask;
        if ((thumb ? rd16(&base[a]) : rd32(&base[a])) != b->ops[i].raw)
            return false;
    }
    return true;
}

static __attribute__((noinline)) gba_block *blk_get_slow(gba_machine *m, uint32_t pc, bool thumb, uint32_t maxops)
{
    struct gba_blocks *bs = m->blocks;
    if (!bs) {
        bs = m->blocks = malloc(sizeof *bs);
        if (!bs)
            return NULL;
        memset(bs->gen, 0, sizeof bs->gen);
        bs->epoch = 0;
        m->code_map = bs->code;
        gba_cpu_flush_blocks(m);
    }
    uint32_t key = pc | (thumb ? 1u : 0u);
    gba_block *b = &bs->tab[((key >> 1) ^ (key >> 14)) & (BLK_HASH - 1)];
    if (b->key == key && (b->page == BLK_NOPAGE || bs->gen[b->page] == b->gen)) {
        if (b->page == BLK_NOPAGE || b->vep == bs->epoch)
            return b;
        if (blk_same(m, b)) {
            b->vep = bs->epoch;
            return b;
        }
    }
    int region = blk_region(m, pc, thumb);
    if (region == RG_NONE)
        return NULL;
    return blk_decode(m, b, pc, thumb, region, maxops);
}

/* The block at pc (decoded now if needed, of maxops ops at most), NULL when pc is not in a region blocks run from. */
static inline __attribute__((always_inline)) const gba_block *blk_get(gba_machine *m, uint32_t pc, bool thumb,
                                                                    uint32_t maxops)
{
    struct gba_blocks *bs = m->blocks;
    uint32_t key = pc | (thumb ? 1u : 0u);
    if (__builtin_expect(bs != NULL, 1)) {
        const gba_block *b = &bs->tab[((key >> 1) ^ (key >> 14)) & (BLK_HASH - 1)];
        if (b->key == key && (b->page == BLK_NOPAGE || (bs->gen[b->page] == b->gen && b->vep == bs->epoch)))
            return b;
    }
    return blk_get_slow(m, pc, thumb, maxops);
}

/* ---- running ---- */

/* rc_refill_cost, its table case inline. */
static inline uint32_t refill_cost(gba_machine *m, uint64_t now, uint32_t r15, bool thumb)
{
    uint32_t r = (r15 >> 24) & 15;
    if (__builtin_expect(r - 8u <= 5u && m->prefetch, 0))
        return rc_refill_cost(HEAD(m), now, r15, thumb);
    return thumb ? (uint32_t)m->wait16[r][0] + m->wait16[r][1] : (uint32_t)m->wait32[r][0] + m->wait32[r][1];
}

static __attribute__((noinline)) void exec_thumb(gba_machine *m, uint32_t op, uint32_t pc)
{
    thumb_execute(m, op, pc);
}

static __attribute__((noinline)) void exec_arm(gba_machine *m, uint32_t op, uint32_t pc)
{
    arm_execute(m, op, pc);
}

/* The flags live in locals (fN fZ fC fV, 0 or 1) inside a run: the helpers below are the ones above on them. */
static inline uint32_t l_adc(uint32_t a, uint32_t b, uint32_t cin, uint32_t *n, uint32_t *z, uint32_t *c,
                             uint32_t *v)
{
    uint64_t r = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)r;
    *n = res >> 31;
    *z = res == 0;
    *c = (uint32_t)(r >> 32);
    *v = (~(a ^ b) & (a ^ res)) >> 31;
    return res;
}

static inline uint32_t l_shift_imm(uint32_t type, uint32_t v, uint32_t amt, uint32_t cin, uint32_t *carry)
{
    switch (type) {
    case 0:
        if (amt == 0) {
            *carry = cin;
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
            return cin << 31 | v >> 1;
        }
        *carry = (v >> (amt - 1)) & 1;
        return ror32(v, amt);
    }
}

/* Condition codes by NZCV (bit 3 = N .. bit 0 = V): bit nzcv of cond_lut[cond] tells whether cond passes. */
static const uint16_t cond_lut[16] = {
    0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
    0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000,
};
#define L_COND(CC) ((cond_lut[(CC)] >> (fN << 3 | fZ << 2 | fC << 1 | fV)) & 1)

#define FLAGS_OUT() (m->cpu.n = fN, m->cpu.z = fZ, m->cpu.c = fC, m->cpu.v = fV)
#define FLAGS_IN() (fN = m->cpu.n, fZ = m->cpu.z, fC = m->cpu.c, fV = m->cpu.v)
#define SET_NZ(RES) (fN = (RES) >> 31, fZ = (RES) == 0)
#define ADC(A, B, CIN) l_adc((A), (B), (CIN), &fN, &fZ, &fC, &fV)

/* EWRAM / IWRAM accesses without the bus call: what gba_read* / gba_write* do there (no side effect but the memory and
 * the clock), on the run's clock, while no immediate DMA is due (a non-sequential access would start it); a write
 * also keeps off the halfwords some block was decoded from and the bytes the running opcode fetch latched (around
 * pc, in the same region). Otherwise the bus does the access, with r15, the latches and the clock where step() has
 * them. */
static inline uint8_t *ram_ptr(gba_machine *m, uint32_t a)
{
    return (a >> 24) == 3 ? &m->iwram[a & (GBA_IWRAM_SIZE - 1)] : &m->ewram[a & (GBA_EWRAM_SIZE - 1)];
}

static inline bool ram_w_clean(const gba_machine *m, uint32_t a, unsigned nhalf, uint32_t pc, uint32_t lreg,
                               uint32_t lmask)
{
    uint32_t idx = (a >> 24) == 3 ? GBA_CODE_IWRAM(a) : GBA_CODE_EWRAM(a);
    if ((m->code_map[idx >> 4] >> ((idx >> 1) & 7)) & (nhalf == 2 ? 3u : 1u))
        return false;
    return (a >> 24) != lreg || ((a - pc + 4) & lmask) >= 20;
}

#define RAM_R(A, SEQ) ((((A) >> 24) - 2u) <= 1u && ((SEQ) || now < m->dma_due))
#define RAM_W(A, NH, SEQ) (RAM_R(A, SEQ) && ram_w_clean(m, (A), (NH), pc, lreg, lmask))
#define WAIT16(A, SEQ) ((uint32_t)m->wait16[(A) >> 24][(SEQ)])
#define WAIT32(A, SEQ) ((uint32_t)m->wait32[(A) >> 24][(SEQ)])

/* Shared by both runners. W: the opcode size; PIPE: r15 - pc; THUMB. */

#define BLK_NEXT()                                                                                                   \
    do {                                                                                                             \
        op++;                                                                                                        \
        pc += W;                                                                                                     \
        if (__builtin_expect(op == end, 0))                                                                          \
            goto out;                                                                                                \
        goto top;                                                                                                    \
    } while (0)

/* Before a bus access: r15, the fetch's latches and the clock where step() has them. */
#define BLK_BUS()                                                                                                    \
    do {                                                                                                             \
        R[15] = pc + PIPE;                                                                                           \
        if (!latched) {                                                                                              \
            blk_latch(m, pc, THUMB, region);                                                                         \
            latched = true;                                                                                          \
        }                                                                                                            \
        m->now = now;                                                                                                \
    } while (0)

/* After it (ICYC: the instruction's internal cycles): the next fetch's non-sequential extra. Anything the run loop
 * checks between instructions (the IRQ line, a halt, the next event), the wait states and the code only change
 * through an IO register write, a DMA or a write to code, which bump blk_attn: then the block stops after this
 * instruction (with the extra from the wait states as they are now, as step() computes it). */
#define BLK_BUS_END(ICYC)                                                                                            \
    do {                                                                                                             \
        now = m->now + (ICYC);                                                                                       \
        if (__builtin_expect(m->blk_attn != attn0, 0)) {                                                             \
            now += nfetch_extra(m, pc, THUMB);                                                                       \
            op++;                                                                                                    \
            pc += W;                                                                                                 \
            goto out;                                                                                                \
        }                                                                                                            \
        now += nx;                                                                                                   \
    } while (0)

/* A word of a multiple transfer (seq: sequential), by the RAM path or the bus; MT_END after the last one. */
#define MT_RD(A, SEQ, DST)                                                                                           \
    do {                                                                                                             \
        uint32_t a_ = (A);                                                                                           \
        if (RAM_R(a_, SEQ)) {                                                                                        \
            now += WAIT32(a_, SEQ);                                                                                  \
            DST = rd32(ram_ptr(m, a_));                                                                              \
        } else {                                                                                                     \
            BLK_BUS();                                                                                               \
            DST = gba_read32(m, a_, SEQ);                                                                            \
            now = m->now;                                                                                            \
        }                                                                                                            \
    } while (0)
#define MT_WR(A, V, SEQ)                                                                                             \
    do {                                                                                                             \
        uint32_t a_ = (A);                                                                                           \
        if (RAM_W(a_, 2, SEQ)) {                                                                                     \
            now += WAIT32(a_, SEQ);                                                                                  \
            wr32(ram_ptr(m, a_), (V));                                                                               \
        } else {                                                                                                     \
            BLK_BUS();                                                                                               \
            gba_write32(m, a_, (V), SEQ);                                                                            \
            now = m->now;                                                                                            \
        }                                                                                                            \
    } while (0)
#define MT_END(ICYC)                                                                                                 \
    do {                                                                                                             \
        m->now = now;                                                                                                \
        BLK_BUS_END(ICYC);                                                                                           \
    } while (0)
#define MT_SEQ(A) ((((A) + 4) >> 24) == ((A) >> 24)) /* the next word in the same region: sequential */

/* A generic op: step()'s execute and its aftermath, the flags in the machine. */
#define BLK_GENERIC(EXEC)                                                                                            \
    do {                                                                                                             \
        BLK_BUS();                                                                                                   \
        FLAGS_OUT();                                                                                                 \
        m->cpu.branched = false;                                                                                     \
        uint32_t ctl_ = m->cpu.cpsr_ctl;                                                                             \
        EXEC(m, op->raw, pc);                                                                                        \
        if (m->cpu.branched) {                                                                                       \
            m->now += rc_refill_cost(h, m->now, R[15], is_thumb(m));                                                 \
            return (uint32_t)(op - cur->ops) + 1;                                                                    \
        }                                                                                                            \
        R[15] += W;                                                                                                  \
        if (m->now != now)                                                                                           \
            m->now += nfetch_extra(m, pc, THUMB);                                                                    \
        if (__builtin_expect((m->irq_line && !(m->cpu.cpsr_ctl & CPSR_I)) | m->cpu.halted | m->cpu.stopped |        \
                                 (m->blk_attn != attn0) | (m->cpu.cpsr_ctl != ctl_),                                 \
                             0))                                                                                     \
            return (uint32_t)(op - cur->ops) + 1;                                                                    \
        now = m->now;                                                                                                \
        ne = cpu_limit(m);                                                                                           \
        FLAGS_IN();                                                                                                  \
        BLK_NEXT();                                                                                                  \
    } while (0)

/* A taken branch (R[15] written for the target): the refill; then the target's block runs on in this loop when it is
 * in the same state and region and the clock is still below the limit (the run loop's other checks cannot have
 * changed since blk_attn was last checked), else the block stops. */
#define BLK_BRANCH() goto branch_taken

#define BLK_PROLOGUE(TH)                                                                                             \
    gba_rc_head *const h = HEAD(m);                                                                                  \
    const gba_block *cur = b;                                                                                        \
    const blk_op *op = b->ops, *end = b->ops + (lim && lim < b->n ? lim : b->n);                                     \
    uint32_t pc = b->key & ~1u;                                                                                      \
    const int region = b->region;                                                                                    \
    const uint32_t rg = (pc >> 24) & 15;                                                                             \
    uint64_t now = m->now, ne = cpu_limit(m);                                                                        \
    const bool pf = region == RG_CART && m->prefetch;                                                                \
    const uint32_t fc = TH ? m->fetch16[rg] : m->fetch32[rg];                                                        \
    const uint32_t ncost = TH ? m->wait16[rg][0] : m->wait32[rg][0];                                                 \
    const uint32_t nx = nfetch_extra(m, pc, TH);                                                                     \
    const uint32_t attn0 = m->blk_attn;                                                                              \
    const uint32_t lreg = region == RG_IWRAM ? 3u : region == RG_EWRAM ? 2u : 0xFFu;                                  \
    const uint32_t lmask = region == RG_IWRAM ? GBA_IWRAM_SIZE - 1 : GBA_EWRAM_SIZE - 1;                             \
    uint32_t fN = m->cpu.n, fZ = m->cpu.z, fC = m->cpu.c, fV = m->cpu.v;                                             \
    bool latched = true

/* The block's normal end and the stops between instructions: pc is the next instruction's. */
#define BLK_OUT()                                                                                                    \
    out:                                                                                                             \
    R[15] = pc + PIPE;                                                                                               \
    m->cpu.branched = false;                                                                                         \
    if (!latched)                                                                                                    \
        blk_latch(m, pc - W, THUMB, region);                                                                         \
    m->now = now;                                                                                                    \
    FLAGS_OUT();                                                                                                     \
    return (uint32_t)(op - cur->ops)

#define THUMB true
#define W 2u
#define PIPE 4u

static uint32_t blk_run_thumb(gba_machine *m, const gba_block *b, uint32_t lim)
{
    static const void *const lab[TK_COUNT] = {
        [TK_GEN] = &&L_GEN, [TK_LSL_I] = &&L_LSL_I, [TK_LSR_I] = &&L_LSR_I, [TK_ASR_I] = &&L_ASR_I,
        [TK_ADD_R] = &&L_ADD_R, [TK_SUB_R] = &&L_SUB_R, [TK_ADD_I3] = &&L_ADD_I3, [TK_SUB_I3] = &&L_SUB_I3,
        [TK_MOV_I8] = &&L_MOV_I8, [TK_CMP_I8] = &&L_CMP_I8, [TK_ADD_I8] = &&L_ADD_I8, [TK_SUB_I8] = &&L_SUB_I8,
        [TK_AND] = &&L_AND, [TK_EOR] = &&L_EOR, [TK_LSL] = &&L_LSL, [TK_LSR] = &&L_LSR, [TK_ASR] = &&L_ASR,
        [TK_ADC] = &&L_ADC, [TK_SBC] = &&L_SBC, [TK_ROR] = &&L_ROR, [TK_TST] = &&L_TST, [TK_NEG] = &&L_NEG,
        [TK_CMP] = &&L_CMP, [TK_CMN] = &&L_CMN, [TK_ORR] = &&L_ORR, [TK_MUL] = &&L_MUL, [TK_BIC] = &&L_BIC,
        [TK_MVN] = &&L_MVN, [TK_HADD] = &&L_HADD, [TK_HCMP] = &&L_HCMP, [TK_HMOV] = &&L_HMOV, [TK_BX] = &&L_BX,
        [TK_LDR_PC] = &&L_LDR_PC, [TK_STR_R] = &&L_STR_R, [TK_STRH_R] = &&L_STRH_R, [TK_STRB_R] = &&L_STRB_R,
        [TK_LDRSB_R] = &&L_LDRSB_R, [TK_LDR_R] = &&L_LDR_R, [TK_LDRH_R] = &&L_LDRH_R, [TK_LDRB_R] = &&L_LDRB_R,
        [TK_LDRSH_R] = &&L_LDRSH_R, [TK_STR_I] = &&L_STR_I, [TK_LDR_I] = &&L_LDR_I, [TK_STRB_I] = &&L_STRB_I,
        [TK_LDRB_I] = &&L_LDRB_I, [TK_STRH_I] = &&L_STRH_I, [TK_LDRH_I] = &&L_LDRH_I, [TK_STR_SP] = &&L_STR_SP,
        [TK_LDR_SP] = &&L_LDR_SP, [TK_ADD_PC] = &&L_ADD_PC, [TK_ADD_SP] = &&L_ADD_SP, [TK_ADJ_SP] = &&L_ADJ_SP,
        [TK_BCOND] = &&L_BCOND, [TK_B] = &&L_B, [TK_BL1] = &&L_BL1, [TK_BL2] = &&L_BL2,
        [TK_PUSH] = &&L_PUSH, [TK_POP] = &&L_POP, [TK_POP_PC] = &&L_POP_PC, [TK_LDMIA] = &&L_LDMIA,
        [TK_STMIA] = &&L_STMIA,
    };
    BLK_PROLOGUE(true);
    uint32_t carry, res, addr;

top:
    if (__builtin_expect(now >= ne, 0))
        goto out;
    now += pf ? rc_pf_fetch(h, now, pc + 4, true, ncost) : fc;
    latched = false;
    goto *lab[op->k];

L_GEN:
    BLK_GENERIC(exec_thumb);
L_LSL_I:
    res = l_shift_imm(0, R[op->b], op->x, fC, &carry);
    goto shift_out;
L_LSR_I:
    res = l_shift_imm(1, R[op->b], op->x, fC, &carry);
    goto shift_out;
L_ASR_I:
    res = l_shift_imm(2, R[op->b], op->x, fC, &carry);
shift_out:
    SET_NZ(res);
    fC = carry;
    R[op->a] = res;
    BLK_NEXT();
L_ADD_R:
    R[op->a] = ADC(R[op->b], R[op->c], 0);
    BLK_NEXT();
L_SUB_R:
    R[op->a] = ADC(R[op->b], ~R[op->c], 1);
    BLK_NEXT();
L_ADD_I3:
    R[op->a] = ADC(R[op->b], op->c, 0);
    BLK_NEXT();
L_SUB_I3:
    R[op->a] = ADC(R[op->b], ~(uint32_t)op->c, 1);
    BLK_NEXT();
L_MOV_I8:
    R[op->a] = op->imm;
    SET_NZ(op->imm);
    BLK_NEXT();
L_CMP_I8:
    ADC(R[op->a], ~op->imm, 1);
    BLK_NEXT();
L_ADD_I8:
    R[op->a] = ADC(R[op->a], op->imm, 0);
    BLK_NEXT();
L_SUB_I8:
    R[op->a] = ADC(R[op->a], ~op->imm, 1);
    BLK_NEXT();
L_AND:
    res = R[op->a] & R[op->b];
    goto logic_out;
L_EOR:
    res = R[op->a] ^ R[op->b];
    goto logic_out;
L_ORR:
    res = R[op->a] | R[op->b];
    goto logic_out;
L_BIC:
    res = R[op->a] & ~R[op->b];
    goto logic_out;
L_MVN:
    res = ~R[op->b];
logic_out:
    SET_NZ(res);
    R[op->a] = res;
    BLK_NEXT();
L_LSL:
    res = rc_shift_reg(0, R[op->a], R[op->b] & 0xFF, fC, &carry);
    goto rshift_out;
L_LSR:
    res = rc_shift_reg(1, R[op->a], R[op->b] & 0xFF, fC, &carry);
    goto rshift_out;
L_ASR:
    res = rc_shift_reg(2, R[op->a], R[op->b] & 0xFF, fC, &carry);
    goto rshift_out;
L_ROR:
    res = rc_shift_reg(3, R[op->a], R[op->b] & 0xFF, fC, &carry);
rshift_out:
    SET_NZ(res);
    fC = carry;
    R[op->a] = res;
    now += 1 + nx;
    BLK_NEXT();
L_ADC:
    R[op->a] = ADC(R[op->a], R[op->b], fC);
    BLK_NEXT();
L_SBC:
    R[op->a] = ADC(R[op->a], ~R[op->b], fC);
    BLK_NEXT();
L_TST:
    res = R[op->a] & R[op->b];
    SET_NZ(res);
    BLK_NEXT();
L_NEG:
    R[op->a] = ADC(0, ~R[op->b], 1);
    BLK_NEXT();
L_CMP:
    ADC(R[op->a], ~R[op->b], 1);
    BLK_NEXT();
L_CMN:
    ADC(R[op->a], R[op->b], 0);
    BLK_NEXT();
L_MUL: {
    uint32_t a = R[op->a];
    now += (uint32_t)mul_cycles(a, true) + nx;
    res = a * R[op->b];
    SET_NZ(res);
    R[op->a] = res;
    BLK_NEXT();
}
L_HADD:
    R[op->a] = R[op->a] + R[op->b];
    BLK_NEXT();
L_HCMP:
    ADC(R[op->a], ~R[op->b], 1);
    BLK_NEXT();
L_HMOV:
    R[op->a] = R[op->b];
    BLK_NEXT();
L_BX:
    branch_exchange(m, R[op->b]);
    BLK_BRANCH();
L_LDR_PC:
    BLK_BUS();
    R[op->a] = gba_read32(m, op->imm, false);
    BLK_BUS_END(1);
    BLK_NEXT();
/* Loads and stores, addr set. */
#define T_LD32()                                                                                                     \
    do {                                                                                                             \
        if (RAM_R(addr, 0)) {                                                                                        \
            now += WAIT32(addr, 0) + 1 + nx;                                                                         \
            R[op->a] = ror32(rd32(ram_ptr(m, addr & ~3u)), (addr & 3) * 8);                                          \
            BLK_NEXT();                                                                                              \
        }                                                                                                            \
        BLK_BUS();                                                                                                   \
        R[op->a] = load32_rot(m, addr, false);                                                                       \
        BLK_BUS_END(1);                                                                                              \
        BLK_NEXT();                                                                                                  \
    } while (0)
#define T_LDH()                                                                                                      \
    do {                                                                                                             \
        if (RAM_R(addr, 0)) {                                                                                        \
            now += WAIT16(addr, 0) + 1 + nx;                                                                         \
            R[op->a] = ror32(rd16(ram_ptr(m, addr & ~1u)), (addr & 1) * 8);                                          \
            BLK_NEXT();                                                                                              \
        }                                                                                                            \
        BLK_BUS();                                                                                                   \
        R[op->a] = ror32(gba_read16(m, addr, false), (addr & 1) * 8);                                                \
        BLK_BUS_END(1);                                                                                              \
        BLK_NEXT();                                                                                                  \
    } while (0)
#define T_LDB(SIGNED)                                                                                                \
    do {                                                                                                             \
        if (RAM_R(addr, 0)) {                                                                                        \
            now += WAIT16(addr, 0) + 1 + nx;                                                                         \
            uint8_t b_ = *ram_ptr(m, addr);                                                                          \
            R[op->a] = (SIGNED) ? (uint32_t)(int32_t)(int8_t)b_ : b_;                                                \
            BLK_NEXT();                                                                                              \
        }                                                                                                            \
        BLK_BUS();                                                                                                   \
        uint8_t b_ = gba_read8(m, addr, false);                                                                      \
        R[op->a] = (SIGNED) ? (uint32_t)(int32_t)(int8_t)b_ : b_;                                                    \
        BLK_BUS_END(1);                                                                                              \
        BLK_NEXT();                                                                                                  \
    } while (0)
#define T_ST32()                                                                                                     \
    do {                                                                                                             \
        if (RAM_W(addr & ~3u, 2, 0)) {                                                                               \
            now += WAIT32(addr, 0) + nx;                                                                             \
            wr32(ram_ptr(m, addr & ~3u), R[op->a]);                                                                  \
            BLK_NEXT();                                                                                              \
        }                                                                                                            \
        BLK_BUS();                                                                                                   \
        gba_write32(m, addr, R[op->a], false);                                                                       \
        BLK_BUS_END(0);                                                                                              \
        BLK_NEXT();                                                                                                  \
    } while (0)
#define T_ST16()                                                                                                     \
    do {                                                                                                             \
        if (RAM_W(addr & ~1u, 1, 0)) {                                                                               \
            now += WAIT16(addr, 0) + nx;                                                                             \
            wr16(ram_ptr(m, addr & ~1u), (uint16_t)R[op->a]);                                                        \
            BLK_NEXT();                                                                                              \
        }                                                                                                            \
        BLK_BUS();                                                                                                   \
        gba_write16(m, addr, (uint16_t)R[op->a], false);                                                             \
        BLK_BUS_END(0);                                                                                              \
        BLK_NEXT();                                                                                                  \
    } while (0)
#define T_ST8()                                                                                                      \
    do {                                                                                                             \
        if (RAM_W(addr, 1, 0)) {                                                                                     \
            now += WAIT16(addr, 0) + nx;                                                                             \
            *ram_ptr(m, addr) = (uint8_t)R[op->a];                                                                   \
            BLK_NEXT();                                                                                              \
        }                                                                                                            \
        BLK_BUS();                                                                                                   \
        gba_write8(m, addr, (uint8_t)R[op->a], false);                                                               \
        BLK_BUS_END(0);                                                                                              \
        BLK_NEXT();                                                                                                  \
    } while (0)
L_STR_R:
    addr = R[op->b] + R[op->c];
    T_ST32();
L_STRH_R:
    addr = R[op->b] + R[op->c];
    T_ST16();
L_STRB_R:
    addr = R[op->b] + R[op->c];
    T_ST8();
L_LDRSB_R:
    addr = R[op->b] + R[op->c];
    T_LDB(true);
L_LDR_R:
    addr = R[op->b] + R[op->c];
    T_LD32();
L_LDRH_R:
    addr = R[op->b] + R[op->c];
    T_LDH();
L_LDRB_R:
    addr = R[op->b] + R[op->c];
    T_LDB(false);
L_LDRSH_R:
    addr = R[op->b] + R[op->c];
    if (addr & 1)
        T_LDB(true); /* misaligned: LDRSB */
    if (RAM_R(addr, 0)) {
        now += WAIT16(addr, 0) + 1 + nx;
        R[op->a] = (uint32_t)(int32_t)(int16_t)rd16(ram_ptr(m, addr));
        BLK_NEXT();
    }
    BLK_BUS();
    R[op->a] = (uint32_t)(int32_t)(int16_t)gba_read16(m, addr, false);
    BLK_BUS_END(1);
    BLK_NEXT();
L_STR_I:
    addr = R[op->b] + op->imm;
    T_ST32();
L_LDR_I:
    addr = R[op->b] + op->imm;
    T_LD32();
L_STRB_I:
    addr = R[op->b] + op->imm;
    T_ST8();
L_LDRB_I:
    addr = R[op->b] + op->imm;
    T_LDB(false);
L_STRH_I:
    addr = R[op->b] + op->imm;
    T_ST16();
L_LDRH_I:
    addr = R[op->b] + op->imm;
    T_LDH();
L_STR_SP:
    addr = R[13] + op->imm;
    T_ST32();
L_LDR_SP:
    addr = R[13] + op->imm;
    T_LD32();
L_PUSH: { /* x: LR too, y: the low registers */
    uint32_t list = op->imm;
    addr = R[13] - 4 * ((uint32_t)op->y + op->x);
    R[13] = addr;
    bool seq = false;
    for (; list; list &= list - 1) {
        MT_WR(addr & ~3u, R[__builtin_ctz(list)], seq);
        seq = MT_SEQ(addr);
        addr += 4;
    }
    if (op->x)
        MT_WR(addr & ~3u, R[14], seq);
    MT_END(0);
    BLK_NEXT();
}
L_POP:
L_POP_PC: {
    uint32_t list = op->imm, v = 0;
    addr = R[13];
    bool seq = false;
    for (; list; list &= list - 1) {
        MT_RD(addr & ~3u, seq, v);
        R[__builtin_ctz(list)] = v;
        seq = MT_SEQ(addr);
        addr += 4;
    }
    if (op->x) {
        MT_RD(addr & ~3u, seq, v);
        addr += 4;
        R[13] = addr;
        now += 1;
        R[15] = (v & ~1u) + 4;
        BLK_BRANCH();
    }
    R[13] = addr;
    MT_END(1);
    BLK_NEXT();
}
L_LDMIA: {
    uint32_t list = op->imm, rb = op->b, v = 0;
    addr = R[rb];
    if (!(list & (1u << rb)))
        R[rb] = addr + 4 * (uint32_t)op->y;
    bool seq = false;
    for (; list; list &= list - 1) {
        MT_RD(addr & ~3u, seq, v);
        R[__builtin_ctz(list)] = v;
        seq = MT_SEQ(addr);
        addr += 4;
    }
    MT_END(1);
    BLK_NEXT();
}
L_STMIA: {
    uint32_t list = op->imm, rb = op->b, first = (uint32_t)__builtin_ctz(list);
    addr = R[rb];
    uint32_t nb = addr + 4 * (uint32_t)op->y;
    bool seq = false;
    for (; list; list &= list - 1) {
        uint32_t i = (uint32_t)__builtin_ctz(list);
        MT_WR(addr & ~3u, (i == rb && i != first) ? nb : R[i], seq);
        seq = MT_SEQ(addr);
        addr += 4;
    }
    R[rb] = nb;
    MT_END(0);
    BLK_NEXT();
}
#undef T_LD32
#undef T_LDH
#undef T_LDB
#undef T_ST32
#undef T_ST16
#undef T_ST8
L_ADD_PC:
    R[op->a] = op->imm;
    BLK_NEXT();
L_ADD_SP:
    R[op->a] = R[13] + op->imm;
    BLK_NEXT();
L_ADJ_SP:
    R[13] += op->imm;
    BLK_NEXT();
L_BCOND:
    if (!L_COND(op->cond))
        BLK_NEXT();
    R[15] = op->imm + 4;
    BLK_BRANCH();
L_B:
    R[15] = op->imm + 4;
    BLK_BRANCH();
L_BL1:
    R[14] = op->imm;
    BLK_NEXT();
L_BL2:
    addr = R[14] + op->imm;
    R[14] = (pc + 2) | 1;
    R[15] = (addr & ~1u) + 4;
    BLK_BRANCH();

branch_taken: {
    now += refill_cost(m, now, R[15], is_thumb(m));
    if (now < ne && !lim && is_thumb(m) == THUMB) {
        uint32_t npc_ = R[15] - PIPE;
        if (npc_ == cur->key - THUMB && m->blk_attn == attn0) { /* a loop back to this block's start */
            op = cur->ops;
            pc = npc_;
            goto top;
        }
        const gba_block *nb_ = blk_get(m, npc_, THUMB, BLK_MAX_OPS);
        if (nb_ && nb_->region == region && ((npc_ >> 24) & 15) == rg && m->blk_attn == attn0) {
            cur = nb_;
            op = nb_->ops;
            end = op + nb_->n;
            pc = npc_;
            goto top;
        }
    }
    if (!latched)
        blk_latch(m, pc, THUMB, region);
    m->cpu.branched = true;
    m->now = now;
    FLAGS_OUT();
    return (uint32_t)(op - cur->ops) + 1;
}
    BLK_OUT();
}

#undef THUMB
#undef W
#undef PIPE
#define THUMB false
#define W 4u
#define PIPE 8u

/* ARM data processing (rd, rn and rm are not PC): A, OP2, SC (the shifter's carry out) set by the form's prologue. */
#define DP_LOGIC(EXPR, WRITE)                                                                                        \
    do {                                                                                                             \
        uint32_t r_ = (EXPR);                                                                                        \
        if (op->y) {                                                                                                 \
            SET_NZ(r_);                                                                                              \
            fC = sc;                                                                                                 \
        }                                                                                                            \
        if (WRITE)                                                                                                   \
            R[op->a] = r_;                                                                                           \
        BLK_NEXT();                                                                                                  \
    } while (0)
#define DP_ARITH(X, Y, CIN, WRITE, S)                                                                                \
    do {                                                                                                             \
        uint32_t r_;                                                                                                 \
        if (S)                                                                                                       \
            r_ = ADC((X), (Y), (CIN));                                                                               \
        else                                                                                                         \
            r_ = (X) + (Y) + (CIN);                                                                                  \
        if (WRITE)                                                                                                   \
            R[op->a] = r_;                                                                                           \
        BLK_NEXT();                                                                                                  \
    } while (0)
#define DP_BODY(OPC)                                                                                                 \
    switch (OPC) {                                                                                                   \
    case 0x0: DP_LOGIC(a & op2, true);                  /* AND */                                                    \
    case 0x1: DP_LOGIC(a ^ op2, true);                  /* EOR */                                                    \
    case 0x2: DP_ARITH(a, ~op2, 1, true, op->y);        /* SUB */                                                    \
    case 0x3: DP_ARITH(op2, ~a, 1, true, op->y);        /* RSB */                                                    \
    case 0x4: DP_ARITH(a, op2, 0, true, op->y);         /* ADD */                                                    \
    case 0x5: DP_ARITH(a, op2, fC, true, op->y);        /* ADC */                                                    \
    case 0x6: DP_ARITH(a, ~op2, fC, true, op->y);       /* SBC */                                                    \
    case 0x7: DP_ARITH(op2, ~a, fC, true, op->y);       /* RSC */                                                    \
    case 0x8: DP_LOGIC(a & op2, false);                 /* TST */                                                    \
    case 0x9: DP_LOGIC(a ^ op2, false);                 /* TEQ */                                                    \
    case 0xA: DP_ARITH(a, ~op2, 1, false, true);        /* CMP */                                                    \
    case 0xB: DP_ARITH(a, op2, 0, false, true);         /* CMN */                                                    \
    case 0xC: DP_LOGIC(a | op2, true);                  /* ORR */                                                    \
    case 0xD: DP_LOGIC(op2, true);                      /* MOV */                                                    \
    case 0xE: DP_LOGIC(a & ~op2, true);                 /* BIC */                                                    \
    default: DP_LOGIC(~op2, true);                      /* MVN */                                                    \
    }
/* The immediate form (x: the rotation was not 0, then d is the carry out) and the register form shifted by an
 * immediate (d: shift type, x: amount). */
#define DPI(OPC)                                                                                                     \
    L_DPI##OPC : a = R[op->b];                                                                                       \
    op2 = op->imm;                                                                                                   \
    sc = op->x ? op->d : fC;                                                                                         \
    DP_BODY(OPC)
#define DPR(OPC)                                                                                                     \
    L_DPR##OPC : a = R[op->b];                                                                                       \
    op2 = l_shift_imm(op->d, R[op->c], op->x, fC, &sc);                                                              \
    DP_BODY(OPC)

static uint32_t blk_run_arm(gba_machine *m, const gba_block *b, uint32_t lim)
{
#define DPL(f, n) [AK_##f + n] = &&L_##f##n
    static const void *const lab[AK_COUNT] = {
        [AK_GEN] = &&L_GEN,
        DPL(DPI, 0), DPL(DPI, 1), DPL(DPI, 2), DPL(DPI, 3), DPL(DPI, 4), DPL(DPI, 5), DPL(DPI, 6), DPL(DPI, 7),
        DPL(DPI, 8), DPL(DPI, 9), DPL(DPI, 10), DPL(DPI, 11), DPL(DPI, 12), DPL(DPI, 13), DPL(DPI, 14), DPL(DPI, 15),
        DPL(DPR, 0), DPL(DPR, 1), DPL(DPR, 2), DPL(DPR, 3), DPL(DPR, 4), DPL(DPR, 5), DPL(DPR, 6), DPL(DPR, 7),
        DPL(DPR, 8), DPL(DPR, 9), DPL(DPR, 10), DPL(DPR, 11), DPL(DPR, 12), DPL(DPR, 13), DPL(DPR, 14), DPL(DPR, 15),
        [AK_LDR] = &&L_LDR, [AK_STR] = &&L_STR, [AK_LDRB] = &&L_LDRB, [AK_STRB] = &&L_STRB,
        [AK_B] = &&L_B, [AK_BL] = &&L_BL, [AK_BX] = &&L_BX,
        [AK_LDM] = &&L_LDM, [AK_STM] = &&L_STM, [AK_LDRH] = &&L_LDRH, [AK_STRH] = &&L_STRH, [AK_LDRSB] = &&L_LDRSB,
        [AK_LDRSH] = &&L_LDRSH, [AK_MRS] = &&L_MRS,
    };
#undef DPL
    BLK_PROLOGUE(false);
    uint32_t a = 0, op2 = 0, sc = 0, addr, base, moved;

top:
    if (__builtin_expect(now >= ne, 0))
        goto out;
    now += pf ? rc_pf_fetch(h, now, pc + 8, false, ncost) : fc;
    latched = false;
    if (op->cond != 0xE && !L_COND(op->cond))
        BLK_NEXT();
    goto *lab[op->k];

L_GEN:
    BLK_GENERIC(exec_arm);
    DPI(0) DPI(1) DPI(2) DPI(3) DPI(4) DPI(5) DPI(6) DPI(7)
    DPI(8) DPI(9) DPI(10) DPI(11) DPI(12) DPI(13) DPI(14) DPI(15)
    DPR(0) DPR(1) DPR(2) DPR(3) DPR(4) DPR(5) DPR(6) DPR(7)
    DPR(8) DPR(9) DPR(10) DPR(11) DPR(12) DPR(13) DPR(14) DPR(15)

/* Single transfers: x = P | U << 1 | W << 2 | register offset << 3 (shift type d, amount y); rn may be PC (without
 * write-back). */
#define ST_ADDR()                                                                                                    \
    do {                                                                                                             \
        uint32_t off_;                                                                                               \
        if (op->x & 8) {                                                                                             \
            uint32_t dummy_;                                                                                         \
            off_ = l_shift_imm(op->d, R[op->c], op->y, fC, &dummy_);                                                 \
        } else {                                                                                                     \
            off_ = op->imm;                                                                                          \
        }                                                                                                            \
        base = op->b == 15 ? pc + 8 : R[op->b];                                                                      \
        moved = (op->x & 2) ? base + off_ : base - off_;                                                             \
        addr = (op->x & 1) ? moved : base;                                                                           \
    } while (0)
#define ST_WB() ((op->x & 5) != 1) /* !P || W */
L_LDR:
    ST_ADDR();
    if (RAM_R(addr, 0)) {
        now += WAIT32(addr, 0) + 1 + nx;
        a = ror32(rd32(ram_ptr(m, addr & ~3u)), (addr & 3) * 8);
        if (ST_WB())
            R[op->b] = moved;
        R[op->a] = a;
        BLK_NEXT();
    }
    BLK_BUS();
    a = load32_rot(m, addr, false);
    if (ST_WB())
        R[op->b] = moved;
    R[op->a] = a;
    BLK_BUS_END(1);
    BLK_NEXT();
L_LDRB:
    ST_ADDR();
    if (RAM_R(addr, 0)) {
        now += WAIT16(addr, 0) + 1 + nx;
        a = *ram_ptr(m, addr);
        if (ST_WB())
            R[op->b] = moved;
        R[op->a] = a;
        BLK_NEXT();
    }
    BLK_BUS();
    a = gba_read8(m, addr, false);
    if (ST_WB())
        R[op->b] = moved;
    R[op->a] = a;
    BLK_BUS_END(1);
    BLK_NEXT();
L_STR:
    ST_ADDR();
    if (RAM_W(addr & ~3u, 2, 0)) {
        now += WAIT32(addr, 0) + nx;
        wr32(ram_ptr(m, addr & ~3u), R[op->a]);
        if (ST_WB())
            R[op->b] = moved;
        BLK_NEXT();
    }
    BLK_BUS();
    gba_write32(m, addr, R[op->a], false);
    if (ST_WB())
        R[op->b] = moved;
    BLK_BUS_END(0);
    BLK_NEXT();
L_STRB:
    ST_ADDR();
    if (RAM_W(addr, 1, 0)) {
        now += WAIT16(addr, 0) + nx;
        *ram_ptr(m, addr) = (uint8_t)R[op->a];
        if (ST_WB())
            R[op->b] = moved;
        BLK_NEXT();
    }
    BLK_BUS();
    gba_write8(m, addr, (uint8_t)R[op->a], false);
    if (ST_WB())
        R[op->b] = moved;
    BLK_BUS_END(0);
    BLK_NEXT();
/* Halfword and signed transfers: x = P | U << 1 | W << 2 | immediate offset << 3. */
#define HT_ADDR()                                                                                                    \
    do {                                                                                                             \
        uint32_t off_ = (op->x & 8) ? op->imm : R[op->c];                                                            \
        base = op->b == 15 ? pc + 8 : R[op->b];                                                                      \
        moved = (op->x & 2) ? base + off_ : base - off_;                                                             \
        addr = (op->x & 1) ? moved : base;                                                                           \
    } while (0)
L_LDRH:
    HT_ADDR();
    if (RAM_R(addr, 0)) {
        now += WAIT16(addr, 0) + 1 + nx;
        a = ror32(rd16(ram_ptr(m, addr & ~1u)), (addr & 1) * 8);
        goto ht_load_out;
    }
    BLK_BUS();
    a = ror32(gba_read16(m, addr, false), (addr & 1) * 8);
    goto ht_load_bus;
L_LDRSB:
    HT_ADDR();
    goto ht_ldrsb;
L_LDRSH:
    HT_ADDR();
    if (addr & 1)
        goto ht_ldrsb; /* misaligned: LDRSB */
    if (RAM_R(addr, 0)) {
        now += WAIT16(addr, 0) + 1 + nx;
        a = (uint32_t)(int32_t)(int16_t)rd16(ram_ptr(m, addr));
        goto ht_load_out;
    }
    BLK_BUS();
    a = (uint32_t)(int32_t)(int16_t)gba_read16(m, addr, false);
    goto ht_load_bus;
ht_ldrsb:
    if (RAM_R(addr, 0)) {
        now += WAIT16(addr, 0) + 1 + nx;
        a = (uint32_t)(int32_t)(int8_t)*ram_ptr(m, addr);
        goto ht_load_out;
    }
    BLK_BUS();
    a = (uint32_t)(int32_t)(int8_t)gba_read8(m, addr, false);
ht_load_bus: /* the registers are written before the block may stop */
    if (ST_WB())
        R[op->b] = moved;
    R[op->a] = a;
    BLK_BUS_END(1);
    BLK_NEXT();
ht_load_out:
    if (ST_WB())
        R[op->b] = moved;
    R[op->a] = a;
    BLK_NEXT();
L_STRH:
    HT_ADDR();
    if (RAM_W(addr & ~1u, 1, 0)) {
        now += WAIT16(addr, 0) + nx;
        wr16(ram_ptr(m, addr & ~1u), (uint16_t)R[op->a]);
        if (ST_WB())
            R[op->b] = moved;
        BLK_NEXT();
    }
    BLK_BUS();
    gba_write16(m, addr, (uint16_t)R[op->a], false);
    if (ST_WB())
        R[op->b] = moved;
    BLK_BUS_END(0);
    BLK_NEXT();
#undef HT_ADDR
#undef ST_ADDR
#undef ST_WB
/* LDM / STM without PC or the S bit: x = P | U << 1 | W << 2, y: registers. */
L_LDM: {
    uint32_t list = op->imm, rn = op->b, size = 4 * (uint32_t)op->y, v = 0;
    base = R[rn];
    addr = (op->x & 2) ? base + ((op->x & 1) ? 4 : 0) : base - size + ((op->x & 1) ? 0 : 4);
    if ((op->x & 4) && !(list & (1u << rn)))
        R[rn] = (op->x & 2) ? base + size : base - size;
    bool seq = false;
    for (; list; list &= list - 1) {
        MT_RD(addr & ~3u, seq, v);
        R[__builtin_ctz(list)] = v;
        seq = MT_SEQ(addr);
        addr += 4;
    }
    MT_END(1);
    BLK_NEXT();
}
L_STM: {
    uint32_t list = op->imm, rn = op->b, size = 4 * (uint32_t)op->y, first = (uint32_t)__builtin_ctz(list);
    base = R[rn];
    addr = (op->x & 2) ? base + ((op->x & 1) ? 4 : 0) : base - size + ((op->x & 1) ? 0 : 4);
    uint32_t nb = (op->x & 2) ? base + size : base - size;
    bool w = (op->x & 4) != 0, seq = false;
    for (; list; list &= list - 1) {
        uint32_t i = (uint32_t)__builtin_ctz(list);
        MT_WR(addr & ~3u, (i == rn && w && i != first) ? nb : R[i], seq);
        seq = MT_SEQ(addr);
        addr += 4;
    }
    if (w)
        R[rn] = nb;
    MT_END(0);
    BLK_NEXT();
}
L_MRS: {
    uint32_t cpsr = fN << 31 | fZ << 30 | fC << 29 | fV << 28 | m->cpu.cpsr_ctl;
    R[op->a] = (op->x && has_spsr(m)) ? m->cpu.spsr[bank_of(m->cpu.cpsr_ctl & 0x1F)] : cpsr;
    BLK_NEXT();
}
L_B:
    R[15] = (op->imm & ~3u) + 8;
    BLK_BRANCH();
L_BL:
    R[14] = pc + 4;
    R[15] = (op->imm & ~3u) + 8;
    BLK_BRANCH();
L_BX:
    branch_exchange(m, R[op->c]);
    BLK_BRANCH();

branch_taken: {
    now += refill_cost(m, now, R[15], is_thumb(m));
    if (now < ne && !lim && is_thumb(m) == THUMB) {
        uint32_t npc_ = R[15] - PIPE;
        if (npc_ == cur->key - THUMB && m->blk_attn == attn0) { /* a loop back to this block's start */
            op = cur->ops;
            pc = npc_;
            goto top;
        }
        const gba_block *nb_ = blk_get(m, npc_, THUMB, BLK_MAX_OPS);
        if (nb_ && nb_->region == region && ((npc_ >> 24) & 15) == rg && m->blk_attn == attn0) {
            cur = nb_;
            op = nb_->ops;
            end = op + nb_->n;
            pc = npc_;
            goto top;
        }
    }
    if (!latched)
        blk_latch(m, pc, THUMB, region);
    m->cpu.branched = true;
    m->now = now;
    FLAGS_OUT();
    return (uint32_t)(op - cur->ops) + 1;
}
    BLK_OUT();
}

#undef THUMB
#undef W
#undef PIPE
#undef DP_LOGIC
#undef DP_ARITH
#undef DP_BODY
#undef DPI
#undef DPR

/* Runs from the block at PC (the caller made the run loop's checks: below the limit, not halted, no IRQ to take);
 * false when PC is not in a region blocks run from. */
static inline bool blk_step(gba_machine *m)
{
    bool thumb = (m->cpu.cpsr_ctl & CPSR_T) != 0;
    uint32_t pc = R[15] - (thumb ? 4u : 8u);
    const gba_block *b = blk_get(m, pc, thumb, BLK_MAX_OPS);
    if (!b)
        return false;
    if (thumb)
        blk_run_thumb(m, b, 0);
    else
        blk_run_arm(m, b, 0);
    return true;
}

static inline bool irq_due(const gba_machine *m)
{
    return m->irq_line && !(m->cpu.cpsr_ctl & CPSR_I);
}

/* The instructions lifted code in the cartridge hands over run from the predecoded blocks (a step() each was most of
 * the recompiler's time outside lifted code): the block at pc, cut to the instructions the loop may run in sequence
 * (*left), without going on after a taken branch. Cartridge blocks need no check against memory (lifted code's RAM
 * stores, m->rc_ran, concern RAM blocks only); IWRAM code is stepped (its blocks would be checked against IWRAM after
 * every lifted call). It stops where the loop would
 * have stopped stepping, or earlier (the block's end, a change the run loop checks): the loop checks then and hands
 * the rest over again, as after its own stops. The clock runs to next_event as the loop's (not gba_run_cycles' cap). */
uint32_t gba_cpu_step_handed(gba_machine *m, uint32_t *left, bool *chained, uint32_t iw_lo, uint32_t iw_hi)
{
    bool thumb = (m->cpu.cpsr_ctl & CPSR_T) != 0;
    uint32_t w = thumb ? 2u : 4u, pc = R[15] - 2 * w, lim = *left ? *left : 1;
    if (m->interp_ref || pc - 0x08000000u >= 0x06000000u)
        return handed_steps(m, left, chained, iw_lo, iw_hi);
    const gba_block *b = blk_get(m, pc, thumb, lim < BLK_MAX_OPS ? lim : BLK_MAX_OPS);
    if (!b)
        return handed_steps(m, left, chained, iw_lo, iw_hi);
    uint64_t cap = m->cpu_cap;
    m->cpu_cap = UINT64_MAX;
    uint32_t n = thumb ? blk_run_thumb(m, b, lim) : blk_run_arm(m, b, lim);
    m->cpu_cap = cap;
    if (!n)
        return handed_steps(m, left, chained, iw_lo, iw_hi);
    bool nt = (m->cpu.cpsr_ctl & CPSR_T) != 0;
    if (n >= *left || nt != thumb || R[15] - (nt ? 4u : 8u) != pc + n * w) {
        *chained = false;
        return n;
    }
    *left -= n;
    *chained = true;
    return n;
}

/* gba_cpu_step_outside_rom goes on in front of pc (was: the pc of the instruction before): outside the cartridge,
 * and, with no IRQ due, not at IWRAM code the recompiler's loop wants to see (see gba_internal.h). */
static inline bool outside_go_on(gba_machine *m, uint32_t was, uint32_t iw_lo, uint32_t iw_hi)
{
    uint32_t pc = R[15] - ((m->cpu.cpsr_ctl & CPSR_T) ? 4u : 8u);
    if (irq_due(m))
        return true;
    if (pc - 0x08000000u < 0x06000000u)
        return false;
    return (pc >> 24) != 3 || ((was >> 24) == 3 && pc - iw_lo >= iw_hi - iw_lo);
}

void gba_cpu_step_outside_rom(gba_machine *m, uint32_t iw_lo, uint32_t iw_hi)
{
    uint32_t was;
    if (m->interp_ref) {
        do {
            was = R[15] - ((m->cpu.cpsr_ctl & CPSR_T) ? 4u : 8u);
            step(m);
        } while (m->now < m->next_event && !m->cpu.halted && !m->cpu.stopped && outside_go_on(m, was, iw_lo, iw_hi));
        return;
    }
    if (m->rc_ran) {
        m->rc_ran = false;
        if (m->blocks)
            m->blocks->epoch++;
    }
    do {
        uint32_t pc = was = R[15] - ((m->cpu.cpsr_ctl & CPSR_T) ? 4u : 8u);
        if (irq_due(m) || pc - 0x08000000u < 0x06000000u || !blk_step(m))
            gba_cpu_step(m);
    } while (m->now < m->next_event && !m->cpu.halted && !m->cpu.stopped && outside_go_on(m, was, iw_lo, iw_hi));
}

void gba_cpu_run(gba_machine *m)
{
    if (m->rc) {
        gba_recomp_run(m);
        return;
    }
    if (m->interp_ref) {
        while (m->now < cpu_limit(m)) {
            if (m->cpu.halted || m->cpu.stopped) {
                m->now = m->next_event;
                return;
            }
            step(m);
        }
        return;
    }
    while (m->now < cpu_limit(m)) {
        if (m->cpu.halted || m->cpu.stopped) {
            m->now = m->next_event;
            return;
        }
        if (irq_due(m) || !blk_step(m))
            gba_cpu_step(m);
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
