/* N64 core unit tests (XBA-171): VR4300 integer instructions, delay slots and branch-likely, exceptions (EPC / BD /
 * codes / vectors), COUNT / COMPARE and MI interrupts, the TLB (refill, XTLB refill, invalid, modified, TLBP / TLBR,
 * page masks), 64-bit address spaces, and COP1 (FR views, rounding modes, FCSR flags / causes / enables, the
 * unimplemented-operation cases). Instructions are hand-encoded; expected values come from the VR4300 user's manual. */
#include "n64_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        checks++;                                                                                                     \
        if (!(cond)) {                                                                                                \
            fails++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)
#define CHECK_EQ(got, want, what)                                                                                     \
    CHECK((uint64_t)(got) == (uint64_t)(want), "%s = 0x%016llX, want 0x%016llX", what, (unsigned long long)(got),    \
          (unsigned long long)(want))

/* ---- Encoding -------------------------------------------------------------------------------------------------- */
#define R_(fn, rs, rt, rd, sa) ((uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | (uint32_t)(rd) << 11 | (uint32_t)(sa) << 6 | (fn))
#define I_(op, rs, rt, imm) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((uint32_t)(imm) & 0xFFFF))
#define J_(op, target) ((uint32_t)(op) << 26 | (((uint32_t)(target) >> 2) & 0x03FFFFFF))
#define NOP 0u
#define ADD(d, s, t) R_(0x20, s, t, d, 0)
#define ADDU(d, s, t) R_(0x21, s, t, d, 0)
#define DADDU(d, s, t) R_(0x2D, s, t, d, 0)
#define SRA(d, t, sa) R_(0x03, 0, t, d, sa)
#define SRL(d, t, sa) R_(0x02, 0, t, d, sa)
#define DSLL32(d, t, sa) R_(0x3C, 0, t, d, sa)
#define DSRA32(d, t, sa) R_(0x3F, 0, t, d, sa)
#define DIV(s, t) R_(0x1A, s, t, 0, 0)
#define DDIV(s, t) R_(0x1E, s, t, 0, 0)
#define DMULT(s, t) R_(0x1C, s, t, 0, 0)
#define DMULTU(s, t) R_(0x1D, s, t, 0, 0)
#define MFHI(d) R_(0x10, 0, 0, d, 0)
#define MFLO(d) R_(0x12, 0, 0, d, 0)
#define SYSCALL R_(0x0C, 0, 0, 0, 0)
#define BREAK R_(0x0D, 0, 0, 0, 0)
#define TEQ(s, t) R_(0x34, s, t, 0, 0)
#define ADDIU(t, s, i) I_(0x09, s, t, i)
#define ORI(t, s, i) I_(0x0D, s, t, i)
#define LUI(t, i) I_(0x0F, 0, t, i)
#define BEQ(s, t, off) I_(0x04, s, t, off)
#define BEQL(s, t, off) I_(0x14, s, t, off)
#define BNEL(s, t, off) I_(0x15, s, t, off)
#define LW(t, s, i) I_(0x23, s, t, i)
#define LD(t, s, i) I_(0x37, s, t, i)
#define SW(t, s, i) I_(0x2B, s, t, i)
#define LWL(t, s, i) I_(0x22, s, t, i)
#define LWR(t, s, i) I_(0x26, s, t, i)
#define LDL(t, s, i) I_(0x1A, s, t, i)
#define LDR(t, s, i) I_(0x1B, s, t, i)
#define SDL(t, s, i) I_(0x2C, s, t, i)
#define SDR(t, s, i) I_(0x2D, s, t, i)
#define LL(t, s, i) I_(0x30, s, t, i)
#define SC(t, s, i) I_(0x38, s, t, i)
#define MTC0(t, d) (0x40800000u | (uint32_t)(t) << 16 | (uint32_t)(d) << 11)
#define DMTC0(t, d) (0x40A00000u | (uint32_t)(t) << 16 | (uint32_t)(d) << 11)
#define MFC0(t, d) (0x40000000u | (uint32_t)(t) << 16 | (uint32_t)(d) << 11)
#define TLBWI 0x42000002u
#define TLBR 0x42000001u
#define TLBP 0x42000008u
#define ERET 0x42000018u
/* COP1 */
#define FOP(fmt, ft, fs, fd, fn) (0x44000000u | (uint32_t)(fmt) << 21 | (uint32_t)(ft) << 16 | (uint32_t)(fs) << 11 | (uint32_t)(fd) << 6 | (fn))
#define F_S 16
#define F_D 17
#define F_W 20
#define F_L 21

/* ---- Harness --------------------------------------------------------------------------------------------------- */
#define CODE 0x80001000u
#define DATA 0x80002000u

static n64_machine *mk(void)
{
    static uint8_t rom[0x2000];
    memset(rom, 0, sizeof rom);
    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
    char err[128];
    n64_machine *m = n64_create(rom, sizeof rom, NULL, err, sizeof err);
    if (!m) {
        printf("FAIL: n64_create: %s\n", err);
        exit(1);
    }
    /* Exception vectors: a jump to itself each, so the PC tells which was taken. */
    static const uint32_t vec[] = {0x80000000u, 0x80000080u, 0x80000180u};
    for (int i = 0; i < 3; i++) {
        n64_poke32(m, vec[i], J_(2, vec[i]));
        n64_poke32(m, vec[i] + 4, NOP);
    }
    m->cpu.cop0[C0_STATUS] = 0x34000000u; /* CU1 CU0 FR, kernel, 32-bit, interrupts off */
    m->cpu.fcsr = 0;
    n64_cpu_status_changed(m);
    return m;
}

static uint64_t sx(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }

/* Loads a program at CODE ending in a jump to itself, runs it (at most `cycles`). */
static void run_prog(n64_machine *m, const uint32_t *code, int n, uint64_t cycles)
{
    for (int i = 0; i < n; i++)
        n64_poke32(m, CODE + 4u * (uint32_t)i, code[i]);
    uint32_t end = CODE + 4u * (uint32_t)n;
    n64_poke32(m, end, J_(2, end));
    n64_poke32(m, end + 4, NOP);
    m->cpu.pc = sx(CODE);
    m->cpu.npc = m->cpu.pc + 4;
    m->cpu.next_ds = false;
    n64_run_cycles(m, cycles);
}

static bool at_end(n64_machine *m, int n)
{
    uint64_t end = sx(CODE + 4u * (uint32_t)n);
    return m->cpu.pc == end || m->cpu.pc == end + 4;
}

static bool at_vec(n64_machine *m, uint32_t v)
{
    return m->cpu.pc == sx(v) || m->cpu.pc == sx(v) + 4;
}

static uint32_t exc_code(n64_machine *m) { return ((uint32_t)m->cpu.cop0[C0_CAUSE] >> 2) & 31; }

/* Executes one instruction at CODE. */
static void step(n64_machine *m, uint32_t ins)
{
    n64_poke32(m, CODE, ins);
    m->cpu.pc = sx(CODE);
    m->cpu.npc = m->cpu.pc + 4;
    m->cpu.next_ds = false;
    m->stop_at = m->now + 1;
    n64_cpu_run(m);
}

/* ---- Integer --------------------------------------------------------------------------------------------------- */

static void test_integer(void)
{
    n64_machine *m = mk();
    uint64_t *g = m->cpu.gpr;

    g[8] = 0x0000000180000000ull;
    step(m, SRA(9, 8, 4)); /* the 64-bit value shifted, then truncated */
    CHECK_EQ(g[9], 0x0000000018000000ull, "SRA of a non sign-extended value");
    step(m, SRL(9, 8, 4));
    CHECK_EQ(g[9], 0x0000000008000000ull, "SRL");
    g[8] = 0x00000000FFFFFFFFull;
    g[10] = 1;
    step(m, ADDU(9, 8, 10));
    CHECK_EQ(g[9], 0, "ADDU wraps in 32 bits, sign-extended");
    g[8] = 0x12345678;
    step(m, DSLL32(9, 8, 4));
    CHECK_EQ(g[9], 0x2345678000000000ull, "DSLL32");
    g[8] = 0x8000000000000000ull;
    step(m, DSRA32(9, 8, 31));
    CHECK_EQ(g[9], ~0ull, "DSRA32 63");
    g[8] = 0x7FFFFFFF;
    step(m, LUI(9, 0x8000));
    CHECK_EQ(g[9], 0xFFFFFFFF80000000ull, "LUI sign-extends");

    /* DIV by zero: LO = -1 for a positive dividend, HI = dividend; DDIV INT64_MIN / -1. */
    g[8] = 5;
    g[10] = 0;
    step(m, DIV(8, 10));
    CHECK_EQ(m->cpu.lo, ~0ull, "DIV 5/0 LO");
    CHECK_EQ(m->cpu.hi, 5, "DIV 5/0 HI");
    g[8] = (uint64_t)-7;
    step(m, DIV(8, 10));
    CHECK_EQ(m->cpu.lo, 1, "DIV -7/0 LO");
    g[8] = 0x8000000000000000ull;
    g[10] = ~0ull;
    step(m, DDIV(8, 10));
    CHECK_EQ(m->cpu.lo, 0x8000000000000000ull, "DDIV overflow LO");
    CHECK_EQ(m->cpu.hi, 0, "DDIV overflow HI");
    g[8] = (uint64_t)-3;
    g[10] = 0x4000000000000001ull;
    step(m, DMULT(8, 10));
    CHECK_EQ(m->cpu.hi, 0xFFFFFFFFFFFFFFFFull, "DMULT HI");
    CHECK_EQ(m->cpu.lo, 0x3FFFFFFFFFFFFFFDull, "DMULT LO");
    g[8] = ~0ull;
    g[10] = ~0ull;
    step(m, DMULTU(8, 10));
    CHECK_EQ(m->cpu.hi, 0xFFFFFFFFFFFFFFFEull, "DMULTU HI");
    CHECK_EQ(m->cpu.lo, 1, "DMULTU LO");

    /* Unaligned big-endian loads / stores. */
    static const uint8_t bytes[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                      0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
    memcpy(m->rdram + 0x2000, bytes, 16);
    g[8] = sx(DATA);
    g[9] = 0;
    step(m, LWL(9, 8, 1));
    step(m, LWR(9, 8, 4));
    CHECK_EQ(g[9], 0x0000000022334455ull, "LWL/LWR at +1");
    g[9] = 0;
    step(m, LWL(9, 8, 6));
    step(m, LWR(9, 8, 9));
    CHECK_EQ(g[9], 0x00000000778899AAull, "LWL/LWR at +6: LWR keeps the upper half LWL sign-extended");
    g[9] = 0;
    step(m, LWR(9, 8, 11)); /* a whole word: sign-extended */
    CHECK_EQ(g[9], 0xFFFFFFFF99AABBCCull, "LWR of a whole word sign-extends");
    g[9] = 0;
    step(m, LDL(9, 8, 3));
    step(m, LDR(9, 8, 10));
    CHECK_EQ(g[9], 0x445566778899AABBull, "LDL/LDR at +3");
    g[9] = 0x0102030405060708ull;
    step(m, SDL(9, 8, 2));
    step(m, SDR(9, 8, 9));
    static const uint8_t after[16] = {0x11, 0x22, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                      0x07, 0x08, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
    CHECK(memcmp(m->rdram + 0x2000, after, 16) == 0, "SDL/SDR at +2..+9");

    /* LL / SC: SC succeeds after LL, fails after ERET cleared LLbit. */
    g[9] = 0;
    step(m, LL(9, 8, 0));
    g[9] = 0xABCD;
    step(m, SC(9, 8, 0));
    CHECK_EQ(g[9], 1, "SC after LL");
    CHECK_EQ(n64_peek32(m, DATA), 0xABCD, "SC stored");
    m->cpu.llbit = false;
    g[9] = 0x1234;
    step(m, SC(9, 8, 0));
    CHECK_EQ(g[9], 0, "SC without LL");
    CHECK_EQ(n64_peek32(m, DATA), 0xABCD, "failed SC stores nothing");
    n64_destroy(m);
}

static void test_branches(void)
{
    n64_machine *m = mk();
    uint64_t *g = m->cpu.gpr;
    /* BEQL not taken skips its delay slot; taken BNEL runs it. */
    const uint32_t p[] = {
        ADDIU(8, 0, 1),
        ADDIU(9, 0, 2),
        BEQL(8, 9, 2),
        ADDIU(10, 0, 0x55), /* nullified */
        BNEL(8, 9, 2),
        ADDIU(11, 0, 0x66), /* runs */
        ADDIU(12, 0, 0x77), /* skipped by the branch */
        NOP,
    };
    run_prog(m, p, 8, 200);
    CHECK(at_end(m, 8), "branch program ended (pc %llx)", (unsigned long long)m->cpu.pc);
    CHECK_EQ(g[10], 0, "BEQL not taken nullifies its delay slot");
    CHECK_EQ(g[11], 0x66, "BNEL taken runs its delay slot");
    CHECK_EQ(g[12], 0, "BNEL target skips");
    n64_destroy(m);
}

static void test_exceptions(void)
{
    n64_machine *m = mk();
    uint64_t *g = m->cpu.gpr;

    /* ADD overflow. */
    const uint32_t ov[] = {LUI(8, 0x7FFF), ORI(8, 8, 0xFFFF), ADDIU(9, 0, 1), ADD(10, 8, 9), NOP};
    g[10] = 0x1234;
    run_prog(m, ov, 5, 200);
    CHECK(at_vec(m, 0x80000180u), "overflow -> general vector");
    CHECK_EQ(exc_code(m), EXC_OV, "overflow code");
    CHECK_EQ(m->cpu.cop0[C0_EPC], sx(CODE + 12), "overflow EPC");
    CHECK(!(m->cpu.cop0[C0_CAUSE] & CAUSE_BD), "overflow BD clear");
    CHECK_EQ(g[10], 0x1234, "overflow leaves rd");

    /* The same in a delay slot: EPC = the branch, BD set. */
    m->cpu.cop0[C0_STATUS] &= ~(uint64_t)SR_EXL;
    n64_cpu_status_changed(m);
    const uint32_t ovd[] = {LUI(8, 0x7FFF), ORI(8, 8, 0xFFFF), ADDIU(9, 0, 1), BEQ(0, 0, 4), ADD(10, 8, 9), NOP, NOP,
                            NOP};
    run_prog(m, ovd, 8, 200);
    CHECK(at_vec(m, 0x80000180u), "delay slot overflow -> vector");
    CHECK_EQ(m->cpu.cop0[C0_EPC], sx(CODE + 12), "delay slot EPC = branch");
    CHECK(m->cpu.cop0[C0_CAUSE] & CAUSE_BD, "delay slot BD set");

    /* Codes: misaligned load, SYSCALL, BREAK, TEQ, reserved opcode, COP1 unusable. */
    struct {
        uint32_t ins;
        int code;
        const char *name;
    } cases[] = {
        {LW(9, 8, 2), EXC_ADEL, "LW misaligned"},
        {SW(9, 8, 1), EXC_ADES, "SW misaligned"},
        {SYSCALL, EXC_SYS, "SYSCALL"},
        {BREAK, EXC_BP, "BREAK"},
        {TEQ(0, 0), EXC_TR, "TEQ"},
        {0x7C000000u, EXC_RI, "reserved opcode 0x1F"},
        {0x4C000000u, EXC_RI, "COP3"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        m->cpu.cop0[C0_STATUS] &= ~(uint64_t)SR_EXL;
        n64_cpu_status_changed(m);
        g[8] = sx(DATA);
        step(m, cases[i].ins);
        CHECK(m->cpu.pc == sx(0x80000180u), "%s -> vector (pc %llx)", cases[i].name, (unsigned long long)m->cpu.pc);
        CHECK(exc_code(m) == (uint32_t)cases[i].code, "%s code %u", cases[i].name, exc_code(m));
        CHECK_EQ(m->cpu.cop0[C0_EPC], sx(CODE), cases[i].name);
    }
    m->cpu.cop0[C0_STATUS] = 0x14000000u; /* CU1 off */
    n64_cpu_status_changed(m);
    step(m, FOP(F_S, 2, 4, 6, 0));
    CHECK_EQ(exc_code(m), EXC_CPU, "COP1 unusable code");
    CHECK_EQ((m->cpu.cop0[C0_CAUSE] >> 28) & 3, 1, "COP1 unusable CE");
    m->cpu.cop0[C0_STATUS] = 0x34000000u;
    n64_cpu_status_changed(m);
    g[8] = sx(DATA + 1);
    step(m, LW(9, 8, 1));
    CHECK_EQ(m->cpu.cop0[C0_BADVADDR], sx(DATA + 2), "BadVAddr of a misaligned load");

    /* ERET returns to EPC and clears EXL. */
    m->cpu.cop0[C0_EPC] = sx(DATA + 0x100);
    m->cpu.cop0[C0_STATUS] |= SR_EXL;
    n64_cpu_status_changed(m);
    step(m, ERET);
    CHECK_EQ(m->cpu.pc, sx(DATA + 0x100), "ERET target");
    CHECK(!(m->cpu.cop0[C0_STATUS] & SR_EXL), "ERET clears EXL");
    n64_destroy(m);
}

static void test_interrupts(void)
{
    n64_machine *m = mk();
    /* COUNT / COMPARE: COUNT runs at half the CPU clock. */
    n64_cpu_set_count(m, 0);
    m->cpu.cop0[C0_COMPARE] = 1000;
    n64_cpu_schedule_compare(m);
    m->cpu.cop0[C0_STATUS] = 0x34008001u; /* IM7, IE */
    n64_cpu_status_changed(m);
    const uint32_t loop[] = {NOP, NOP};
    run_prog(m, loop, 2, 1900);
    CHECK(!at_vec(m, 0x80000180u), "no timer interrupt before COUNT reaches COMPARE");
    n64_run_cycles(m, 200);
    CHECK(at_vec(m, 0x80000180u), "timer interrupt taken");
    CHECK_EQ(exc_code(m), EXC_INT, "interrupt code");
    CHECK(m->cpu.cop0[C0_CAUSE] & CAUSE_IP7, "IP7 set");
    CHECK(n64_cpu_count(m) >= 1000 && n64_cpu_count(m) < 1100, "COUNT %u", n64_cpu_count(m));
    /* MTC0 COMPARE clears IP7. */
    m->cpu.gpr[8] = 5;
    step(m, MTC0(8, C0_COMPARE));
    CHECK(!(m->cpu.cop0[C0_CAUSE] & CAUSE_IP7), "writing COMPARE clears IP7");

    /* MI: the VI interrupt through MI_MASK and IP2. */
    m->cpu.cop0[C0_STATUS] = 0x34000401u; /* IM2, IE */
    n64_cpu_status_changed(m);
    n64_bus_write32(m, 0x0430000C, 0x80, ~0u); /* MI_MASK set VI */
    m->vi[3] = 2;                                /* V_INTR: line 1 */
    run_prog(m, loop, 2, 2000000);
    CHECK(at_vec(m, 0x80000180u), "VI interrupt taken");
    CHECK(m->cpu.cop0[C0_CAUSE] & CAUSE_IP2, "IP2 set");
    CHECK(n64_bus_read32(m, 0x04300008) & (1u << N64_MI_VI), "MI_INTR VI");
    n64_bus_write32(m, 0x04400010, 0, ~0u); /* VI_V_CURRENT write clears it */
    CHECK(!(n64_bus_read32(m, 0x04300008) & (1u << N64_MI_VI)), "VI interrupt cleared");
    CHECK(!(m->cpu.cop0[C0_CAUSE] & CAUSE_IP2), "IP2 cleared");
    n64_destroy(m);
}

/* ---- TLB ------------------------------------------------------------------------------------------------------- */

static void tlb_set(n64_machine *m, int index, uint32_t mask, uint64_t hi, uint32_t lo0, uint32_t lo1)
{
    uint64_t *g = m->cpu.gpr;
    g[8] = (uint64_t)index;
    step(m, MTC0(8, C0_INDEX));
    g[8] = mask;
    step(m, MTC0(8, C0_PAGEMASK));
    g[8] = hi;
    step(m, DMTC0(8, C0_ENTRYHI));
    g[8] = lo0;
    step(m, MTC0(8, C0_ENTRYLO0));
    g[8] = lo1;
    step(m, MTC0(8, C0_ENTRYLO1));
    step(m, TLBWI);
}

#define LO(pfn_phys, flags) ((uint32_t)(((pfn_phys) >> 12) << 6) | (flags))
#define LO_G 1u
#define LO_V 2u
#define LO_D 4u
#define LO_C 0x18u

static void test_tlb(void)
{
    n64_machine *m = mk();
    uint64_t *g = m->cpu.gpr;
    n64_wbe32(m->rdram + 0x100010, 0xCAFE0001);
    n64_wbe32(m->rdram + 0x200020, 0xCAFE0002);
    /* 4 KB pages: 0x00400000 -> 0x00100000 (dirty), 0x00401000 -> 0x00200000 (clean). */
    tlb_set(m, 3, 0, 0x00400000, LO(0x100000, LO_C | LO_D | LO_V | LO_G), LO(0x200000, LO_C | LO_V | LO_G));
    g[8] = 0x00400010;
    step(m, LW(9, 8, 0));
    CHECK_EQ(g[9], sx(0xCAFE0001), "TLB even page");
    g[8] = 0x00401020;
    step(m, LW(9, 8, 0));
    CHECK_EQ(g[9], sx(0xCAFE0002), "TLB odd page");

    /* Store to the clean page: TLB modified. */
    g[8] = 0x00401000;
    step(m, SW(9, 8, 0));
    CHECK_EQ(m->cpu.pc, sx(0x80000180u), "TLB mod -> general vector");
    CHECK_EQ(exc_code(m), EXC_MOD, "TLB mod code");
    CHECK_EQ(m->cpu.cop0[C0_BADVADDR], 0x00401000, "TLB mod BadVAddr");

    /* Unmapped: refill at 0x80000000, BadVAddr, Context, EntryHi (ASID kept). */
    m->cpu.cop0[C0_STATUS] &= ~(uint64_t)SR_EXL;
    m->cpu.cop0[C0_ENTRYHI] = 0x2A;
    m->cpu.cop0[C0_CONTEXT] = 0x00800000;
    n64_cpu_status_changed(m);
    g[8] = 0x00402468;
    step(m, LW(9, 8, 0));
    CHECK_EQ(m->cpu.pc, sx(0x80000000u), "TLB refill vector");
    CHECK_EQ(exc_code(m), EXC_TLBL, "TLB refill load code");
    CHECK_EQ(m->cpu.cop0[C0_BADVADDR], 0x00402468, "refill BadVAddr");
    CHECK_EQ(m->cpu.cop0[C0_CONTEXT], 0x00800000 | (0x00402468 >> 13) << 4, "refill Context");
    CHECK_EQ(m->cpu.cop0[C0_ENTRYHI], 0x0040202A, "refill EntryHi");

    /* A refill while EXL is set goes to the general vector. */
    step(m, LW(9, 8, 0));
    CHECK_EQ(m->cpu.pc, sx(0x80000180u), "nested refill -> general vector");

    /* Invalid entry: TLBL at the general vector. */
    m->cpu.cop0[C0_STATUS] &= ~(uint64_t)SR_EXL;
    n64_cpu_status_changed(m);
    tlb_set(m, 4, 0, 0x00600000, LO(0x300000, LO_C | LO_G), LO(0x300000, LO_C | LO_G));
    g[8] = 0x00600000;
    step(m, SW(9, 8, 0));
    CHECK_EQ(m->cpu.pc, sx(0x80000180u), "invalid -> general vector");
    CHECK_EQ(exc_code(m), EXC_TLBS, "invalid store code");

    /* TLBP / TLBR. */
    m->cpu.cop0[C0_STATUS] &= ~(uint64_t)SR_EXL;
    n64_cpu_status_changed(m);
    g[8] = 0x00401000;
    step(m, DMTC0(8, C0_ENTRYHI));
    step(m, TLBP);
    CHECK_EQ(m->cpu.cop0[C0_INDEX], 3, "TLBP finds the entry");
    g[8] = 0x00A00000;
    step(m, DMTC0(8, C0_ENTRYHI));
    step(m, TLBP);
    CHECK_EQ(m->cpu.cop0[C0_INDEX], 0x80000000u, "TLBP miss sets P");
    g[8] = 3;
    step(m, MTC0(8, C0_INDEX));
    step(m, TLBR);
    CHECK_EQ(m->cpu.cop0[C0_ENTRYHI], 0x00400000, "TLBR EntryHi");
    CHECK_EQ(m->cpu.cop0[C0_ENTRYLO1], LO(0x200000, LO_C | LO_V | LO_G), "TLBR EntryLo1");

    /* A 16 KB page pair (PageMask 0x6000): the odd page starts at +16 KB. */
    tlb_set(m, 5, 0x6000, 0x00800000, LO(0x100000, LO_D | LO_V | LO_G), LO(0x200000, LO_D | LO_V | LO_G));
    n64_wbe32(m->rdram + 0x103FFC, 0x16161616);
    n64_wbe32(m->rdram + 0x200020, 0x32323232);
    g[8] = 0x00803FFC;
    step(m, LW(9, 8, 0));
    CHECK_EQ(g[9], 0x16161616, "16 KB even page end");
    g[8] = 0x00804020;
    step(m, LW(9, 8, 0));
    CHECK_EQ(g[9], 0x32323232, "16 KB odd page");

    /* ASID: a non-global entry only matches its ASID. */
    tlb_set(m, 6, 0, 0x00C00000 | 0x07, LO(0x100000, LO_V), LO(0x100000, LO_V));
    m->cpu.cop0[C0_ENTRYHI] = 0x07;
    g[8] = 0x00C00010;
    step(m, LW(9, 8, 0));
    CHECK_EQ(g[9], sx(0xCAFE0001), "ASID match");
    m->cpu.cop0[C0_ENTRYHI] = 0x08;
    step(m, LW(9, 8, 0));
    CHECK_EQ(m->cpu.pc, sx(0x80000000u), "ASID mismatch -> refill");

    /* 64-bit kernel (KX): xkuseg above 4 GB refills at the XTLB vector; xkphys is unmapped. */
    m->cpu.cop0[C0_STATUS] = 0x340000E0u; /* KX SX UX */
    n64_cpu_status_changed(m);
    g[8] = 0x0000000100000000ull;
    step(m, LD(9, 8, 0));
    CHECK_EQ(m->cpu.pc, sx(0x80000080u), "XTLB refill vector");
    CHECK_EQ(m->cpu.cop0[C0_XCONTEXT] & 0x1FFFFFFF0ull, (0x0000000100000000ull >> 13) << 4, "XContext BadVPN2");
    m->cpu.cop0[C0_STATUS] = 0x340000E0u;
    n64_cpu_status_changed(m);
    g[8] = 0x9000000000100010ull;
    step(m, LW(9, 8, 0));
    CHECK_EQ(g[9], sx(0xCAFE0001), "xkphys load");
    g[8] = 0x4000000000000000ull; /* xksseg, unmapped */
    step(m, LW(9, 8, 0));
    CHECK_EQ(m->cpu.pc, sx(0x80000080u), "xksseg miss -> XTLB refill");
    /* 32-bit mode: an address that is not a sign-extended 32-bit value is an address error. */
    m->cpu.cop0[C0_STATUS] = 0x34000000u;
    n64_cpu_status_changed(m);
    g[8] = 0x0000000080100010ull;
    step(m, LW(9, 8, 0));
    CHECK_EQ(exc_code(m), EXC_ADEL, "32-bit mode, address not sign-extended");
    CHECK_EQ(m->cpu.cop0[C0_BADVADDR], 0x0000000080100010ull, "its BadVAddr");
    n64_destroy(m);
}

/* ---- COP1 ------------------------------------------------------------------------------------------------------ */

static void fset_s(n64_machine *m, int r, uint32_t v) { n64_fpu_write_s(&m->cpu, r, v); }
static uint32_t fget_s(n64_machine *m, int r) { return n64_fpu_read_s(&m->cpu, r); }

/* Runs a COP1 instruction; returns the exception code, -1 if none. */
static int fpu(n64_machine *m, uint32_t ins)
{
    m->cpu.cop0[C0_STATUS] &= ~(uint64_t)SR_EXL;
    n64_cpu_status_changed(m);
    step(m, ins);
    if (m->cpu.pc == sx(0x80000180u))
        return (int)exc_code(m);
    return -1;
}

static uint32_t cause(n64_machine *m) { return (m->cpu.fcsr >> FCSR_CAUSE_SHIFT) & 0x3F; }
static uint32_t flags(n64_machine *m) { return (m->cpu.fcsr >> FCSR_FLAGS_SHIFT) & 0x1F; }

static void test_fpu(void)
{
    n64_machine *m = mk();
    fset_s(m, 2, 0x3F800000); /* 1.0 */
    fset_s(m, 4, 0x40000000); /* 2.0 */
    CHECK(fpu(m, FOP(F_S, 4, 2, 6, 0x00)) < 0, "add.s no exception");
    CHECK_EQ(fget_s(m, 6), 0x40400000, "1 + 2");
    CHECK_EQ(cause(m), 0, "exact: no cause");

    fset_s(m, 8, 0x40400000); /* 3.0 */
    CHECK(fpu(m, FOP(F_S, 8, 2, 6, 0x03)) < 0, "div.s");
    CHECK_EQ(fget_s(m, 6), 0x3EAAAAAB, "1 / 3 (nearest)");
    CHECK_EQ(cause(m), FPE_I, "1/3 cause I");
    CHECK_EQ(flags(m), FPE_I, "1/3 flag I");
    m->cpu.fcsr = 1; /* toward zero */
    fpu(m, FOP(F_S, 8, 2, 6, 0x03));
    CHECK_EQ(fget_s(m, 6), 0x3EAAAAAA, "1 / 3 (toward zero)");

    /* Enabled inexact: FPE exception, destination untouched, cause kept, flag not set. */
    m->cpu.fcsr = FPE_I << FCSR_ENABLES_SHIFT;
    fset_s(m, 6, 0x12345678);
    CHECK_EQ(fpu(m, FOP(F_S, 8, 2, 6, 0x03)), EXC_FPE, "enabled inexact traps");
    CHECK_EQ(fget_s(m, 6), 0x12345678, "trapped op writes nothing");
    CHECK_EQ(cause(m), FPE_I, "trap cause");
    CHECK_EQ(flags(m), 0, "trap leaves flags");

    /* Division by zero, invalid (sqrt of -1: the VR4300's default NaN). */
    m->cpu.fcsr = 0;
    fset_s(m, 10, 0);
    fpu(m, FOP(F_S, 10, 2, 6, 0x03));
    CHECK_EQ(fget_s(m, 6), 0x7F800000, "1/0 = +inf");
    CHECK_EQ(cause(m), FPE_Z, "1/0 cause Z");
    fset_s(m, 12, 0xBF800000);
    fpu(m, FOP(F_S, 0, 12, 6, 0x04));
    CHECK_EQ(fget_s(m, 6), 0x7FBFFFFF, "sqrt(-1) = default NaN");
    CHECK_EQ(cause(m), FPE_V, "sqrt(-1) cause V");

    /* Unimplemented operation: denormal operand, signaling NaN, out-of-range conversion. */
    m->cpu.fcsr = 0;
    fset_s(m, 14, 0x00000001);
    CHECK_EQ(fpu(m, FOP(F_S, 2, 14, 6, 0x00)), EXC_FPE, "denormal operand traps");
    CHECK_EQ(cause(m), FPE_E, "denormal cause E");
    m->cpu.fcsr = 0;
    fset_s(m, 14, 0x7FBFFFFF); /* MIPS legacy: top mantissa bit clear = quiet */
    CHECK_EQ(fpu(m, FOP(F_S, 2, 14, 6, 0x00)), EXC_FPE, "qNaN operand: unimplemented");
    m->cpu.fcsr = 0;
    fset_s(m, 14, 0x7FC00000); /* signaling */
    CHECK(fpu(m, FOP(F_S, 2, 14, 6, 0x00)) < 0, "sNaN operand: invalid, not enabled");
    CHECK_EQ(cause(m), FPE_V, "sNaN cause V");
    CHECK_EQ(fget_s(m, 6), 0x7FBFFFFF, "sNaN operand gives the default NaN");
    m->cpu.fcsr = 0;
    fset_s(m, 14, 0x4F32D05E); /* 3e9 */
    CHECK_EQ(fpu(m, FOP(F_S, 0, 14, 6, 0x24)), EXC_FPE, "cvt.w.s out of range traps");
    CHECK_EQ(cause(m), FPE_E, "cvt.w.s range cause E");
    /* Underflow: E unless FS flushes it (to zero, U and I). */
    m->cpu.fcsr = 0;
    fset_s(m, 16, 0x00800000); /* smallest normal */
    fset_s(m, 18, 0x3F000000); /* 0.5 */
    CHECK_EQ(fpu(m, FOP(F_S, 18, 16, 6, 0x02)), EXC_FPE, "denormal result traps");
    m->cpu.fcsr = FCSR_FS;
    CHECK(fpu(m, FOP(F_S, 18, 16, 6, 0x02)) < 0, "FS flushes");
    CHECK_EQ(fget_s(m, 6), 0, "flushed to zero");
    CHECK_EQ(flags(m), FPE_U | FPE_I, "flush flags U I");

    /* Rounding: cvt.w.s by FCSR.RM, and the fixed round / trunc / ceil / floor. */
    static const struct { uint32_t in; int rm; uint32_t fn; int32_t out; } rnd[] = {
        {0x40200000, 0, 0x24, 2},   {0x40200000, 1, 0x24, 2},  {0x40200000, 2, 0x24, 3}, {0x40200000, 3, 0x24, 2},
        {0xC0200000, 3, 0x24, -3},  {0x40600000, 0, 0x24, 4},  {0xC0200000, 0, 0x0C, -2}, {0xC0200000, 0, 0x0D, -2},
        {0xC0200000, 0, 0x0E, -2},  {0xC0200000, 0, 0x0F, -3},
    };
    for (size_t i = 0; i < sizeof rnd / sizeof rnd[0]; i++) {
        m->cpu.fcsr = (uint32_t)rnd[i].rm;
        fset_s(m, 20, rnd[i].in);
        int e = fpu(m, FOP(F_S, 0, 20, 6, rnd[i].fn));
        CHECK(e < 0 && (int32_t)fget_s(m, 6) == rnd[i].out, "convert %08X rm %d fn %02X -> %d (got %d, exc %d)",
              rnd[i].in, rnd[i].rm, rnd[i].fn, rnd[i].out, (int32_t)fget_s(m, 6), e);
    }
    CHECK_EQ(cause(m), FPE_I, "inexact conversion cause I");

    /* Doubles, conversions. */
    m->cpu.fcsr = 0;
    n64_fpu_write_d(&m->cpu, 2, 0x3FF0000000000000ull); /* 1.0 */
    n64_fpu_write_d(&m->cpu, 4, 0x4008000000000000ull); /* 3.0 */
    fpu(m, FOP(F_D, 4, 2, 6, 0x03));
    CHECK_EQ(n64_fpu_read_d(&m->cpu, 6), 0x3FD5555555555555ull, "1/3 double");
    CHECK_EQ(m->cpu.fcsr, 0x1004, "FCSR after div.d");
    fpu(m, FOP(F_D, 0, 6, 8, 0x20));
    CHECK_EQ(fget_s(m, 8), 0x3EAAAAAB, "cvt.s.d");
    n64_fpu_write_d(&m->cpu, 10, (uint64_t)(-123456789012ll));
    fpu(m, FOP(F_L, 0, 10, 12, 0x21));
    CHECK_EQ(n64_fpu_read_d(&m->cpu, 12), 0xC23CBE991A140000ull, "cvt.d.l");
    n64_fpu_write_d(&m->cpu, 10, 1ull << 56);
    CHECK_EQ(fpu(m, FOP(F_L, 0, 10, 12, 0x20)), EXC_FPE, "cvt.s.l beyond 2^55 traps");
    m->cpu.fcsr = 0;
    fset_s(m, 14, (uint32_t)-7);
    fpu(m, FOP(F_W, 0, 14, 12, 0x21));
    CHECK_EQ(n64_fpu_read_d(&m->cpu, 12), 0xC01C000000000000ull, "cvt.d.w");
    CHECK_EQ(fpu(m, FOP(F_S, 0, 2, 6, 0x20)), EXC_FPE, "cvt.s.s is unimplemented");

    /* Compare and branch. */
    m->cpu.fcsr = 0;
    fset_s(m, 2, 0x3F800000);
    fset_s(m, 4, 0x40000000);
    fpu(m, FOP(F_S, 4, 2, 0, 0x3C)); /* c.lt.s f2, f4 */
    CHECK(m->cpu.fcsr & FCSR_C, "c.lt.s 1 < 2");
    fpu(m, FOP(F_S, 2, 4, 0, 0x3C));
    CHECK(!(m->cpu.fcsr & FCSR_C), "c.lt.s 2 < 1 false");
    fset_s(m, 6, 0x7FBFFFFF);
    fpu(m, FOP(F_S, 6, 2, 0, 0x31)); /* c.un.s */
    CHECK((m->cpu.fcsr & FCSR_C) && cause(m) == 0, "c.un.s with a qNaN, no invalid");
    fset_s(m, 7, 0x7FC00000);
    fpu(m, FOP(F_S, 7, 2, 0, 0x31));
    CHECK((m->cpu.fcsr & FCSR_C) && cause(m) == FPE_V, "c.un.s with an sNaN: invalid");
    m->cpu.fcsr = FPE_V << FCSR_ENABLES_SHIFT;
    CHECK_EQ(fpu(m, FOP(F_S, 6, 2, 0, 0x3C)), EXC_FPE, "c.lt.s (signaling) with a qNaN traps when V enabled");

    /* FR = 0: odd registers are the high halves of the even ones. */
    m->cpu.fcsr = 0;
    m->cpu.cop0[C0_STATUS] = 0x30000000u;
    n64_cpu_status_changed(m);
    m->cpu.gpr[8] = 0x11111111;
    m->cpu.gpr[9] = 0x22222222;
    step(m, 0x44880800u); /* mtc1 t0, f1 */
    step(m, 0x44890000u); /* mtc1 t1, f0 */
    step(m, 0x442A0000u); /* dmfc1 t2, f0 */
    CHECK_EQ(m->cpu.gpr[10], 0x1111111122222222ull, "FR=0 pair view");
    step(m, 0x442A0800u); /* dmfc1 t2, f1: the even register */
    CHECK_EQ(m->cpu.gpr[10], 0x1111111122222222ull, "FR=0 odd double reads the pair");
    m->cpu.cop0[C0_STATUS] = 0x34000000u;
    n64_cpu_status_changed(m);
    step(m, 0x442A0800u);
    CHECK_EQ(m->cpu.gpr[10], 0, "FR=1: f1 is its own register");

    /* CTC1 with an enabled cause pending traps at once. */
    m->cpu.gpr[8] = (FPE_Z << FCSR_CAUSE_SHIFT) | (FPE_Z << FCSR_ENABLES_SHIFT);
    CHECK_EQ(fpu(m, 0x44C8F800u), EXC_FPE, "ctc1 with an enabled cause traps"); /* ctc1 t0, $31 */
    n64_destroy(m);
}

int main(void)
{
    test_integer();
    test_branches();
    test_exceptions();
    test_interrupts();
    test_tlb();
    test_fpu();
    printf("n64_cpu: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
