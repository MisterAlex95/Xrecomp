/* PS1 core unit tests (XBA-162 / XBA-167): R3000A delay slots, load delays and their cancellation, unaligned
 * loads / stores, exceptions, COP0, interrupts, cache isolation, the DMA OTC channel, the root counters and the SIO0
 * pad / memory card protocols. Hand-assembled programs run from RAM; the "BIOS" is 512 KB of our own (a branch to
 * itself at the reset vector), no BIOS image needed. */
#include "ps1_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

/* ---- A tiny assembler ---- */
enum { ZERO, AT, V0, V1, A0, A1, A2, A3, T0, T1, T2, T3, T4, T5, T6, T7, S0, S1, S2, S3, S4, S5, S6, S7, T8, T9,
       K0, K1, GP, SP, FP, RA };

static uint32_t R(int fn, int rs, int rt, int rd, int sh) { return (uint32_t)(rs << 21 | rt << 16 | rd << 11 | sh << 6 | fn); }
static uint32_t I(int op, int rs, int rt, int imm) { return (uint32_t)op << 26 | (uint32_t)(rs << 21 | rt << 16 | (imm & 0xFFFF)); }
#define NOP 0u
#define ADDU(d, s, t) R(0x21, s, t, d, 0)
#define ADD(d, s, t) R(0x20, s, t, d, 0)
#define ADDIU(t, s, i) I(0x09, s, t, i)
#define ORI(t, s, i) I(0x0D, s, t, i)
#define LUI(t, i) I(0x0F, 0, t, i)
#define LW(t, o, b) I(0x23, b, t, o)
#define LWL(t, o, b) I(0x22, b, t, o)
#define LWR(t, o, b) I(0x26, b, t, o)
#define SW(t, o, b) I(0x2B, b, t, o)
#define SWL(t, o, b) I(0x2A, b, t, o)
#define SWR(t, o, b) I(0x2E, b, t, o)
#define BEQ(s, t, o) I(0x04, s, t, o)
#define BNE(s, t, o) I(0x05, s, t, o)
#define BLTZAL(s, o) I(0x01, s, 0x10, o)
#define JR(s) R(0x08, s, 0, 0, 0)
#define JALR(d, s) R(0x09, s, 0, d, 0)
#define SYSCALL R(0x0C, 0, 0, 0, 0)
#define BREAK R(0x0D, 0, 0, 0, 0)
#define DIV(s, t) R(0x1A, s, t, 0, 0)
#define DIVU(s, t) R(0x1B, s, t, 0, 0)
#define MFHI(d) R(0x10, 0, 0, d, 0)
#define MFLO(d) R(0x12, 0, 0, d, 0)
#define MFC0(t, d) (0x40000000u | (uint32_t)(t) << 16 | (uint32_t)(d) << 11)
#define MTC0(t, d) (0x40800000u | (uint32_t)(t) << 16 | (uint32_t)(d) << 11)
#define RFE 0x42000010u
#define LOOP 0x1000FFFFu /* b . (with a nop in its slot) */

#define PROG 0x80010000u /* where test programs go */
#define DATA 0x80020000u

static uint8_t *g_bios;

static ps1_machine *machine(void)
{
    char err[128];
    ps1_machine *m = ps1_create(g_bios, PS1_BIOS_SIZE, NULL, NULL, 0, err, sizeof err);
    if (!m) {
        printf("FAIL: ps1_create: %s\n", err);
        exit(1);
    }
    return m;
}

static void load(ps1_machine *m, uint32_t addr, const uint32_t *code, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ps1_poke32(m, addr + 4 * (uint32_t)i, code[i]);
}

/* Starts the program at PROG with interrupts off (kernel mode, BEV = 0: exceptions go to 0x80000080). */
static void start(ps1_machine *m, const uint32_t *code, size_t n)
{
    load(m, PROG, code, n);
    m->pc = PROG;
    m->next_pc = PROG + 4;
    m->next_delay = false;
    m->ld_reg = 0;
    m->cop0[C0_SR] = 0;
    ps1_cpu_update_irq(m);
    /* Exception vector: a loop, so a test can see where it went. */
    ps1_poke32(m, 0x80000080u, LOOP);
    ps1_poke32(m, 0x80000084u, NOP);
}

/* Runs exactly one instruction (or one interrupt entry). */
static void step(ps1_machine *m)
{
    m->next_event = m->cycles + 1;
    ps1_cpu_run(m);
}

static void steps(ps1_machine *m, int n)
{
    while (n--)
        step(m);
}

static uint32_t exc_code(ps1_machine *m)
{
    return (m->cop0[C0_CAUSE] >> 2) & 31;
}

/* ---------------------------------------------------------------------------------------------------------------- */

static void test_load_delay(void)
{
    ps1_machine *m = machine();
    ps1_poke32(m, DATA, 0x11223344u);
    ps1_poke32(m, DATA + 4, 0x55667788u);
    uint32_t code[] = {
        LUI(T1, 0x8002),     /* t1 = DATA */
        ORI(T0, ZERO, 0x55), /* t0 = 0x55 */
        LW(T0, 0, T1),       /* load delay */
        ADDU(T2, T0, ZERO),  /* sees the old t0 */
        ADDU(T3, T0, ZERO),  /* sees the loaded value */
        LOOP, NOP,
    };
    start(m, code, sizeof code / 4);
    steps(m, 5);
    CHECK(m->r[T2] == 0x55, "load delay: t2 = %08X", m->r[T2]);
    CHECK(m->r[T3] == 0x11223344u, "after the delay: t3 = %08X", m->r[T3]);
    ps1_destroy(m);

    /* A plain write in the delay slot cancels the load. */
    m = machine();
    ps1_poke32(m, DATA, 0x11223344u);
    uint32_t code2[] = {
        LUI(T1, 0x8002),
        LW(T0, 0, T1),
        ORI(T0, ZERO, 5), /* wins over the pending load */
        ADDU(T2, T0, ZERO),
        LOOP, NOP,
    };
    start(m, code2, sizeof code2 / 4);
    steps(m, 4);
    CHECK(m->r[T0] == 5 && m->r[T2] == 5, "load cancelled by a write: t0 = %08X t2 = %08X", m->r[T0], m->r[T2]);
    ps1_destroy(m);

    /* Two loads to the same register: the first never lands. */
    m = machine();
    ps1_poke32(m, DATA, 0x11223344u);
    ps1_poke32(m, DATA + 4, 0x55667788u);
    uint32_t code3[] = {
        LUI(T1, 0x8002),
        ORI(T0, ZERO, 7),
        LW(T0, 0, T1),
        LW(T0, 4, T1),
        ADDU(T2, T0, ZERO), /* neither load has landed: 7 */
        ADDU(T3, T0, ZERO), /* the second one */
        LOOP, NOP,
    };
    start(m, code3, sizeof code3 / 4);
    steps(m, 6);
    CHECK(m->r[T2] == 7, "back-to-back loads: t2 = %08X", m->r[T2]);
    CHECK(m->r[T3] == 0x55667788u, "back-to-back loads: t3 = %08X", m->r[T3]);
    ps1_destroy(m);

    /* MFC0 is delayed like a load. */
    m = machine();
    uint32_t code4[] = {
        ORI(T0, ZERO, 1),
        MFC0(T0, C0_PRID),
        ADDU(T2, T0, ZERO),
        ADDU(T3, T0, ZERO),
        LOOP, NOP,
    };
    start(m, code4, sizeof code4 / 4);
    steps(m, 4);
    CHECK(m->r[T2] == 1 && m->r[T3] == 2, "MFC0 delay: t2 = %08X t3 = %08X", m->r[T2], m->r[T3]);
    ps1_destroy(m);
}

static void test_unaligned(void)
{
    ps1_machine *m = machine();
    ps1_poke32(m, DATA, 0x33221100u);
    ps1_poke32(m, DATA + 4, 0x77665544u);
    uint32_t code[] = {
        LUI(T1, 0x8002),
        LWR(T0, 1, T1),
        LWL(T0, 4, T1), /* sees the LWR result through the pending load */
        NOP,
        LUI(T2, 0xDDCC),
        ORI(T2, T2, 0xBBAA),
        SWR(T2, 9, T1), /* DATA + 8.. */
        SWL(T2, 12, T1),
        LOOP, NOP,
    };
    start(m, code, sizeof code / 4);
    steps(m, 8);
    CHECK(m->r[T0] == 0x44332211u, "LWR/LWL pair: %08X", m->r[T0]);
    CHECK(ps1_peek32(m, DATA + 8) == 0xCCBBAA00u, "SWR: %08X", ps1_peek32(m, DATA + 8));
    CHECK(ps1_peek32(m, DATA + 12) == 0x000000DDu, "SWL: %08X", ps1_peek32(m, DATA + 12));
    ps1_destroy(m);
}

static void test_branches(void)
{
    ps1_machine *m = machine();
    uint32_t code[] = {
        BEQ(ZERO, ZERO, 2),  /* to +3 */
        ORI(T0, ZERO, 1),    /* delay slot runs */
        ORI(T1, ZERO, 1),    /* skipped */
        ORI(T2, ZERO, 1),    /* target */
        ORI(T3, ZERO, 1),
        BLTZAL(T3, 4),       /* not taken, links anyway */
        NOP,
        LUI(T4, 0x8001),
        ORI(T4, T4, 0x0040), /* PROG + 0x40 */
        JALR(RA, T4),
        ORI(T5, ZERO, 9),    /* delay slot */
        NOP, NOP, NOP, NOP, NOP,
        LOOP, NOP, /* PROG + 0x40 */
    };
    start(m, code, sizeof code / 4);
    steps(m, 2);
    CHECK(m->r[T0] == 1, "branch delay slot ran");
    step(m);
    CHECK(m->pc == PROG + 16 && m->r[T1] == 0 && m->r[T2] == 1, "branch target: pc %08X", m->pc);
    steps(m, 3);
    CHECK(m->r[RA] == PROG + 7 * 4, "BLTZAL links when not taken: ra = %08X", m->r[RA]);
    steps(m, 4);
    CHECK(m->r[RA] == PROG + 11 * 4 && m->r[T5] == 9 && m->pc == PROG + 0x40, "JALR: ra = %08X pc = %08X",
          m->r[RA], m->pc);
    ps1_destroy(m);
}

static void test_divide(void)
{
    ps1_machine *m = machine();
    uint32_t code[] = {
        ORI(T0, ZERO, 5), DIV(T0, ZERO), MFLO(T1), MFHI(T2),
        ADDIU(T0, ZERO, -5), DIV(T0, ZERO), MFLO(T3), MFHI(T4),
        LUI(T0, 0x8000), ADDIU(T5, ZERO, -1), DIV(T0, T5), MFLO(T6), MFHI(T7),
        ORI(T0, ZERO, 5), DIVU(T0, ZERO), MFLO(S0), MFHI(S1),
        LOOP, NOP,
    };
    start(m, code, sizeof code / 4);
    steps(m, 17);
    CHECK(m->r[T1] == 0xFFFFFFFFu && m->r[T2] == 5, "5 / 0: lo %08X hi %08X", m->r[T1], m->r[T2]);
    CHECK(m->r[T3] == 1 && m->r[T4] == 0xFFFFFFFBu, "-5 / 0: lo %08X hi %08X", m->r[T3], m->r[T4]);
    CHECK(m->r[T6] == 0x80000000u && m->r[T7] == 0, "overflow: lo %08X hi %08X", m->r[T6], m->r[T7]);
    CHECK(m->r[S0] == 0xFFFFFFFFu && m->r[S1] == 5, "5 /u 0: lo %08X hi %08X", m->r[S0], m->r[S1]);
    ps1_destroy(m);
}

static void test_exceptions(void)
{
    /* Overflow: rd unchanged, EPC = the ADD, code 12. */
    ps1_machine *m = machine();
    uint32_t code[] = {
        LUI(T0, 0x7FFF), ORI(T0, T0, 0xFFFF), ORI(T1, ZERO, 1), ORI(T2, ZERO, 0x77),
        ADD(T2, T0, T1),
        LOOP, NOP,
    };
    start(m, code, sizeof code / 4);
    steps(m, 5);
    CHECK(exc_code(m) == EXC_OV && m->cop0[C0_EPC] == PROG + 16 && m->r[T2] == 0x77 && m->pc == 0x80000080u,
          "overflow: code %u epc %08X t2 %08X pc %08X", exc_code(m), m->cop0[C0_EPC], m->r[T2], m->pc);
    ps1_destroy(m);

    /* Address error on load: BadVaddr. */
    m = machine();
    uint32_t code2[] = {LUI(T1, 0x8002), LW(T0, 2, T1), LOOP, NOP};
    start(m, code2, sizeof code2 / 4);
    steps(m, 2);
    CHECK(exc_code(m) == EXC_ADEL && m->cop0[C0_BADVADDR] == DATA + 2, "AdEL: code %u badv %08X", exc_code(m),
          m->cop0[C0_BADVADDR]);
    ps1_destroy(m);

    /* Bus error on an unmapped load. */
    m = machine();
    uint32_t code3[] = {LUI(T1, 0x1FE0), LW(T0, 0, T1), LOOP, NOP};
    start(m, code3, sizeof code3 / 4);
    steps(m, 2);
    CHECK(exc_code(m) == EXC_DBE, "DBE: code %u", exc_code(m));
    ps1_destroy(m);

    /* SYSCALL in a branch delay slot: BD, EPC = the branch; then RFE pops the mode stack. */
    m = machine();
    uint32_t code4[] = {BEQ(ZERO, ZERO, 3), SYSCALL, NOP, NOP, NOP, LOOP, NOP};
    start(m, code4, sizeof code4 / 4);
    m->cop0[C0_SR] = 0x0000000Du; /* KUo/IEo pattern to see the push */
    steps(m, 2);
    CHECK(exc_code(m) == EXC_SYS && (m->cop0[C0_CAUSE] & 0x80000000u) && (m->cop0[C0_CAUSE] & 0x40000000u) &&
              m->cop0[C0_EPC] == PROG,
          "syscall in delay slot: cause %08X epc %08X", m->cop0[C0_CAUSE], m->cop0[C0_EPC]);
    CHECK((m->cop0[C0_SR] & 0x3F) == 0x34, "mode stack push: SR %08X", m->cop0[C0_SR]);
    ps1_poke32(m, 0x80000080u, RFE);
    m->pc = 0x80000080u;
    m->next_pc = m->pc + 4;
    step(m);
    CHECK((m->cop0[C0_SR] & 0x3F) == 0x3D, "RFE: SR %08X", m->cop0[C0_SR]);
    ps1_destroy(m);

    /* BREAK, reserved instruction, coprocessor unusable (COP2 with CU2 clear). */
    m = machine();
    uint32_t code5[] = {BREAK, LOOP, NOP};
    start(m, code5, sizeof code5 / 4);
    step(m);
    CHECK(exc_code(m) == EXC_BP, "break: code %u", exc_code(m));
    ps1_destroy(m);
    m = machine();
    uint32_t code6[] = {0xFC000000u, LOOP, NOP};
    start(m, code6, sizeof code6 / 4);
    step(m);
    CHECK(exc_code(m) == EXC_RI, "reserved: code %u", exc_code(m));
    ps1_destroy(m);
    m = machine();
    uint32_t code7[] = {0x4A000001u /* RTPS */, LOOP, NOP};
    start(m, code7, sizeof code7 / 4);
    step(m);
    CHECK(exc_code(m) == EXC_CPU && ((m->cop0[C0_CAUSE] >> 28) & 3) == 2, "CpU: cause %08X", m->cop0[C0_CAUSE]);
    ps1_destroy(m);
}

static void test_interrupt(void)
{
    ps1_machine *m = machine();
    uint32_t code[] = {NOP, NOP, NOP, LOOP, NOP};
    start(m, code, sizeof code / 4);
    m->cop0[C0_SR] = 0x00000401u; /* IEc, IM2 */
    ps1_io_write(m, 0x1F801074u, 1u << PS1_IRQ_VBLANK, 4);
    ps1_cpu_update_irq(m);
    step(m);
    CHECK(m->pc == PROG + 4, "no interrupt yet");
    ps1_irq_raise(m, PS1_IRQ_VBLANK);
    step(m); /* the interrupt entry */
    CHECK(m->pc == 0x80000080u && exc_code(m) == EXC_INT && m->cop0[C0_EPC] == PROG + 4,
          "interrupt: pc %08X code %u epc %08X", m->pc, exc_code(m), m->cop0[C0_EPC]);
    CHECK(!(m->cop0[C0_SR] & 1), "IEc cleared on entry");
    CHECK(ps1_io_read(m, 0x1F801070u, 4) & 1, "I_STAT bit 0");
    ps1_io_write(m, 0x1F801070u, ~1u, 4);
    CHECK(!(ps1_io_read(m, 0x1F801070u, 4) & 1) && !(m->cop0[C0_CAUSE] & 0x400), "I_STAT acknowledged");
    ps1_destroy(m);
}

static void test_cache_isolation(void)
{
    ps1_machine *m = machine();
    ps1_poke32(m, DATA, 0x12345678u);
    uint32_t code[] = {LUI(T1, 0x8002), SW(ZERO, 0, T1), LOOP, NOP};
    start(m, code, sizeof code / 4);
    m->cop0[C0_SR] = SR_ISC;
    steps(m, 2);
    CHECK(ps1_peek32(m, DATA) == 0x12345678u, "store with IsC reached RAM: %08X", ps1_peek32(m, DATA));
    ps1_destroy(m);
}

static void test_mirrors(void)
{
    ps1_machine *m = machine();
    uint32_t v = 0;
    ps1_write32(m, 0x80001000u, 0xCAFEF00Du);
    ps1_read32(m, 0xA0601000u, &v); /* KSEG1, fourth RAM mirror */
    CHECK(v == 0xCAFEF00Du, "RAM mirror: %08X", v);
    ps1_read32(m, 0x00201000u, &v);
    CHECK(v == 0xCAFEF00Du, "KUSEG mirror: %08X", v);
    ps1_write32(m, 0x1F800010u, 0xABCD1234u);
    ps1_read32(m, 0x9F800010u, &v);
    CHECK(v == 0xABCD1234u, "scratchpad: %08X", v);
    CHECK(ps1_read32(m, 0xBF800010u, &v) == EXC_DBE, "scratchpad not in KSEG1");
    ps1_read32(m, 0x9FC00000u, &v);
    CHECK(v == ps1_peek32(m, 0xBFC00000u), "BIOS mirror");
    ps1_destroy(m);
}

static void test_dma_otc(void)
{
    ps1_machine *m = machine();
    ps1_io_write(m, 0x1F8010F0u, 0x08000000u, 4); /* DPCR: channel 6 on */
    ps1_io_write(m, 0x1F8010E0u, 0x00100010u, 4); /* MADR = 0x100010 */
    ps1_io_write(m, 0x1F8010E4u, 4, 4);
    ps1_io_write(m, 0x1F8010E8u, 0x11000002u, 4);
    CHECK(ps1_peek32(m, 0x80100010u) == 0x0010000Cu && ps1_peek32(m, 0x80100008u) == 0x00100004u &&
              ps1_peek32(m, 0x80100004u) == 0x00FFFFFFu,
          "OTC: %08X %08X %08X", ps1_peek32(m, 0x80100010u), ps1_peek32(m, 0x80100008u), ps1_peek32(m, 0x80100004u));
    CHECK(!(ps1_io_read(m, 0x1F8010E8u, 4) & 0x01000000u), "OTC done");
    ps1_destroy(m);
}

static void test_timers(void)
{
    ps1_machine *m = machine();
    m->cop0[C0_SR] = 0;
    ps1_io_write(m, 0x1F801074u, 0x40, 4);        /* I_MASK: timer 2 */
    ps1_io_write(m, 0x1F801128u, 100, 4);         /* target */
    ps1_io_write(m, 0x1F801124u, 0x0058, 4);      /* reset at target, IRQ at target, repeat, sysclk */
    uint64_t t0 = m->cycles;
    ps1_run_cycles(m, 50);
    uint32_t c = ps1_io_read(m, 0x1F801120u, 4);
    CHECK(c >= 45 && c <= 70, "timer 2 counting: %u after %llu cycles", c, (unsigned long long)(m->cycles - t0));
    ps1_run_cycles(m, 1000);
    CHECK(ps1_io_read(m, 0x1F801070u, 4) & 0x40, "timer 2 target IRQ");
    uint32_t mode = ps1_io_read(m, 0x1F801124u, 4);
    CHECK(mode & 0x800, "reached-target flag: %04X", mode);
    CHECK(!(ps1_io_read(m, 0x1F801124u, 4) & 0x800), "flag cleared by the read");
    CHECK(ps1_io_read(m, 0x1F801120u, 4) <= 100, "reset at target");
    ps1_destroy(m);

    /* Timer 1 on hblank: one tick per scanline. */
    m = machine();
    ps1_run_frame(m);
    ps1_io_write(m, 0x1F801114u, 0x0100, 4);
    ps1_run_frame(m);
    c = ps1_io_read(m, 0x1F801110u, 4);
    CHECK(c >= 262 && c <= 264, "timer 1 hblanks per frame: %u", c);
    ps1_destroy(m);
}

/* One SIO0 byte exchange through the registers. */
static uint8_t sio_xfer(ps1_machine *m, uint8_t tx, bool *ack)
{
    ps1_io_write(m, 0x1F801040u, tx, 1);
    ps1_run_cycles(m, 2000);
    *ack = (ps1_io_read(m, 0x1F801044u, 4) & 0x200) != 0;
    ps1_io_write(m, 0x1F80104Au, ps1_io_read(m, 0x1F80104Au, 2) | 0x10, 2); /* acknowledge */
    return (uint8_t)ps1_io_read(m, 0x1F801040u, 1);
}

static void test_sio(void)
{
    ps1_machine *m = machine();
    bool ack;
    ps1_set_pad(m, 0, PS1_PAD_DIGITAL, PS1_PAD_CROSS | PS1_PAD_UP, 0x80, 0x80, 0x80, 0x80);
    ps1_io_write(m, 0x1F80104Au, 0x1003, 2); /* TX enable, /JOY1, ACK IRQ */
    uint8_t r0 = sio_xfer(m, 0x01, &ack);
    CHECK(r0 == 0xFF && ack, "pad select: %02X ack %d", r0, ack);
    uint8_t id = sio_xfer(m, 0x42, &ack);
    uint8_t z = sio_xfer(m, 0x00, &ack);
    uint8_t lo = sio_xfer(m, 0x00, &ack);
    uint8_t hi = sio_xfer(m, 0x00, &ack);
    CHECK(id == 0x41 && z == 0x5A, "pad id %02X %02X", id, z);
    CHECK(lo == 0xEF && hi == 0xBF && !ack, "buttons %02X %02X (last ack %d)", lo, hi, ack);
    CHECK(ps1_io_read(m, 0x1F801070u, 4) & (1u << PS1_IRQ_SIO0), "SIO0 IRQ");
    ps1_io_write(m, 0x1F80104Au, 0, 2);

    /* Port 2 is empty. */
    ps1_io_write(m, 0x1F80104Au, 0x3003, 2);
    r0 = sio_xfer(m, 0x01, &ack);
    CHECK(r0 == 0xFF && !ack, "empty port: %02X ack %d", r0, ack);
    ps1_io_write(m, 0x1F80104Au, 0, 2);

    /* Memory card: write sector 0x123, read it back. */
    uint8_t data[128], chk = 0x01 ^ 0x23;
    for (int i = 0; i < 128; i++) {
        data[i] = (uint8_t)(i * 7 + 3);
        chk ^= data[i];
    }
    ps1_io_write(m, 0x1F80104Au, 0x1003, 2);
    sio_xfer(m, 0x81, &ack);
    CHECK(ack, "card select");
    uint8_t flag = sio_xfer(m, 'W', &ack);
    CHECK(flag == 0x08, "fresh card flag %02X", flag);
    sio_xfer(m, 0, &ack);
    sio_xfer(m, 0, &ack);
    sio_xfer(m, 0x01, &ack);
    sio_xfer(m, 0x23, &ack);
    for (int i = 0; i < 128; i++)
        sio_xfer(m, data[i], &ack);
    sio_xfer(m, chk, &ack);
    uint8_t a1 = sio_xfer(m, 0, &ack), a2 = sio_xfer(m, 0, &ack), end = sio_xfer(m, 0, &ack);
    CHECK(a1 == 0x5C && a2 == 0x5D && end == 0x47 && !ack, "card write: %02X %02X %02X", a1, a2, end);
    ps1_io_write(m, 0x1F80104Au, 0, 2);
    CHECK(ps1_card_take_dirty(m, 0) && !ps1_card_take_dirty(m, 0), "card dirty flag");
    size_t sz = 0;
    const uint8_t *card = ps1_card_data(m, 0, &sz);
    CHECK(sz == PS1_CARD_SIZE && !memcmp(card + 0x123 * 128, data, 128), "card data written");

    ps1_io_write(m, 0x1F80104Au, 0x1003, 2);
    sio_xfer(m, 0x81, &ack);
    flag = sio_xfer(m, 'R', &ack);
    CHECK(flag == 0x00, "flag after a write %02X", flag);
    sio_xfer(m, 0, &ack);
    sio_xfer(m, 0, &ack);
    sio_xfer(m, 0x01, &ack);
    sio_xfer(m, 0x23, &ack);
    a1 = sio_xfer(m, 0, &ack);
    a2 = sio_xfer(m, 0, &ack);
    uint8_t msb = sio_xfer(m, 0, &ack), lsb = sio_xfer(m, 0, &ack);
    CHECK(a1 == 0x5C && a2 == 0x5D && msb == 0x01 && lsb == 0x23, "card read header %02X %02X %02X %02X", a1, a2,
          msb, lsb);
    int same = 1;
    for (int i = 0; i < 128; i++)
        same &= sio_xfer(m, 0, &ack) == data[i];
    uint8_t rchk = sio_xfer(m, 0, &ack);
    end = sio_xfer(m, 0, &ack);
    CHECK(same && rchk == chk && end == 0x47 && !ack, "card read: same %d chk %02X/%02X end %02X", same, rchk, chk,
          end);
    ps1_io_write(m, 0x1F80104Au, 0, 2);

    /* A formatted blank card. */
    CHECK(card[0] == 'M' && card[1] == 'C' && card[0x7F] == ('M' ^ 'C') && card[128] == 0xA0, "card format");
    ps1_destroy(m);
}

int main(void)
{
    g_bios = calloc(1, PS1_BIOS_SIZE);
    if (!g_bios)
        return 1;
    /* Reset vector: b . ; nop */
    g_bios[0] = 0xFF;
    g_bios[1] = 0xFF;
    g_bios[2] = 0x00;
    g_bios[3] = 0x10;

    test_load_delay();
    test_unaligned();
    test_branches();
    test_divide();
    test_exceptions();
    test_interrupt();
    test_cache_isolation();
    test_mirrors();
    test_dma_otc();
    test_timers();
    test_sio();
    free(g_bios);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ps1 cpu: ok\n");
    return 0;
}
