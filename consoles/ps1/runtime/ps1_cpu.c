/* R3000A interpreter (P-4, XBA-162): all MIPS I instructions, branch delay slots, load delay slots with their
 * cancellation rules, unaligned LWL / LWR / SWL / SWR, COP0 (SR, CAUSE, EPC, BadVaddr, RFE, cache isolation),
 * exceptions and interrupts, COP2 transfers to the GTE.
 *
 * Load delay model: a load's value lands after the next instruction ran, so that instruction still reads the old
 * register. A register written by that next instruction (a plain write) cancels the pending load; a second load to
 * the same register cancels the first one (only the newest lands). LWL / LWR see the pending value of their target
 * (forwarding), so back-to-back LWR / LWL pairs merge. MFC0 / MFC2 / CFC2 are delayed like loads.
 *
 * Timing: a fixed cost per instruction by fetch region (no instruction cache model) plus a cost per data access by
 * target (no data cache on the R3000A: main RAM loads stall). Tuned so the BIOS and homebrew run at roughly the
 * console's pace; the timers and video are cycle-driven from the same clock. */
#include "ps1_hle.h"
#include "ps1_internal.h"

#include <stdlib.h>
#include <string.h>

/* Cycles per instruction by fetch region, and extra cycles per data load / store. */
#define CPI_RAM 1          /* KUSEG / KSEG0 main RAM (cached: instruction cache hits mostly) */
#define CPI_RAM_UNCACHED 5 /* KSEG1 main RAM */
#define CPI_BIOS 2         /* KSEG0 BIOS ROM */
#define CPI_BIOS_UNCACHED 12
#define LOAD_RAM 3
#define LOAD_BIOS 12
#define LOAD_IO 2

static inline uint32_t rd32le(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

void ps1_cpu_reset(ps1_machine *m)
{
    memset(m->r, 0, sizeof m->r);
    m->hi = m->lo = 0;
    m->pc = 0xBFC00000u;
    m->next_pc = m->pc + 4;
    m->cur_pc = m->pc;
    m->ld_reg = m->ld_val = m->pend_reg = m->pend_val = 0;
    m->in_delay = m->next_delay = false;
    m->branch_taken = m->next_taken = false;
    memset(m->cop0, 0, sizeof m->cop0);
    m->cop0[C0_SR] = SR_BEV;
    m->cop0[C0_PRID] = 0x00000002u;
    m->hook_at = 0;
    ps1_cpu_update_irq(m);
}

void ps1_cpu_update_irq(ps1_machine *m)
{
    if (m->i_stat & m->i_mask)
        m->cop0[C0_CAUSE] |= 0x400u;
    else
        m->cop0[C0_CAUSE] &= ~0x400u;
    uint32_t sr = m->cop0[C0_SR];
    m->irq_pending = (sr & SR_IEC) && (sr & m->cop0[C0_CAUSE] & 0xFF00u);
}

/* Enters the exception handler. The running instruction (cur_pc) is the one that faulted; for an interrupt taken
 * before an instruction, the caller sets cur_pc / in_delay to that instruction first. */
void ps1_cpu_exception(ps1_machine *m, uint32_t code, uint32_t cop)
{
    /* The load issued before the faulting instruction still lands. */
    if (m->pend_reg)
        m->r[m->pend_reg] = m->pend_val;
    m->pend_reg = 0;
    m->ld_reg = 0;
    uint32_t sr = m->cop0[C0_SR];
    m->cop0[C0_SR] = (sr & ~0x3Fu) | ((sr << 2) & 0x3Fu);
    uint32_t cause = m->cop0[C0_CAUSE] & ~(0xB000007Cu | 0x40000000u);
    cause |= code << 2;
    if (code == EXC_CPU)
        cause |= (cop & 3) << 28;
    uint32_t epc = m->cur_pc;
    if (m->in_delay) {
        epc -= 4;
        cause |= 0x80000000u;
        if (m->branch_taken)
            cause |= 0x40000000u;
        m->cop0[C0_JUMPDEST] = m->pc; /* the branch target */
    }
    m->cop0[C0_CAUSE] = cause;
    m->cop0[C0_EPC] = epc;
    m->pc = (sr & SR_BEV) ? 0xBFC00180u : 0x80000080u;
    m->next_pc = m->pc + 4;
    m->next_delay = false;
    m->next_taken = false;
    m->r[0] = 0;
    ps1_cpu_update_irq(m);
}

static void address_error(ps1_machine *m, uint32_t code, uint32_t addr)
{
    m->cop0[C0_BADVADDR] = addr;
    ps1_cpu_exception(m, code, 0);
}

/* ---- Register writes with the load delay rules ---- */
static inline void wr(ps1_machine *m, uint32_t rd, uint32_t v)
{
    m->r[rd] = v;
    if (rd == m->pend_reg)
        m->pend_reg = 0;
}

static inline void wr_delayed(ps1_machine *m, uint32_t rt, uint32_t v)
{
    if (rt == 0)
        return;
    if (rt == m->pend_reg)
        m->pend_reg = 0;
    m->ld_reg = rt;
    m->ld_val = v;
}

/* ---- Hooks: BIOS console output, fast boot ---- */
static void tty_putc(ps1_machine *m, uint32_t c)
{
    if (m->tty)
        m->tty(m->tty_ctx, (char)c);
}

static void tty_puts(ps1_machine *m, uint32_t addr)
{
    for (int i = 0; i < 4096; i++) {
        uint8_t c = ps1_peek8(m, addr + (uint32_t)i);
        if (!c)
            break;
        tty_putc(m, c);
    }
}

static void hook(ps1_machine *m)
{
    uint32_t pc = m->pc & 0x1FFFFFFFu;
    uint32_t fn = m->r[9];
    m->hook_at = 0;
    if (m->hle) /* the HLE BIOS's tables do the console output, and there is no shell */
        return;
    if (pc == 0xA0) {
        if (fn == 0x3C)
            tty_putc(m, m->r[4]);
        else if (fn == 0x3E)
            tty_puts(m, m->r[4]);
    } else if (pc == 0xB0) {
        if (fn == 0x3D)
            tty_putc(m, m->r[4]);
        else if (fn == 0x3F)
            tty_puts(m, m->r[4]);
    } else if (pc == 0x00030000u && m->exe && !m->exe_started) {
        ps1_boot_exe(m);
    } else if (pc == 0x00030000u && !m->exe && m->skip_shell && !m->shell_skipped) {
        /* fast boot of a disc (XBA-168): the shell returns at once, as when it finds a disc, and the BIOS goes on
         * with its own boot (SYSTEM.CNF's settings, the boot EXE read from the disc) */
        m->shell_skipped = true;
        m->r[2] = 0;
        m->pc = m->r[31];
        m->next_pc = m->r[31] + 4;
        m->next_delay = false;
    }
}

static inline void jump(ps1_machine *m, uint32_t target)
{
    m->next_pc = target;
    m->next_taken = true;
    uint32_t p = target & 0x1FFFFFFFu;
    if (p == 0xA0 || p == 0xB0 || p == 0x00030000u)
        m->hook_at = target;
}

/* ---- Memory access fast paths ---- */
static inline uint32_t phys_of(uint32_t addr)
{
    return (addr >> 29) == 7 ? addr : addr & 0x1FFFFFFFu; /* KUSEG / KSEG0 / KSEG1 mirrors; KSEG2 as is */
}

static inline int load32(ps1_machine *m, uint32_t addr, uint32_t *v)
{
    uint32_t p = phys_of(addr);
    if (p < 0x00800000u && (addr >> 29) != 1 && (addr >> 29) != 2 && (addr >> 29) != 3) {
        *v = rd32le(m->ram + (p & 0x1FFFFCu));
        m->cycles += LOAD_RAM;
        return 0;
    }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) {
        *v = rd32le(m->scratch + (p & 0x3FCu));
        return 0;
    }
    if ((p & 0xFFF80000u) == 0x1FC00000u)
        m->cycles += LOAD_BIOS;
    else
        m->cycles += LOAD_IO;
    return ps1_read32(m, addr, v);
}

/* Main RAM as the bus decodes it (ps1_bus.c decode: REG_RAM): KUSEG, or KSEG0 / KSEG1 in kernel mode, below 8 MB
 * (2 MB mirrored). The interpreter's byte / halfword / word accesses there skip the bus call; everything else (and
 * stores with the cache isolated) still goes through ps1_read* / ps1_write*. */
static inline bool ram_addr(const ps1_machine *m, uint32_t addr)
{
    uint32_t segs = (m->cop0[C0_SR] & SR_KUC) ? 0x01u : 0x31u; /* bit n: segment n reaches main RAM */
    return ((segs >> (addr >> 29)) & 1) && (addr & 0x1F800000u) == 0;
}

static inline int read8(ps1_machine *m, uint32_t addr, uint32_t *v)
{
    if (__builtin_expect(ram_addr(m, addr), 1)) {
        *v = m->ram[addr & 0x1FFFFFu];
        return 0;
    }
    return ps1_read8(m, addr, v);
}

static inline int read16(ps1_machine *m, uint32_t addr, uint32_t *v) /* addr is even */
{
    if (__builtin_expect(ram_addr(m, addr), 1)) {
        const uint8_t *q = m->ram + (addr & 0x1FFFFEu);
        *v = (uint32_t)q[0] | (uint32_t)q[1] << 8;
        return 0;
    }
    return ps1_read16(m, addr, v);
}

static inline int write_ram_ok(const ps1_machine *m, uint32_t addr)
{
    return !(m->cop0[C0_SR] & SR_ISC) && ram_addr(m, addr);
}

static inline int write8(ps1_machine *m, uint32_t addr, uint32_t v)
{
    if (__builtin_expect(write_ram_ok(m, addr), 1)) {
        PS1_RAM_TOUCH(m, addr);
        m->ram[addr & 0x1FFFFFu] = (uint8_t)v;
        return 0;
    }
    return ps1_write8(m, addr, v);
}

static inline int write16(ps1_machine *m, uint32_t addr, uint32_t v) /* addr is even */
{
    if (__builtin_expect(write_ram_ok(m, addr), 1)) {
        PS1_RAM_TOUCH(m, addr);
        uint8_t *q = m->ram + (addr & 0x1FFFFEu);
        q[0] = (uint8_t)v;
        q[1] = (uint8_t)(v >> 8);
        return 0;
    }
    return ps1_write16(m, addr, v);
}

static inline int write32(ps1_machine *m, uint32_t addr, uint32_t v) /* addr is a multiple of 4 */
{
    if (__builtin_expect(write_ram_ok(m, addr), 1)) {
        PS1_RAM_TOUCH(m, addr);
        v = rd32le((const uint8_t *)&v); /* to little-endian on a big-endian host (rd32le swaps there) */
        memcpy(m->ram + (addr & 0x1FFFFCu), &v, 4);
        return 0;
    }
    return ps1_write32(m, addr, v);
}

/* ---- COP0 ---- */
static int mfc0(ps1_machine *m, uint32_t rd, uint32_t *v)
{
    switch (rd) {
    case C0_BPC: case C0_BDA: case C0_JUMPDEST: case C0_DCIC: case C0_BADVADDR: case C0_BDAM: case C0_BPCM:
    case C0_SR: case C0_CAUSE: case C0_EPC: case C0_PRID:
        *v = m->cop0[rd];
        return 0;
    default:
        if (rd >= 16)
            return -1; /* reserved instruction */
        *v = 0;
        return 0;
    }
}

static void mtc0(ps1_machine *m, uint32_t rd, uint32_t v)
{
    switch (rd) {
    case C0_BPC: case C0_BDA: case C0_DCIC: case C0_BDAM: case C0_BPCM:
        m->cop0[rd] = v;
        break;
    case C0_SR:
        m->cop0[C0_SR] = v;
        ps1_cpu_update_irq(m);
        break;
    case C0_CAUSE: /* only the software interrupt bits */
        m->cop0[C0_CAUSE] = (m->cop0[C0_CAUSE] & ~0x300u) | (v & 0x300u);
        ps1_cpu_update_irq(m);
        break;
    default:
        break;
    }
}

static inline bool cop_usable(ps1_machine *m, int n)
{
    uint32_t sr = m->cop0[C0_SR];
    if (n == 0)
        return !(sr & SR_KUC) || (sr & SR_CU0);
    return (sr >> (28 + n)) & 1;
}

/* An interrupt is taken before the instruction at pc. A GTE command there is run first: the BIOS exception handler
 * skips a COP2 command at EPC (on hardware it already executed), so it must not be lost. */
static void take_interrupt(ps1_machine *m)
{
    m->cur_pc = m->pc;
    m->in_delay = m->next_delay;
    m->branch_taken = m->next_taken;
    uint32_t p = phys_of(m->pc);
    if (!(m->pc & 3) && (p < 0x00800000u || (p & 0xFFF80000u) == 0x1FC00000u)) {
        uint32_t ins = p < 0x00800000u ? rd32le(m->ram + (p & 0x1FFFFCu)) : rd32le(m->bios + (p & 0x7FFFCu));
        if ((ins >> 25) == 0x25 && cop_usable(m, 2))
            m->cycles += (uint64_t)ps1_gte_command(&m->gte, ins & 0x1FFFFFFu);
    }
    /* The pending load lands; the exception entry handles it (pend_reg). */
    m->pend_reg = m->ld_reg;
    m->pend_val = m->ld_val;
    m->ld_reg = 0;
    m->cycles += 2;
    ps1_cpu_exception(m, EXC_INT, 0);
}

/* An instruction fetch outside the inline main RAM path (step): an address error, the BIOS ROM, the IO registers
 * that answer fetches, or a bus error. Returns 0 when it raised an exception. */
static __attribute__((noinline)) int fetch_slow(ps1_machine *m, uint32_t pc, uint32_t *ins)
{
    uint32_t seg = pc >> 29;
    uint32_t p = pc & 0x1FFFFFFFu;
    if ((pc & 3) || (seg >= 4 && (m->cop0[C0_SR] & SR_KUC))) {
        address_error(m, EXC_ADEL, pc);
        return 0;
    }
    if (p < 0x00800000u && (seg == 0 || seg >= 4)) {
        *ins = rd32le(m->ram + (p & 0x1FFFFCu));
        m->cycles += seg == 5 ? CPI_RAM_UNCACHED : CPI_RAM;
    } else if ((p & 0xFFF80000u) == 0x1FC00000u && (seg == 0 || seg >= 4)) {
        *ins = rd32le(m->bios + (p & 0x7FFFCu));
        m->cycles += seg == 5 ? CPI_BIOS_UNCACHED : CPI_BIOS;
    } else if ((seg == 0 || seg >= 4) && ((p >= 0x1F801080u && p < 0x1F801100u) || (p >= 0x1F801C00u && p < 0x1F802000u))) {
        /* The DMA and SPU registers answer instruction fetches (ps1-tests cpu/code-in-io); other IO, the scratchpad
         * and unmapped space raise a bus error. */
        *ins = ps1_io_read(m, p, 4);
        m->cycles += CPI_RAM_UNCACHED;
    } else {
        m->cycles += CPI_RAM_UNCACHED;
        ps1_cpu_exception(m, EXC_IBE, 0);
        return 0;
    }
    return 1;
}

/* ---- The interpreter ---- */
#define RS m->r[(ins >> 21) & 31]
#define RT m->r[(ins >> 16) & 31]
#define RT_N ((ins >> 16) & 31)
#define RD_N ((ins >> 11) & 31)
#define SHAMT ((ins >> 6) & 31)
#define IMM_S ((uint32_t)(int32_t)(int16_t)ins)
#define IMM_U (ins & 0xFFFFu)

/* Inlined into the run loop (ps1_cpu_run): a call per instruction (saving and restoring the callee-saved registers
 * the big switch needs) cost about a fifth of the interpreter's time. ps1_cpu_step keeps an out-of-line copy. */
static inline __attribute__((always_inline)) void step(ps1_machine *m)
{
    uint32_t pc = m->pc, ins;
    m->cur_pc = pc;
    m->in_delay = m->next_delay;
    m->branch_taken = m->next_taken;
    m->next_delay = false;
    m->next_taken = false;
    m->pend_reg = m->ld_reg;
    m->pend_val = m->ld_val;
    m->ld_reg = 0;

    /* Fetch: main RAM inline (aligned, from KUSEG, or KSEG0 / KSEG1 / KSEG2 in kernel mode), the rest in fetch_slow. */
    uint32_t seg = pc >> 29;
    uint32_t p = pc & 0x1FFFFFFFu;
    uint32_t segs = (m->cop0[C0_SR] & SR_KUC) ? 0x01u : 0xF1u; /* bit n: an instruction may come from segment n */
    if (__builtin_expect((pc & 0x1F800003u) == 0 && ((segs >> seg) & 1), 1)) {
        ins = rd32le(m->ram + (p & 0x1FFFFCu));
        m->cycles += seg == 5 ? CPI_RAM_UNCACHED : CPI_RAM;
    } else if (!fetch_slow(m, pc, &ins)) {
        return;
    }
    m->pc = m->next_pc;
    m->next_pc += 4;

    uint32_t v, addr;
    int e;
    switch (ins >> 26) {
    case 0x00: /* SPECIAL */
        switch (ins & 63) {
        case 0x00: wr(m, RD_N, RT << SHAMT); break;
        case 0x02: wr(m, RD_N, RT >> SHAMT); break;
        case 0x03: wr(m, RD_N, (uint32_t)((int32_t)RT >> SHAMT)); break;
        case 0x04: wr(m, RD_N, RT << (RS & 31)); break;
        case 0x06: wr(m, RD_N, RT >> (RS & 31)); break;
        case 0x07: wr(m, RD_N, (uint32_t)((int32_t)RT >> (RS & 31))); break;
        case 0x08: /* JR */
            m->next_delay = true;
            jump(m, RS);
            break;
        case 0x09: /* JALR */
            v = RS;
            wr(m, RD_N, m->pc + 4);
            m->next_delay = true;
            jump(m, v);
            break;
        case 0x0C: ps1_cpu_exception(m, EXC_SYS, 0); return;
        case 0x0D: ps1_cpu_exception(m, EXC_BP, 0); return;
        case 0x10: wr(m, RD_N, m->hi); break;
        case 0x11: m->hi = RS; break;
        case 0x12: wr(m, RD_N, m->lo); break;
        case 0x13: m->lo = RS; break;
        case 0x18: { /* MULT */
            int64_t r = (int64_t)(int32_t)RS * (int64_t)(int32_t)RT;
            m->lo = (uint32_t)r;
            m->hi = (uint32_t)((uint64_t)r >> 32);
            break;
        }
        case 0x19: { /* MULTU */
            uint64_t r = (uint64_t)RS * (uint64_t)RT;
            m->lo = (uint32_t)r;
            m->hi = (uint32_t)(r >> 32);
            break;
        }
        case 0x1A: { /* DIV */
            int32_t n = (int32_t)RS, d = (int32_t)RT;
            if (d == 0) {
                m->hi = (uint32_t)n;
                m->lo = n >= 0 ? 0xFFFFFFFFu : 1u;
            } else if ((uint32_t)n == 0x80000000u && d == -1) {
                m->hi = 0;
                m->lo = 0x80000000u;
            } else {
                m->lo = (uint32_t)(n / d);
                m->hi = (uint32_t)(n % d);
            }
            break;
        }
        case 0x1B: { /* DIVU */
            uint32_t n = RS, d = RT;
            if (d == 0) {
                m->hi = n;
                m->lo = 0xFFFFFFFFu;
            } else {
                m->lo = n / d;
                m->hi = n % d;
            }
            break;
        }
        case 0x20: { /* ADD */
            uint32_t a = RS, b = RT, r = a + b;
            if ((~(a ^ b) & (a ^ r)) & 0x80000000u) {
                ps1_cpu_exception(m, EXC_OV, 0);
                return;
            }
            wr(m, RD_N, r);
            break;
        }
        case 0x21: wr(m, RD_N, RS + RT); break;
        case 0x22: { /* SUB */
            uint32_t a = RS, b = RT, r = a - b;
            if (((a ^ b) & (a ^ r)) & 0x80000000u) {
                ps1_cpu_exception(m, EXC_OV, 0);
                return;
            }
            wr(m, RD_N, r);
            break;
        }
        case 0x23: wr(m, RD_N, RS - RT); break;
        case 0x24: wr(m, RD_N, RS & RT); break;
        case 0x25: wr(m, RD_N, RS | RT); break;
        case 0x26: wr(m, RD_N, RS ^ RT); break;
        case 0x27: wr(m, RD_N, ~(RS | RT)); break;
        case 0x2A: wr(m, RD_N, (int32_t)RS < (int32_t)RT); break;
        case 0x2B: wr(m, RD_N, RS < RT); break;
        default: ps1_cpu_exception(m, EXC_RI, 0); return;
        }
        break;
    case 0x01: { /* REGIMM: BLTZ / BGEZ / BLTZAL / BGEZAL; rt bits 4-1 = 1000 links (even when not taken) */
        uint32_t rt = RT_N;
        bool ge = rt & 1;
        bool taken = ge ? (int32_t)RS >= 0 : (int32_t)RS < 0;
        uint32_t target = m->pc + (IMM_S << 2);
        if ((rt & 0x1E) == 0x10)
            wr(m, 31, m->pc + 4);
        m->next_delay = true;
        if (taken)
            jump(m, target);
        break;
    }
    case 0x02: /* J */
        m->next_delay = true;
        jump(m, (m->pc & 0xF0000000u) | ((ins & 0x3FFFFFFu) << 2));
        break;
    case 0x03: /* JAL */
        wr(m, 31, m->pc + 4);
        m->next_delay = true;
        jump(m, (m->pc & 0xF0000000u) | ((ins & 0x3FFFFFFu) << 2));
        break;
    case 0x04: /* BEQ */
        m->next_delay = true;
        if (RS == RT)
            jump(m, m->pc + (IMM_S << 2));
        break;
    case 0x05: /* BNE */
        m->next_delay = true;
        if (RS != RT)
            jump(m, m->pc + (IMM_S << 2));
        break;
    case 0x06: /* BLEZ */
        m->next_delay = true;
        if ((int32_t)RS <= 0)
            jump(m, m->pc + (IMM_S << 2));
        break;
    case 0x07: /* BGTZ */
        m->next_delay = true;
        if ((int32_t)RS > 0)
            jump(m, m->pc + (IMM_S << 2));
        break;
    case 0x08: { /* ADDI */
        uint32_t a = RS, b = IMM_S, r = a + b;
        if ((~(a ^ b) & (a ^ r)) & 0x80000000u) {
            ps1_cpu_exception(m, EXC_OV, 0);
            return;
        }
        wr(m, RT_N, r);
        break;
    }
    case 0x09: wr(m, RT_N, RS + IMM_S); break;
    case 0x0A: wr(m, RT_N, (int32_t)RS < (int32_t)IMM_S); break;
    case 0x0B: wr(m, RT_N, RS < IMM_S); break;
    case 0x0C: wr(m, RT_N, RS & IMM_U); break;
    case 0x0D: wr(m, RT_N, RS | IMM_U); break;
    case 0x0E: wr(m, RT_N, RS ^ IMM_U); break;
    case 0x0F: wr(m, RT_N, IMM_U << 16); break;
    case 0x10: /* COP0 */
        if (!cop_usable(m, 0)) {
            ps1_cpu_exception(m, EXC_CPU, 0);
            return;
        }
        switch ((ins >> 21) & 31) {
        case 0x00: /* MFC0 */
            if (mfc0(m, RD_N, &v) < 0) {
                ps1_cpu_exception(m, EXC_RI, 0);
                return;
            }
            wr_delayed(m, RT_N, v);
            break;
        case 0x04: mtc0(m, RD_N, RT); break;
        case 0x10: /* CO: only RFE exists */
            if ((ins & 63) == 0x10) {
                uint32_t sr = m->cop0[C0_SR];
                m->cop0[C0_SR] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
                ps1_cpu_update_irq(m);
            }
            break;
        default: break; /* other COP0 encodings do nothing (no exception on the R3000A) */
        }
        break;
    case 0x11: case 0x13: /* COP1 / COP3: not fitted */
        if (!cop_usable(m, (int)((ins >> 26) & 3))) {
            ps1_cpu_exception(m, EXC_CPU, (ins >> 26) & 3);
            return;
        }
        break;
    case 0x12: /* COP2: the GTE */
        if (!cop_usable(m, 2)) {
            ps1_cpu_exception(m, EXC_CPU, 2);
            return;
        }
        if (ins & (1u << 25)) {
            m->cycles += (uint64_t)ps1_gte_command(&m->gte, ins & 0x1FFFFFFu);
            break;
        }
        switch ((ins >> 21) & 31) {
        case 0x00: wr_delayed(m, RT_N, ps1_gte_read_data(&m->gte, (int)RD_N)); break;
        case 0x02: wr_delayed(m, RT_N, ps1_gte_read_ctrl(&m->gte, (int)RD_N)); break;
        case 0x04: ps1_gte_write_data(&m->gte, (int)RD_N, RT); break;
        case 0x06: ps1_gte_write_ctrl(&m->gte, (int)RD_N, RT); break;
        default: ps1_cpu_exception(m, EXC_RI, 0); return;
        }
        break;
    case 0x20: /* LB */
        addr = RS + IMM_S;
        if ((e = read8(m, addr, &v)) != 0) {
            if (e == EXC_ADEL) address_error(m, EXC_ADEL, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        wr_delayed(m, RT_N, (uint32_t)(int32_t)(int8_t)v);
        break;
    case 0x21: /* LH */
        addr = RS + IMM_S;
        if (addr & 1) {
            address_error(m, EXC_ADEL, addr);
            return;
        }
        if ((e = read16(m, addr, &v)) != 0) {
            if (e == EXC_ADEL) address_error(m, EXC_ADEL, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        wr_delayed(m, RT_N, (uint32_t)(int32_t)(int16_t)v);
        break;
    case 0x22: case 0x26: { /* LWL / LWR */
        addr = RS + IMM_S;
        if ((e = load32(m, addr & ~3u, &v)) != 0) {
            if (e == EXC_ADEL) address_error(m, EXC_ADEL, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        uint32_t rt = RT_N;
        uint32_t cur = (rt && rt == m->pend_reg) ? m->pend_val : m->r[rt];
        uint32_t sh = addr & 3, r;
        if ((ins >> 26) == 0x22)
            r = sh == 3 ? v : (cur & (0x00FFFFFFu >> (sh * 8))) | (v << (24 - sh * 8));
        else
            r = sh == 0 ? v : (cur & (0xFFFFFF00u << (24 - sh * 8))) | (v >> (sh * 8));
        wr_delayed(m, rt, r);
        break;
    }
    case 0x23: /* LW */
        addr = RS + IMM_S;
        if (addr & 3) {
            address_error(m, EXC_ADEL, addr);
            return;
        }
        if ((e = load32(m, addr, &v)) != 0) {
            if (e == EXC_ADEL) address_error(m, EXC_ADEL, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        wr_delayed(m, RT_N, v);
        break;
    case 0x24: /* LBU */
        addr = RS + IMM_S;
        if ((e = read8(m, addr, &v)) != 0) {
            if (e == EXC_ADEL) address_error(m, EXC_ADEL, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        wr_delayed(m, RT_N, v & 0xFF);
        break;
    case 0x25: /* LHU */
        addr = RS + IMM_S;
        if (addr & 1) {
            address_error(m, EXC_ADEL, addr);
            return;
        }
        if ((e = read16(m, addr, &v)) != 0) {
            if (e == EXC_ADEL) address_error(m, EXC_ADEL, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        wr_delayed(m, RT_N, v & 0xFFFF);
        break;
    case 0x28: /* SB */
        addr = RS + IMM_S;
        if ((e = write8(m, addr, RT)) != 0) {
            if (e == EXC_ADES) address_error(m, EXC_ADES, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        break;
    case 0x29: /* SH */
        addr = RS + IMM_S;
        if (addr & 1) {
            address_error(m, EXC_ADES, addr);
            return;
        }
        if ((e = write16(m, addr, RT)) != 0) {
            if (e == EXC_ADES) address_error(m, EXC_ADES, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        break;
    case 0x2A: case 0x2E: { /* SWL / SWR */
        addr = RS + IMM_S;
        uint32_t a = addr & ~3u, sh = addr & 3, rt = RT, w, r;
        if (m->cop0[C0_SR] & SR_ISC)
            break;
        if ((e = load32(m, a, &w)) != 0) {
            if (e == EXC_ADEL) address_error(m, EXC_ADES, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        if ((ins >> 26) == 0x2A)
            r = sh == 3 ? rt : (w & (0xFFFFFF00u << (sh * 8))) | (rt >> (24 - sh * 8));
        else
            r = sh == 0 ? rt : (w & (0x00FFFFFFu >> (24 - sh * 8))) | (rt << (sh * 8));
        if ((e = write32(m, a, r)) != 0) {
            if (e == EXC_ADES) address_error(m, EXC_ADES, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        break;
    }
    case 0x2B: /* SW */
        addr = RS + IMM_S;
        if (addr & 3) {
            address_error(m, EXC_ADES, addr);
            return;
        }
        if ((e = write32(m, addr, RT)) != 0) {
            if (e == EXC_ADES) address_error(m, EXC_ADES, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        break;
    case 0x32: /* LWC2 */
        if (!cop_usable(m, 2)) {
            ps1_cpu_exception(m, EXC_CPU, 2);
            return;
        }
        addr = RS + IMM_S;
        if (addr & 3) {
            address_error(m, EXC_ADEL, addr);
            return;
        }
        if ((e = load32(m, addr, &v)) != 0) {
            if (e == EXC_ADEL) address_error(m, EXC_ADEL, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        ps1_gte_write_data(&m->gte, (int)RT_N, v);
        break;
    case 0x3A: /* SWC2 */
        if (!cop_usable(m, 2)) {
            ps1_cpu_exception(m, EXC_CPU, 2);
            return;
        }
        addr = RS + IMM_S;
        if (addr & 3) {
            address_error(m, EXC_ADES, addr);
            return;
        }
        if ((e = write32(m, addr, ps1_gte_read_data(&m->gte, (int)RT_N))) != 0) {
            if (e == EXC_ADES) address_error(m, EXC_ADES, addr); else ps1_cpu_exception(m, (uint32_t)e, 0);
            return;
        }
        break;
    case 0x30: case 0x31: case 0x33: case 0x38: case 0x39: case 0x3B: { /* LWC0/1/3, SWC0/1/3 */
        /* Gated by the CU bit itself (CU0 too, even in kernel mode); no such register file: nothing happens. */
        uint32_t n = (ins >> 26) & 3;
        if (!((m->cop0[C0_SR] >> (28 + n)) & 1)) {
            ps1_cpu_exception(m, EXC_CPU, n);
            return;
        }
        break;
    }
    default:
        if (m->hle && (ins & PS1_HLE_TRAP_MASK) == PS1_HLE_TRAP && p < 0x10000u) {
            /* the HLE BIOS's trap word, from kernel RAM (ps1_hle.h): the call runs as one instruction */
            if (m->pend_reg)
                m->r[m->pend_reg] = m->pend_val;
            m->pend_reg = 0;
            m->r[0] = 0;
            ps1_hle_trap(m, ins & 0xFFFFu);
            m->r[0] = 0;
            return;
        }
        ps1_cpu_exception(m, EXC_RI, 0);
        return;
    }
    /* The previous instruction's load lands (no pending load: pend_reg 0, and r0 is cleared just after). */
    m->r[m->pend_reg] = m->pend_val;
    m->r[0] = 0;
}

void ps1_cpu_step(ps1_machine *m) { step(m); }

/* ---- The block loop (XBA-204) ----
 * ps1_cpu_run (and the recompiler's unliftable loop) run predecoded blocks (below) with the CPU state in locals, and
 * step() for whatever a block doesn't hold. Between instructions where the run loop looks at the machine (cycles >=
 * next_event, an interrupt pending, pc at the hook address) every field is what step()'s own loop would leave, the
 * interpreter scratch fields (cur_pc, in_delay, branch_taken, pend_reg / pend_val) included: the machine, its save
 * states and the frames are byte-identical. ps1_cpu_set_reference keeps step()'s loop (ps1_run --lockstep-ref: the
 * oracle the tests compare the blocks with, instruction by instruction). */
static __attribute__((noinline)) void slow_step(ps1_machine *m) { step(m); }

static inline __attribute__((always_inline)) bool is_hook_target(uint32_t target)
{
    uint32_t p = target & 0x1FFFFFFFu;
    return p == 0xA0 || p == 0xB0 || p == 0x00030000u;
}

/* ---- Predecoded blocks (XBA-204) ----
 * A straight run of code in main RAM or the BIOS ROM (KUSEG / KSEG0 / KSEG1), decoded once into ops with their
 * operands extracted, branch targets computed, and the load delays settled where the code allows it statically:
 * a load whose next instruction neither reads its register nor loads into it lands at once (nothing can tell the
 * difference: the next instruction is in the same block, and if it falls back to step() the load is put back in
 * flight with the register's old value). A load followed by an instruction that would see the difference ends the
 * block with the load in flight, as do a branch (after its delay slot), a GTE command (its cost varies), the end of
 * the 1 KB RAM page (one page write counter covers the block), the size cap and any instruction the ops don't hold
 * (COP0, SYSCALL / BREAK, a write to r0, a branch in a delay slot...), which step() then runs. Loads and stores
 * outside main RAM and the scratchpad (IO, the BIOS ROM, the expansions) go through the bus with the clock where
 * step() has it, and the block stops after them (IO may raise an interrupt, move the next event, start a DMA).
 *
 * A block is entered where step() would run its instructions back to back: not in a delay slot (a load in flight in
 * front of it lands at once when its first op can't tell: conf0), kernel mode with the cache not isolated, every
 * check the run loop makes before an instruction known to pass (no interrupt pending, the hook address not inside,
 * in the unliftable loop not liftable code, and the clock below next_event: cum bounds the fetch and load cycles;
 * near the event only the ops known to start before it run, the block stops in front of the next). An op that would
 * raise an exception (overflow, address or bus error, coprocessor unusable) is found before it changes anything: the
 * state is rebuilt as step() holds it in front of that instruction and step() runs it. A store into the block's own
 * code stops the block after it. Blocks of main RAM are checked against the page
 * write counter (ram_gen, which every CPU store, DMA, EXE load, HLE write and save state load bumps) at entry: a
 * counter that moved means the instruction words are compared again, and the block decoded again if they differ. */
enum {
    K_NOP,
    /* d = s op t */
    K_ADDU, K_SUBU, K_AND, K_OR, K_XOR, K_NOR, K_SLT, K_SLTU, K_SLLV, K_SRLV, K_SRAV, K_ADD, K_SUB,
    /* d = t op imm (shift amount) */
    K_SLL, K_SRL, K_SRA,
    /* d = s op imm */
    K_ADDIU, K_SLTI, K_SLTIU, K_ANDI, K_ORI, K_XORI, K_LUI, K_ADDI,
    /* HI / LO */
    K_MFHI, K_MFLO, K_MTHI, K_MTLO, K_MULT, K_MULTU, K_DIV, K_DIVU,
    /* delayed writes to d (loads: address s + imm; MFC2 / CFC2: GTE register t) */
    K_LB, K_LBU, K_LH, K_LHU, K_LW, K_LWL, K_LWR, K_MFC2, K_CFC2,
    /* stores of t to s + imm */
    K_SB, K_SH, K_SW, K_SWL, K_SWR,
    /* GTE: MTC2 / CTC2 (t to GTE register d), LWC2 (s + imm to GTE d), SWC2 (GTE t to s + imm), a command (imm) */
    K_MTC2, K_CTC2, K_LWC2, K_SWC2, K_GTE,
    /* branches: imm = target, d = the link register (0: none); the block's last op but one */
    K_BEQ, K_BNE, K_BLEZ, K_BGTZ, K_BLTZ, K_BGEZ, K_J, K_JR,
    K_UNSUPPORTED,
};

#define K_IS_DELAYED(k) ((k) >= K_LB && (k) <= K_CFC2)
#define K_IS_BRANCH(k) ((k) >= K_BEQ && (k) <= K_JR)
#define K_LOAD32(k) ((k) == K_LW || (k) == K_LWL || (k) == K_LWR || (k) == K_SWL || (k) == K_SWR || (k) == K_LWC2)

typedef struct bop {
    uint8_t k, s, t, d;
    uint32_t imm;
} bop;

#define BLOCK_MAX 48
#define BLOCK_NOPAGE 0xFFFFFFFFu

typedef struct block {
    uint32_t pc;       /* its first instruction */
    uint32_t fall;     /* the address after its last instruction (the link value of its branch) */
    uint32_t page;     /* main RAM: the ram_gen page it lies in (BLOCK_NOPAGE: the BIOS ROM) */
    uint32_t gen;      /* that page's counter when the words were last checked */
    uint32_t ram_lo;   /* main RAM: its offset in the 2 MB (stores into [ram_lo, ram_lo + len4) stop it) */
    uint32_t len4;     /* 4 x n */
    uint32_t maxcost;  /* bound of the cycles its instructions take before the last one starts (cum[n - 1]) */
    uint32_t fcsum;    /* fetch cycles of all of it */
    uint16_t n;        /* ops (0: no block here, step() runs the instruction) */
    uint8_t fc;        /* fetch cycles per instruction */
    uint8_t branch;    /* it ends with a branch and its delay slot (2: one that may go to a hook address) */
    const uint32_t *words; /* the instruction words decoded (main RAM: compared when the page counter moved) */
    const uint16_t *cum;   /* cum[j]: bound of the cycles ops 0..j-1 take (fetch and load cycles) */
    uint32_t conf0;        /* the registers a load in flight in front of the block must not be: its first op reads
                            * them or loads into them */
    bop ops[];
} block;

#define BLOCKS_TAB 65536u
#define BLOCKS_ARENA (4u << 20)

struct ps1_blocks {
    block *tab[BLOCKS_TAB]; /* direct mapped by pc */
    uint8_t *arena;
    size_t used;
};

void ps1_cpu_free(ps1_machine *m)
{
    if (m->blocks) {
        free(m->blocks->arena);
        free(m->blocks);
        m->blocks = NULL;
    }
}

void ps1_cpu_flush_blocks(ps1_machine *m)
{
    if (m->blocks) {
        memset(m->blocks->tab, 0, sizeof m->blocks->tab);
        m->blocks->used = 0;
    }
}

/* The plain register write of an op (step()'s wr: cancels a load in flight to that register), 0 if none. */
static inline uint32_t op_wdest(const bop *o)
{
    if (o->k == K_NOP || K_IS_DELAYED(o->k) || (o->k >= K_MTHI && o->k <= K_DIVU) || (o->k >= K_SB && o->k <= K_GTE))
        return 0;
    return o->d; /* ALU destinations, branch links */
}

/* Decodes one instruction. Returns the op kind (K_UNSUPPORTED: step() runs it) and the registers it reads (bit mask)
 * in *reads. pc is the instruction's address (branch targets). */
static uint32_t decode_op(uint32_t ins, uint32_t pc, bop *o, uint32_t *reads)
{
    uint32_t rs = (ins >> 21) & 31, rt = (ins >> 16) & 31, rd = (ins >> 11) & 31, sh = (ins >> 6) & 31;
    uint32_t imm_s = (uint32_t)(int32_t)(int16_t)ins, imm_u = ins & 0xFFFFu;
    uint32_t k = K_UNSUPPORTED;
    o->s = (uint8_t)rs;
    o->t = (uint8_t)rt;
    o->d = (uint8_t)rd;
    o->imm = 0;
    *reads = (1u << rs) | (1u << rt);
    switch (ins >> 26) {
    case 0x00: {
        static const uint8_t special[64] = {
            [0x00] = K_SLL, [0x02] = K_SRL, [0x03] = K_SRA, [0x04] = K_SLLV, [0x06] = K_SRLV, [0x07] = K_SRAV,
            [0x08] = K_JR, [0x09] = K_JR, [0x10] = K_MFHI, [0x11] = K_MTHI, [0x12] = K_MFLO, [0x13] = K_MTLO,
            [0x18] = K_MULT, [0x19] = K_MULTU, [0x1A] = K_DIV, [0x1B] = K_DIVU, [0x20] = K_ADD, [0x21] = K_ADDU,
            [0x22] = K_SUB, [0x23] = K_SUBU, [0x24] = K_AND, [0x25] = K_OR, [0x26] = K_XOR, [0x27] = K_NOR,
            [0x2A] = K_SLT, [0x2B] = K_SLTU,
        };
        uint32_t f = ins & 63;
        k = special[f];
        if (k == K_NOP) /* not in the table (K_NOP is 0): SYSCALL, BREAK, reserved */
            return K_UNSUPPORTED;
        if (k == K_JR) {
            o->d = f == 0x09 ? (uint8_t)rd : 0;
            if (f == 0x09 && rd == 0)
                return K_UNSUPPORTED; /* JALR linking into r0 */
            break;
        }
        if (k == K_SLL || k == K_SRL || k == K_SRA)
            o->imm = sh;
        if (k >= K_MTHI && k <= K_DIVU)
            break;
        if (rd == 0)
            k = (k == K_ADD || k == K_SUB) ? K_UNSUPPORTED : K_NOP; /* r0 written (an overflow still traps) */
        break;
    }
    case 0x01: /* BLTZ / BGEZ / BLTZAL / BGEZAL */
        k = (rt & 1) ? K_BGEZ : K_BLTZ;
        o->d = (rt & 0x1E) == 0x10 ? 31 : 0;
        o->imm = pc + 4 + (imm_s << 2);
        *reads = 1u << rs;
        break;
    case 0x02: case 0x03:
        k = K_J;
        o->d = (ins >> 26) == 0x03 ? 31 : 0;
        o->imm = ((pc + 4) & 0xF0000000u) | ((ins & 0x3FFFFFFu) << 2);
        *reads = 0;
        break;
    case 0x04: case 0x05: case 0x06: case 0x07: {
        static const uint8_t br[4] = {K_BEQ, K_BNE, K_BLEZ, K_BGTZ};
        k = br[(ins >> 26) - 4];
        o->d = 0;
        o->imm = pc + 4 + (imm_s << 2);
        if (k == K_BLEZ || k == K_BGTZ)
            *reads = 1u << rs;
        break;
    }
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: {
        static const uint8_t alui[8] = {K_ADDI, K_ADDIU, K_SLTI, K_SLTIU, K_ANDI, K_ORI, K_XORI, K_LUI};
        k = alui[(ins >> 26) - 8];
        o->d = (uint8_t)rt;
        o->imm = k == K_LUI ? imm_u << 16 : (k >= K_ANDI && k <= K_XORI) ? imm_u : imm_s;
        *reads = 1u << rs;
        if (rt == 0)
            k = k == K_ADDI ? K_UNSUPPORTED : K_NOP;
        break;
    }
    case 0x12: /* COP2 */
        if (ins & (1u << 25)) {
            k = K_GTE;
            o->imm = ins & 0x1FFFFFFu;
            *reads = 0;
            break;
        }
        switch (rs) {
        case 0x00: case 0x02:
            k = rs ? K_CFC2 : K_MFC2;
            o->t = (uint8_t)rd; /* the GTE register */
            o->d = (uint8_t)rt;
            *reads = 0;
            if (rt == 0)
                k = K_UNSUPPORTED;
            break;
        case 0x04: case 0x06:
            k = rs == 4 ? K_MTC2 : K_CTC2;
            o->d = (uint8_t)rd;
            *reads = 1u << rt;
            break;
        default:
            return K_UNSUPPORTED;
        }
        break;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: {
        static const uint8_t ld[7] = {K_LB, K_LH, K_LWL, K_LW, K_LBU, K_LHU, K_LWR};
        k = ld[(ins >> 26) - 0x20];
        o->d = (uint8_t)rt;
        o->imm = imm_s;
        if (k != K_LWL && k != K_LWR)
            *reads = 1u << rs; /* LWL / LWR merge into rt: they read it */
        if (rt == 0)
            k = K_UNSUPPORTED;
        break;
    }
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E: {
        static const uint8_t st[7] = {K_SB, K_SH, K_SWL, K_SW, 0, 0, K_SWR};
        k = st[(ins >> 26) - 0x28];
        o->imm = imm_s;
        break;
    }
    case 0x32: /* LWC2 */
        k = K_LWC2;
        o->d = (uint8_t)rt;
        o->imm = imm_s;
        *reads = 1u << rs;
        break;
    case 0x3A: /* SWC2 */
        k = K_SWC2;
        o->imm = imm_s;
        *reads = 1u << rs;
        break;
    default:
        return K_UNSUPPORTED;
    }
    o->k = (uint8_t)k;
    return k;
}

/* Decodes the block at pc (the run loop checked: kernel mode). Returns it, n = 0 when step() must run the first
 * instruction. NULL when the arena is full (the caller flushes and retries). */
static __attribute__((noinline)) block *block_build(ps1_machine *m, struct ps1_blocks *bc, uint32_t pc)
{
    uint32_t seg = pc >> 29, p = pc & 0x1FFFFFFFu;
    const uint8_t *src = NULL;
    uint32_t limit = 0, fc = 0, page = BLOCK_NOPAGE, ram_lo = 0x80000000u;
    if (seg == 0 || seg == 4 || seg == 5) {
        if ((pc & 0x1F800003u) == 0) {
            src = m->ram + (p & 0x1FFFFCu);
            limit = (0x400u - (p & 0x3FCu)) / 4; /* to the end of the 1 KB page */
            fc = seg == 5 ? CPI_RAM_UNCACHED : CPI_RAM;
            page = (p & 0x1FFFFFu) >> PS1_RAM_PAGE_SHIFT;
            ram_lo = p & 0x1FFFFCu;
        } else if ((p & 0xFFF80003u) == 0x1FC00000u) {
            src = m->bios + (p & 0x7FFFCu);
            limit = (0x400u - (p & 0x3FCu)) / 4;
            fc = seg == 5 ? CPI_BIOS_UNCACHED : CPI_BIOS;
        }
    }
    if (limit > BLOCK_MAX)
        limit = BLOCK_MAX;
    size_t need = sizeof(block) + limit * (sizeof(bop) + 6) + 16;
    if (bc->used + need > BLOCKS_ARENA)
        return NULL;
    block *b = (block *)(void *)(bc->arena + bc->used);
    uint32_t n = 0, branch = 0;
    b->conf0 = 0;
    for (uint32_t i = 0; i < limit; i++) {
        uint32_t reads, ins = rd32le(src + 4 * i), ipc = pc + 4 * i;
        bop *o = &b->ops[i];
        uint32_t k = decode_op(ins, ipc, o, &reads);
        if (k == K_UNSUPPORTED)
            break;
        if (i == 0)
            b->conf0 = reads | (K_IS_DELAYED(k) ? 1u << o->d : 0);
        /* The load before: lands at once only if this instruction can't tell (else the block ends with it in flight). */
        if (i > 0 && K_IS_DELAYED(b->ops[i - 1].k)) {
            uint32_t L = b->ops[i - 1].d;
            if (((reads >> L) & 1) || (K_IS_DELAYED(k) && o->d == L))
                break;
        }
        if (K_IS_BRANCH(k)) {
            /* With its delay slot in the block: an op, not a branch, not past the page; the delay slot is not where
             * a jump to a hook address would stop the run loop. */
            bop ds;
            uint32_t dreads, dins;
            if (i + 1 >= limit || is_hook_target(ipc + 4))
                break;
            dins = rd32le(src + 4 * (i + 1));
            uint32_t dk = decode_op(dins, ipc + 4, &ds, &dreads);
            if (dk == K_UNSUPPORTED || K_IS_BRANCH(dk))
                break;
            b->ops[i + 1] = ds;
            n = i + 2;
            branch = k == K_JR || is_hook_target(o->imm) ? 2 : 1;
            break;
        }
        n = i + 1;
        if (k == K_GTE)
            break; /* its cost varies: the last op, after every check */
    }
    uint32_t *words = (uint32_t *)(void *)&b->ops[n];
    uint16_t *cum = (uint16_t *)(void *)(words + n);
    memcpy(words, src ? src : (const uint8_t *)words, (size_t)n * 4);
    for (uint32_t i = 0, c = 0; i < n; i++) {
        cum[i] = (uint16_t)c;
        c += fc + (K_LOAD32(b->ops[i].k) ? LOAD_RAM : 0);
    }
    b->pc = pc;
    b->fall = pc + 4 * n;
    b->page = page;
    b->gen = page != BLOCK_NOPAGE ? m->ram_gen[page] : 0;
    b->ram_lo = ram_lo;
    b->len4 = 4 * n;
    b->maxcost = n ? cum[n - 1] : 0;
    b->fcsum = fc * n;
    b->n = (uint16_t)n;
    b->fc = (uint8_t)fc;
    b->branch = (uint8_t)branch;
    b->words = words;
    b->cum = cum;
    bc->used += (sizeof(block) + (size_t)n * (sizeof(bop) + 6) + 7) & ~(size_t)7;
    bc->tab[(pc >> 2) & (BLOCKS_TAB - 1)] = b;
    return b;
}

/* The block at pc, decoded when missing, checked against its page counter. n = 0: none (step() runs it). */
static __attribute__((noinline)) block *block_get_slow(ps1_machine *m, uint32_t pc)
{
    struct ps1_blocks *bc = m->blocks;
    if (!bc) {
        bc = m->blocks = calloc(1, sizeof *bc);
        if (bc && !(bc->arena = malloc(BLOCKS_ARENA))) {
            free(bc);
            bc = m->blocks = NULL;
        }
        if (!bc)
            return NULL;
    }
    block *b = bc->tab[(pc >> 2) & (BLOCKS_TAB - 1)];
    if (b && b->pc == pc && b->page != BLOCK_NOPAGE && m->ram_gen[b->page] != b->gen &&
        !memcmp(m->ram + b->ram_lo, b->words, b->len4)) {
        b->gen = m->ram_gen[b->page]; /* the page was written, not this code */
        return b;
    }
    b = block_build(m, bc, pc);
    if (!b) { 
        ps1_cpu_flush_blocks(m);
        b = block_build(m, bc, pc);
    }
    return b;
}

static inline __attribute__((always_inline)) block *block_get(ps1_machine *m, uint32_t pc)
{
    struct ps1_blocks *bc = m->blocks;
    if (__builtin_expect(bc != NULL, 1)) {
        block *b = bc->tab[(pc >> 2) & (BLOCKS_TAB - 1)];
        if (__builtin_expect(b && b->pc == pc && (b->page == BLOCK_NOPAGE || m->ram_gen[b->page] == b->gen), 1))
            return b;
    }
    return block_get_slow(m, pc);
}

/* Main RAM and the scratchpad as load32 decodes them (LW class, with its cycles: KSEG2's view of RAM and KSEG1's of
 * the scratchpad included). */
#define RAM_LOAD32_OK(a) (phys_of(a) < 0x00800000u && ((0x0Eu >> ((a) >> 29)) & 1) == 0)
#define SCRATCH_LOAD32_OK(a) ((phys_of(a) & 0xFFFFFC00u) == 0x1F800000u)
#define TOUCH(a) (m->ram_gen[((a) & (PS1_RAM_SIZE - 1)) >> PS1_RAM_PAGE_SHIFT]++)

enum { BLOCKS_STOPPED, BLOCKS_INTERPRET, BLOCKS_SLOW };

/* A block's load or store outside main RAM and the scratchpad (IO, the BIOS ROM, the expansions): through the bus as
 * step() makes it, the clock where step() has it then (at: the fetch cycles of the rest of the block not counted;
 * load32 adds its load cycles first). Returns -1, nothing done, when it would raise an exception (step() runs it). */
static __attribute__((noinline)) int block_bus(ps1_machine *m, uint32_t addr, int width, int store, int load32,
                                               uint32_t *v, uint64_t at)
{
    if (ps1_bus_fault(m, addr, store))
        return -1;
    if (load32)
        at += (phys_of(addr) & 0xFFF80000u) == 0x1FC00000u ? LOAD_BIOS : LOAD_IO;
    m->cycles = at;
    if (store)
        (void)(width == 4 ? ps1_write32 : width == 2 ? ps1_write16 : ps1_write8)(m, addr, *v);
    else
        (void)(width == 4 ? ps1_read32 : width == 2 ? ps1_read16 : ps1_read8)(m, addr, v);
    return 0;
}

/* Runs blocks one after the other from m->pc (no delay slot, no load in flight, kernel mode, cache not isolated, the
 * run loop's checks passed). Returns BLOCKS_STOPPED where the run loop stops (every field of the machine as step()
 * would leave it), BLOCKS_INTERPRET when the instruction at pc must go through the per-instruction loop (no block
 * there, a delay slot or a load in flight after a block cut short, the hook address inside), BLOCKS_SLOW when step()
 * must run it (an op that falls back); the machine holds the state in front of it then. */
static __attribute__((noinline)) int run_blocks(ps1_machine *m, const bool unliftable)
{
    uint32_t *const r = m->r;
    uint8_t *const ram = m->ram;
    const uint32_t sr = m->cop0[C0_SR];
    uint32_t pc = m->pc, npc = m->next_pc, dl = 0, ldr = m->ld_reg, ldv = m->ld_val, hook = m->hook_at;
    uint64_t cyc = m->cycles;
    uint64_t nev = m->irq_pending ? 0 : m->next_event; /* only a block's bus access changes them (BBUS) */
    const block *b;
    const bop *o, *oe, *ostop;
    uint32_t lold, pv, btgt, btaken, v, addr, selfmod, pend0;
    int ret;
    static const void *const op_label[K_UNSUPPORTED] = {
        [K_NOP] = &&l_nop, [K_ADDU] = &&l_addu, [K_SUBU] = &&l_subu, [K_AND] = &&l_and, [K_OR] = &&l_or,
        [K_XOR] = &&l_xor, [K_NOR] = &&l_nor, [K_SLT] = &&l_slt, [K_SLTU] = &&l_sltu, [K_SLLV] = &&l_sllv,
        [K_SRLV] = &&l_srlv, [K_SRAV] = &&l_srav, [K_ADD] = &&l_add, [K_SUB] = &&l_sub, [K_SLL] = &&l_sll,
        [K_SRL] = &&l_srl, [K_SRA] = &&l_sra, [K_ADDIU] = &&l_addiu, [K_SLTI] = &&l_slti, [K_SLTIU] = &&l_sltiu,
        [K_ANDI] = &&l_andi, [K_ORI] = &&l_ori, [K_XORI] = &&l_xori, [K_LUI] = &&l_lui, [K_ADDI] = &&l_addi,
        [K_MFHI] = &&l_mfhi, [K_MFLO] = &&l_mflo, [K_MTHI] = &&l_mthi, [K_MTLO] = &&l_mtlo, [K_MULT] = &&l_mult,
        [K_MULTU] = &&l_multu, [K_DIV] = &&l_div, [K_DIVU] = &&l_divu, [K_LB] = &&l_lb, [K_LBU] = &&l_lbu,
        [K_LH] = &&l_lh, [K_LHU] = &&l_lhu, [K_LW] = &&l_lw, [K_LWL] = &&l_lwl, [K_LWR] = &&l_lwr,
        [K_MFC2] = &&l_mfc2, [K_CFC2] = &&l_cfc2, [K_SB] = &&l_sb, [K_SH] = &&l_sh, [K_SW] = &&l_sw,
        [K_SWL] = &&l_swl, [K_SWR] = &&l_swr, [K_MTC2] = &&l_mtc2, [K_CTC2] = &&l_ctc2, [K_LWC2] = &&l_lwc2,
        [K_SWC2] = &&l_swc2, [K_GTE] = &&l_gte, [K_BEQ] = &&l_beq, [K_BNE] = &&l_bne, [K_BLEZ] = &&l_blez,
        [K_BGTZ] = &&l_bgtz, [K_BLTZ] = &&l_bltz, [K_BGEZ] = &&l_bgez, [K_J] = &&l_j, [K_JR] = &&l_jr,
    };

/* The next op, or the end of the run. */
#define NEXT()                                                                                                         \
    do {                                                                                                               \
        if (__builtin_expect(++o != ostop, 1))                                                                         \
            goto *op_label[o->k];                                                                                      \
        goto done;                                                                                                     \
    } while (0)
/* A delayed write: lands at once with an op after it in the block (the old value kept for a fall back of that op),
 * in flight when it is the last. */
#define BDELAYED(val)                                                                                                  \
    do {                                                                                                               \
        v = (val);                                                                                                     \
        if (o + 1 != oe) {                                                                                             \
            lold = r[o->d];                                                                                            \
            r[o->d] = v;                                                                                               \
        } else {                                                                                                       \
            ldr = o->d;                                                                                                \
        }                                                                                                              \
        pv = ldv;                                                                                                      \
        ldv = v;                                                                                                       \
    } while (0)
/* Kernel mode, cache not isolated (block entry): main RAM as ram_addr / write_ram_ok see it (bytes, halfwords,
 * stores), the scratchpad as the bus decodes it (KUSEG, KSEG0). */
#define BRAM(a) (((0x31u >> ((a) >> 29)) & 1) && ((a) & 0x1F800000u) == 0)
#define BSCRATCH(a) (((a) & 0xFFFFFC00u) == 0x1F800000u || ((a) & 0xFFFFFC00u) == 0x9F800000u)
#define BLOAD32(a, out)                                                                                                \
    do {                                                                                                               \
        if (__builtin_expect(RAM_LOAD32_OK(a), 1)) {                                                                   \
            out = rd32le(ram + (phys_of(a) & 0x1FFFFCu));                                                              \
            cyc += LOAD_RAM;                                                                                           \
        } else if (SCRATCH_LOAD32_OK(a)) {                                                                             \
            out = rd32le(m->scratch + (phys_of(a) & 0x3FCu));                                                          \
        } else {                                                                                                       \
            BBUS(a, 4, 0, 1, out);                                                                                     \
        }                                                                                                              \
    } while (0)
/* The bus (block_bus): IO may raise an interrupt, move the next event, start a DMA into code, so the block stops
 * after this op, and the next one is looked up again. */
#define BBUS(a, width, store, load32, var)                                                                             \
    do {                                                                                                               \
        uint64_t rest_ = (uint64_t)b->fc * (uint32_t)(oe - o - 1);                                                     \
        if (block_bus(m, a, width, store, load32, &(var), cyc - rest_) < 0)                                            \
            goto bail;                                                                                                 \
        cyc = m->cycles + rest_;                                                                                       \
        nev = m->irq_pending ? 0 : m->next_event;                                                                      \
        ostop = o + 1;                                                                                                 \
        selfmod = 1;                                                                                                   \
    } while (0)
/* A store into the block's own words: the block stops after it (the next lookup decodes them again). */
#define BSTORE_RAM(a)                                                                                                  \
    do {                                                                                                               \
        TOUCH(a);                                                                                                      \
        if (__builtin_expect(((a) & 0x1FFFFCu) - b->ram_lo < b->len4, 0)) {                                            \
            selfmod = 1;                                                                                               \
            ostop = o + 1;                                                                                             \
        }                                                                                                              \
    } while (0)
/* The state in front of the block's op at o (k): the fetch cycles of ops k.. taken back, a load that landed early
 * back in flight with the register's old value; in the delay slot, next_pc the branch's target and the flags. */
#define BLOCK_STATE_BEFORE()                                                                                           \
    do {                                                                                                               \
        uint32_t k_ = (uint32_t)(o - b->ops);                                                                          \
        cyc -= (uint64_t)b->fc * (b->n - k_);                                                                          \
        if (k_ > 0 ? K_IS_DELAYED(o[-1].k) : pend0 != 0) {                                                            \
            ldr = k_ > 0 ? o[-1].d : pend0;                                                                            \
            r[ldr] = lold;                                                                                             \
        }                                                                                                              \
        pc = b->pc + 4 * k_;                                                                                           \
        if (b->branch && o + 1 == oe) {                                                                                \
            npc = btgt;                                                                                                \
            dl = 1 | btaken << 1;                                                                                      \
            if (b->branch == 2 && btaken && is_hook_target(btgt))                                                      \
                hook = btgt;                                                                                           \
        } else {                                                                                                       \
            npc = pc + 4;                                                                                              \
        }                                                                                                              \
    } while (0)

next_block:
    b = block_get(m, pc);
    if (!b || !b->n || hook - pc < b->len4 || (unliftable && ps1_rc_liftable_pc(pc)) ||
        (ldr && ((b->conf0 >> ldr) & 1))) {
        ret = BLOCKS_INTERPRET;
        goto out;
    }
again:
    o = b->ops;
    oe = ostop = o + b->n;
    lold = 0;
    pv = ldv;
    pend0 = ldr; /* a load in flight in front of the block lands at once: its first op can't tell (conf0) */
    if (ldr) {
        lold = r[ldr];
        r[ldr] = ldv;
        ldr = 0;
    }
    btgt = b->fall;
    btaken = 0;
    selfmod = 0;
    if (__builtin_expect(cyc + b->maxcost >= nev, 0)) {
        /* Only the ops before the first that might meet next_event (the loop checked the first: cyc < nev). */
        uint32_t k = 1;
        while (k < b->n && cyc + b->cum[k] < nev)
            k++;
        ostop = o + k;
    }
    cyc += b->fcsum;
    goto *op_label[o->k];

l_nop: NEXT();
l_addu: r[o->d] = r[o->s] + r[o->t]; NEXT();
l_subu: r[o->d] = r[o->s] - r[o->t]; NEXT();
l_and: r[o->d] = r[o->s] & r[o->t]; NEXT();
l_or: r[o->d] = r[o->s] | r[o->t]; NEXT();
l_xor: r[o->d] = r[o->s] ^ r[o->t]; NEXT();
l_nor: r[o->d] = ~(r[o->s] | r[o->t]); NEXT();
l_slt: r[o->d] = (int32_t)r[o->s] < (int32_t)r[o->t]; NEXT();
l_sltu: r[o->d] = r[o->s] < r[o->t]; NEXT();
l_sllv: r[o->d] = r[o->t] << (r[o->s] & 31); NEXT();
l_srlv: r[o->d] = r[o->t] >> (r[o->s] & 31); NEXT();
l_srav: r[o->d] = (uint32_t)((int32_t)r[o->t] >> (r[o->s] & 31)); NEXT();
l_add: {
    uint32_t a = r[o->s], c = r[o->t], x = a + c;
    if ((~(a ^ c) & (a ^ x)) & 0x80000000u)
        goto bail;
    r[o->d] = x;
    NEXT();
}
l_sub: {
    uint32_t a = r[o->s], c = r[o->t], x = a - c;
    if (((a ^ c) & (a ^ x)) & 0x80000000u)
        goto bail;
    r[o->d] = x;
    NEXT();
}
l_sll: r[o->d] = r[o->t] << o->imm; NEXT();
l_srl: r[o->d] = r[o->t] >> o->imm; NEXT();
l_sra: r[o->d] = (uint32_t)((int32_t)r[o->t] >> o->imm); NEXT();
l_addiu: r[o->d] = r[o->s] + o->imm; NEXT();
l_slti: r[o->d] = (int32_t)r[o->s] < (int32_t)o->imm; NEXT();
l_sltiu: r[o->d] = r[o->s] < o->imm; NEXT();
l_andi: r[o->d] = r[o->s] & o->imm; NEXT();
l_ori: r[o->d] = r[o->s] | o->imm; NEXT();
l_xori: r[o->d] = r[o->s] ^ o->imm; NEXT();
l_lui: r[o->d] = o->imm; NEXT();
l_addi: {
    uint32_t a = r[o->s], c = o->imm, x = a + c;
    if ((~(a ^ c) & (a ^ x)) & 0x80000000u)
        goto bail;
    r[o->d] = x;
    NEXT();
}
l_mfhi: r[o->d] = m->hi; NEXT();
l_mflo: r[o->d] = m->lo; NEXT();
l_mthi: m->hi = r[o->s]; NEXT();
l_mtlo: m->lo = r[o->s]; NEXT();
l_mult: {
    int64_t x = (int64_t)(int32_t)r[o->s] * (int64_t)(int32_t)r[o->t];
    m->lo = (uint32_t)x;
    m->hi = (uint32_t)((uint64_t)x >> 32);
    NEXT();
}
l_multu: {
    uint64_t x = (uint64_t)r[o->s] * (uint64_t)r[o->t];
    m->lo = (uint32_t)x;
    m->hi = (uint32_t)(x >> 32);
    NEXT();
}
l_div: {
    int32_t n = (int32_t)r[o->s], d = (int32_t)r[o->t];
    if (d == 0) {
        m->hi = (uint32_t)n;
        m->lo = n >= 0 ? 0xFFFFFFFFu : 1u;
    } else if ((uint32_t)n == 0x80000000u && d == -1) {
        m->hi = 0;
        m->lo = 0x80000000u;
    } else {
        m->lo = (uint32_t)(n / d);
        m->hi = (uint32_t)(n % d);
    }
    NEXT();
}
l_divu: {
    uint32_t n = r[o->s], d = r[o->t];
    if (d == 0) {
        m->hi = n;
        m->lo = 0xFFFFFFFFu;
    } else {
        m->lo = n / d;
        m->hi = n % d;
    }
    NEXT();
}
l_lb:
    addr = r[o->s] + o->imm;
    if (__builtin_expect(BRAM(addr), 1))
        v = ram[addr & 0x1FFFFFu];
    else if (BSCRATCH(addr))
        v = m->scratch[addr & 0x3FFu];
    else
        BBUS(addr, 1, 0, 0, v);
    BDELAYED((uint32_t)(int32_t)(int8_t)v);
    NEXT();
l_lbu:
    addr = r[o->s] + o->imm;
    if (__builtin_expect(BRAM(addr), 1))
        v = ram[addr & 0x1FFFFFu];
    else if (BSCRATCH(addr))
        v = m->scratch[addr & 0x3FFu];
    else
        BBUS(addr, 1, 0, 0, v);
    BDELAYED(v & 0xFFu); /* the bus may answer wider (the cache control register) */
    NEXT();
l_lh:
l_lhu: {
    const uint8_t *q;
    addr = r[o->s] + o->imm;
    if (addr & 1)
        goto bail;
    if (__builtin_expect(BRAM(addr), 1)) {
        q = ram + (addr & 0x1FFFFEu);
        v = (uint32_t)q[0] | (uint32_t)q[1] << 8;
    } else if (BSCRATCH(addr)) {
        q = m->scratch + (addr & 0x3FEu);
        v = (uint32_t)q[0] | (uint32_t)q[1] << 8;
    } else {
        BBUS(addr, 2, 0, 0, v);
    }
    BDELAYED(o->k == K_LH ? (uint32_t)(int32_t)(int16_t)v : v & 0xFFFFu);
    NEXT();
}
l_lw:
    addr = r[o->s] + o->imm;
    if (addr & 3)
        goto bail;
    BLOAD32(addr, v);
    BDELAYED(v);
    NEXT();
l_lwl:
l_lwr: {
    uint32_t w, cur = r[o->d], sh; /* no load in flight to d here (block rule) */
    addr = r[o->s] + o->imm;
    sh = addr & 3;
    BLOAD32(addr & ~3u, w);
    if (o->k == K_LWL)
        BDELAYED(sh == 3 ? w : (cur & (0x00FFFFFFu >> (sh * 8))) | (w << (24 - sh * 8)));
    else
        BDELAYED(sh == 0 ? w : (cur & (0xFFFFFF00u << (24 - sh * 8))) | (w >> (sh * 8)));
    NEXT();
}
l_mfc2:
    if (!(sr & SR_CU2))
        goto bail;
    BDELAYED(ps1_gte_read_data(&m->gte, o->t));
    NEXT();
l_cfc2:
    if (!(sr & SR_CU2))
        goto bail;
    BDELAYED(ps1_gte_read_ctrl(&m->gte, o->t));
    NEXT();
l_sb:
    addr = r[o->s] + o->imm;
    if (__builtin_expect(BRAM(addr), 1)) {
        BSTORE_RAM(addr);
        ram[addr & 0x1FFFFFu] = (uint8_t)r[o->t];
    } else if (BSCRATCH(addr)) {
        m->scratch[addr & 0x3FFu] = (uint8_t)r[o->t];
    } else {
        v = r[o->t];
        BBUS(addr, 1, 1, 0, v);
    }
    NEXT();
l_sh: {
    uint8_t *q;
    addr = r[o->s] + o->imm;
    if (addr & 1)
        goto bail;
    if (__builtin_expect(BRAM(addr), 1)) {
        BSTORE_RAM(addr);
        q = ram + (addr & 0x1FFFFEu);
    } else if (BSCRATCH(addr)) {
        q = m->scratch + (addr & 0x3FEu);
    } else {
        v = r[o->t];
        BBUS(addr, 2, 1, 0, v);
        NEXT();
    }
    q[0] = (uint8_t)r[o->t];
    q[1] = (uint8_t)(r[o->t] >> 8);
    NEXT();
}
l_sw:
    addr = r[o->s] + o->imm;
    if (addr & 3)
        goto bail;
    v = rd32le((const uint8_t *)&r[o->t]);
    if (__builtin_expect(BRAM(addr), 1)) {
        BSTORE_RAM(addr);
        memcpy(ram + (addr & 0x1FFFFCu), &v, 4);
    } else if (BSCRATCH(addr)) {
        memcpy(m->scratch + (addr & 0x3FCu), &v, 4);
    } else {
        v = r[o->t];
        BBUS(addr, 4, 1, 0, v);
    }
    NEXT();
l_swl:
l_swr: {
    uint32_t a, sh, w, x, y = r[o->t];
    addr = r[o->s] + o->imm;
    a = addr & ~3u;
    sh = addr & 3;
    if (!(RAM_LOAD32_OK(a) && BRAM(a)))
        goto bail;
    w = rd32le(ram + (phys_of(a) & 0x1FFFFCu));
    cyc += LOAD_RAM;
    if (o->k == K_SWL)
        x = sh == 3 ? y : (w & (0xFFFFFF00u << (sh * 8))) | (y >> (24 - sh * 8));
    else
        x = sh == 0 ? y : (w & (0x00FFFFFFu >> (24 - sh * 8))) | (y << (sh * 8));
    BSTORE_RAM(a);
    x = rd32le((const uint8_t *)&x);
    memcpy(ram + (a & 0x1FFFFCu), &x, 4);
    NEXT();
}
l_mtc2:
    if (!(sr & SR_CU2))
        goto bail;
    ps1_gte_write_data(&m->gte, o->d, r[o->t]);
    NEXT();
l_ctc2:
    if (!(sr & SR_CU2))
        goto bail;
    ps1_gte_write_ctrl(&m->gte, o->d, r[o->t]);
    NEXT();
l_lwc2:
    addr = r[o->s] + o->imm;
    if (!(sr & SR_CU2) || (addr & 3))
        goto bail;
    BLOAD32(addr, v);
    ps1_gte_write_data(&m->gte, o->d, v);
    NEXT();
l_swc2: {
    uint8_t *q;
    addr = r[o->s] + o->imm;
    if (!(sr & SR_CU2) || (addr & 3))
        goto bail;
    if (__builtin_expect(BRAM(addr), 1)) {
        BSTORE_RAM(addr);
        q = ram + (addr & 0x1FFFFCu);
    } else if (BSCRATCH(addr)) {
        q = m->scratch + (addr & 0x3FCu);
    } else {
        goto bail;
    }
    v = ps1_gte_read_data(&m->gte, o->t);
    v = rd32le((const uint8_t *)&v);
    memcpy(q, &v, 4);
    NEXT();
}
l_gte:
    if (!(sr & SR_CU2))
        goto bail;
    cyc += (uint64_t)ps1_gte_command(&m->gte, o->imm);
    NEXT();
l_beq:
    if (r[o->s] == r[o->t])
        btgt = o->imm, btaken = 1;
    NEXT();
l_bne:
    if (r[o->s] != r[o->t])
        btgt = o->imm, btaken = 1;
    NEXT();
l_blez:
    if ((int32_t)r[o->s] <= 0)
        btgt = o->imm, btaken = 1;
    NEXT();
l_bgtz:
    if ((int32_t)r[o->s] > 0)
        btgt = o->imm, btaken = 1;
    NEXT();
l_bltz:
    if ((int32_t)r[o->s] < 0)
        btgt = o->imm, btaken = 1;
    r[o->d] = o->d ? b->fall : 0; /* BLTZAL links even when not taken */
    NEXT();
l_bgez:
    if ((int32_t)r[o->s] >= 0)
        btgt = o->imm, btaken = 1;
    r[o->d] = o->d ? b->fall : 0;
    NEXT();
l_j:
    btgt = o->imm;
    btaken = 1;
    r[o->d] = o->d ? b->fall : 0;
    NEXT();
l_jr:
    btgt = r[o->s];
    btaken = 1;
    r[o->d] = o->d ? b->fall : 0;
    NEXT();

done:
    /* After the op before o (j): the run loop's state in front of the next instruction. Past the block's end: its
     * branch target or fall-through. Inside it (cut short by next_event, a store into its code): as in front of
     * op j + 1. */
    if (o == oe) {
        pc = btgt;
        npc = pc + 4;
        if (b->branch == 2 && btaken && is_hook_target(btgt))
            hook = btgt;
    } else {
        BLOCK_STATE_BEFORE();
    }
    if (cyc >= nev || pc == hook || (unliftable && ps1_rc_liftable_pc(pc))) {
        uint32_t j = (uint32_t)(o - b->ops) - 1;
        m->cur_pc = b->pc + 4 * j;
        m->in_delay = b->branch && j == b->n - 1u;
        m->branch_taken = m->in_delay && btaken;
        uint32_t lp = j > 0 ? (K_IS_DELAYED(b->ops[j - 1].k) ? b->ops[j - 1].d : 0) : pend0;
        m->pend_reg = lp && op_wdest(&b->ops[j]) != lp ? lp : 0;
        m->pend_val = K_IS_DELAYED(b->ops[j].k) ? pv : ldv;
        ret = BLOCKS_STOPPED;
        goto out;
    }
    if (dl) {
        ret = BLOCKS_INTERPRET;
        goto out;
    }
    if (pc == b->pc && !selfmod && !ldr)
        goto again; /* a loop on itself: the same checks pass (the hook address is not pc: checked just above) */
    goto next_block;

bail:
    /* The op at o falls back to step(), from the state in front of it. */
    BLOCK_STATE_BEFORE();
    ret = BLOCKS_SLOW;
out:
    m->pc = pc;
    m->next_pc = npc;
    m->next_delay = dl & 1;
    m->next_taken = (dl >> 1) & 1;
    m->ld_reg = ldr;
    m->ld_val = ldv;
    m->cycles = cyc;
    m->hook_at = hook;
    return ret;
#undef NEXT
}

/* The run loop with blocks: blocks while they go, step() for the instructions in between (a delay slot after a block
 * cut short, user mode or an isolated cache, an instruction no block holds, an op that fell back). At least one
 * instruction, then on until the check ps1_cpu_run's loop (or the unliftable loop) makes before each instruction
 * would stop it. */
static inline __attribute__((always_inline)) void run_fast(ps1_machine *m, const bool unliftable)
{
    for (;;) {
        if (__builtin_expect(!m->next_delay && m->next_pc == m->pc + 4 && !(m->cop0[C0_SR] & (SR_KUC | SR_ISC)), 1) &&
            run_blocks(m, unliftable) == BLOCKS_STOPPED)
            return;
        slow_step(m);
        if (m->cycles >= m->next_event || m->irq_pending || m->pc == m->hook_at ||
            (unliftable && ps1_rc_liftable_pc(m->pc)))
            return;
    }
}

/* ps1_run --lockstep-ref (XBA-204): this machine runs step()'s own loop, the reference the register-cached loop is
 * compared with. */
void ps1_cpu_set_reference(ps1_machine *m, int on) { m->interp_ref = on != 0; }

void ps1_cpu_step_unliftable(ps1_machine *m)
{
    if (!m->interp_ref) {
        run_fast(m, true);
        return;
    }
    do {
        step(m);
    } while (m->cycles < m->next_event && !m->irq_pending && m->pc != m->hook_at && !ps1_rc_liftable_pc(m->pc));
}
void ps1_cpu_take_interrupt(ps1_machine *m) { take_interrupt(m); }

void ps1_cpu_hook_check(ps1_machine *m)
{
    if (m->pc == m->hook_at)
        hook(m);
}

void ps1_cpu_run(ps1_machine *m)
{
    if (m->rc) {
        ps1_recomp_run(m); /* a recompiled title pack is attached (ps1_recomp.c): same loop, lifted code too */
        return;
    }
    bool ref = m->interp_ref;
    while (m->cycles < m->next_event) {
        if (m->irq_pending) {
            take_interrupt(m);
            continue;
        }
        if (m->pc == m->hook_at)
            hook(m);
        if (ref)
            slow_step(m);
        else
            run_fast(m, false); /* one instruction at least, then on until the next check this loop makes */
    }
}
