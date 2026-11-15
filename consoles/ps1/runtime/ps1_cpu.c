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

void ps1_cpu_step_unliftable(ps1_machine *m)
{
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
    while (m->cycles < m->next_event) {
        if (m->irq_pending) {
            take_interrupt(m);
            continue;
        }
        if (m->pc == m->hook_at)
            hook(m);
        step(m);
    }
}
