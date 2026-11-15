/* Unit tests of the GBA core's tricky corners (XBA-152 / XBA-153): barrel shifter carries, PC + 12, misaligned
 * loads, LDM / STM edge cases, banked registers, multiplies, swaps, Thumb BL, exception return, HLE SWIs, DMA,
 * timers. Each case assembles a few instructions (hand-encoded) into IWRAM, runs them with gba_cpu_step and checks
 * the registers. Our own code. */
#include "gba_internal.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            failures++;                                                                                        \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                                        \
            printf(__VA_ARGS__);                                                                               \
            printf("\n");                                                                                      \
        }                                                                                                      \
    } while (0)

#define CODE 0x03001000u
#define DATA 0x03002000u

static gba_machine *machine(void)
{
    static const uint8_t rom[256] = {0};
    gba_machine *m = gba_create(rom, sizeof rom, NULL, 0);
    if (!m) {
        printf("FAIL: gba_create\n");
        failures++;
    }
    return m;
}

static void arm(gba_machine *m, const uint32_t *ops, int n)
{
    for (int i = 0; i < n; i++)
        wr32(&m->iwram[(CODE & 0x7FFF) + 4 * (unsigned)i], ops[i]);
    gba_cpu_jump(m, CODE, false);
    for (int i = 0; i < n; i++)
        gba_cpu_step(m);
}

static void thumb(gba_machine *m, const uint16_t *ops, int n, int steps)
{
    for (int i = 0; i < n; i++)
        wr16(&m->iwram[(CODE & 0x7FFF) + 2 * (unsigned)i], ops[i]);
    gba_cpu_jump(m, CODE, true);
    for (int i = 0; i < steps; i++)
        gba_cpu_step(m);
}

static void test_shifter(void)
{
    gba_machine *m = machine();
    m->cpu.c = 1;
    m->cpu.r[1] = 5;
    arm(m, (uint32_t[]){0xE1B00001}, 1); /* movs r0, r1, lsl #0 */
    CHECK(m->cpu.r[0] == 5 && m->cpu.c == 1, "lsl #0 keeps C: r0=%x c=%u", m->cpu.r[0], m->cpu.c);

    m->cpu.c = 0;
    m->cpu.r[1] = 0x80000000u;
    m->cpu.r[2] = 32;
    arm(m, (uint32_t[]){0xE1B00231}, 1); /* movs r0, r1, lsr r2 (32) */
    CHECK(m->cpu.r[0] == 0 && m->cpu.c == 1 && m->cpu.z == 1, "lsr by 32: r0=%x c=%u", m->cpu.r[0], m->cpu.c);

    m->cpu.r[2] = 33;
    m->cpu.c = 1;
    arm(m, (uint32_t[]){0xE1B00231}, 1); /* lsr by 33: 0, C = 0 */
    CHECK(m->cpu.r[0] == 0 && m->cpu.c == 0, "lsr by 33: c=%u", m->cpu.c);

    m->cpu.r[1] = 0x80000001u;
    m->cpu.r[2] = 32;
    m->cpu.c = 0;
    arm(m, (uint32_t[]){0xE1B00271}, 1); /* movs r0, r1, ror r2 (32): unchanged, C = bit 31 */
    CHECK(m->cpu.r[0] == 0x80000001u && m->cpu.c == 1, "ror by 32: r0=%x c=%u", m->cpu.r[0], m->cpu.c);

    m->cpu.r[1] = 3;
    m->cpu.c = 1;
    arm(m, (uint32_t[]){0xE1B00061}, 1); /* movs r0, r1, rrx */
    CHECK(m->cpu.r[0] == 0x80000001u && m->cpu.c == 1, "rrx: r0=%x c=%u", m->cpu.r[0], m->cpu.c);

    m->cpu.r[1] = 0x80000000u;
    m->cpu.r[2] = 40;
    arm(m, (uint32_t[]){0xE1B00251}, 1); /* movs r0, r1, asr r2 (40): sign fill, C = bit 31 */
    CHECK(m->cpu.r[0] == 0xFFFFFFFFu && m->cpu.c == 1, "asr by 40: r0=%x c=%u", m->cpu.r[0], m->cpu.c);

    arm(m, (uint32_t[]){0xE3B004FF}, 1); /* movs r0, #0xFF000000: rotated immediate, C = bit 31 */
    CHECK(m->cpu.r[0] == 0xFF000000u && m->cpu.c == 1 && m->cpu.n == 1, "imm rot: r0=%x", m->cpu.r[0]);
    gba_destroy(m);
}

static void test_pc_reads(void)
{
    gba_machine *m = machine();
    m->cpu.r[1] = 0;
    m->cpu.r[2] = 0;
    arm(m, (uint32_t[]){0xE08F0211, 0xE28F1000}, 2); /* add r0, pc, r1, lsl r2 ; add r1, pc, #0 */
    CHECK(m->cpu.r[0] == CODE + 12, "register shift sees PC + 12: %x", m->cpu.r[0]);
    CHECK(m->cpu.r[1] == CODE + 4 + 8, "PC + 8: %x", m->cpu.r[1]);

    m->cpu.r[1] = DATA;
    arm(m, (uint32_t[]){0xE581F000}, 1); /* str pc, [r1] */
    CHECK(gba_peek32(m, DATA) == CODE + 12, "str pc stores + 12: %x", gba_peek32(m, DATA));
    gba_destroy(m);
}

static void test_misaligned(void)
{
    gba_machine *m = machine();
    wr32(&m->iwram[DATA & 0x7FFF], 0x1122B344);
    m->cpu.r[1] = DATA + 1;
    arm(m, (uint32_t[]){0xE5910000}, 1); /* ldr r0, [r1] */
    CHECK(m->cpu.r[0] == 0x441122B3u, "ldr rotates: %x", m->cpu.r[0]);
    arm(m, (uint32_t[]){0xE1D100B0}, 1); /* ldrh r0, [r1] */
    CHECK(m->cpu.r[0] == 0x440000B3u, "ldrh rotates: %x", m->cpu.r[0]);
    arm(m, (uint32_t[]){0xE1D100F0}, 1); /* ldrsh r0, [r1] at an odd address = ldrsb */
    CHECK(m->cpu.r[0] == 0xFFFFFFB3u, "ldrsh odd: %x", m->cpu.r[0]);
    m->cpu.r[1] = DATA;
    arm(m, (uint32_t[]){0xE1D100F0}, 1);
    CHECK(m->cpu.r[0] == 0xFFFFB344u, "ldrsh: %x", m->cpu.r[0]);
    gba_destroy(m);
}

static void test_block_transfer(void)
{
    gba_machine *m = machine();
    m->cpu.r[0] = 0xAAAA;
    m->cpu.r[1] = DATA;
    arm(m, (uint32_t[]){0xE8A10003}, 1); /* stmia r1!, {r0, r1}: r1 not first -> new base stored */
    CHECK(gba_peek32(m, DATA) == 0xAAAA && gba_peek32(m, DATA + 4) == DATA + 8 && m->cpu.r[1] == DATA + 8,
          "stm base not first: %x %x", gba_peek32(m, DATA + 4), m->cpu.r[1]);

    m->cpu.r[0] = DATA;
    m->cpu.r[1] = 0xBBBB;
    arm(m, (uint32_t[]){0xE8A00003}, 1); /* stmia r0!, {r0, r1}: r0 first -> old base stored */
    CHECK(gba_peek32(m, DATA) == DATA && m->cpu.r[0] == DATA + 8, "stm base first: %x", gba_peek32(m, DATA));

    wr32(&m->iwram[DATA & 0x7FFF], 0x1234);
    m->cpu.r[0] = DATA;
    arm(m, (uint32_t[]){0xE8B00001}, 1); /* ldmia r0!, {r0}: loaded value wins */
    CHECK(m->cpu.r[0] == 0x1234, "ldm base in list: %x", m->cpu.r[0]);

    wr32(&m->iwram[DATA & 0x7FFF], CODE + 0x100);
    m->cpu.r[0] = DATA;
    arm(m, (uint32_t[]){0xE8B00000}, 1); /* ldmia r0!, {}: PC loaded, base + 0x40 */
    CHECK(m->cpu.r[0] == DATA + 0x40 && gba_cpu_pc(m) == CODE + 0x100, "empty ldm: r0=%x pc=%x", m->cpu.r[0],
          gba_cpu_pc(m));

    m->cpu.r[0] = DATA + 0x40;
    m->cpu.r[1] = 1;
    m->cpu.r[2] = 2;
    arm(m, (uint32_t[]){0xE9200006}, 1); /* stmdb r0!, {r1, r2} */
    CHECK(gba_peek32(m, DATA + 0x38) == 1 && gba_peek32(m, DATA + 0x3C) == 2 && m->cpu.r[0] == DATA + 0x38,
          "stmdb");

    /* User-bank transfer from IRQ mode: stmia r0, {r13}^ stores the user r13. */
    uint32_t user_sp = m->cpu.r[13];
    gba_set_mode(m, MODE_IRQ);
    m->cpu.r[0] = DATA;
    arm(m, (uint32_t[]){0xE8C02000}, 1);
    CHECK(gba_peek32(m, DATA) == user_sp && m->cpu.r[13] != user_sp, "stm ^: %x vs %x", gba_peek32(m, DATA), user_sp);
    gba_destroy(m);
}

static void test_modes(void)
{
    gba_machine *m = machine();
    CHECK((gba_get_cpsr(m) & 0x1F) == MODE_SYS && m->cpu.r[13] == 0x03007F00, "direct boot: SYS, sp 0x03007F00");
    arm(m, (uint32_t[]){0xE321F012}, 1); /* msr cpsr_c, #0x12 (IRQ) */
    CHECK(m->cpu.r[13] == 0x03007FA0, "irq sp: %x", m->cpu.r[13]);
    arm(m, (uint32_t[]){0xE321F013}, 1); /* SVC */
    CHECK(m->cpu.r[13] == 0x03007FE0, "svc sp: %x", m->cpu.r[13]);
    m->cpu.r[8] = 8;
    arm(m, (uint32_t[]){0xE321F011, 0xE3A08063, 0xE321F01F}, 3); /* FIQ; mov r8, #0x63; SYS */
    CHECK(m->cpu.r[8] == 8 && m->cpu.fiq_r8_12[0] == 0x63, "fiq r8 banked: %x", m->cpu.r[8]);

    /* MSR in user mode only reaches the flags. */
    arm(m, (uint32_t[]){0xE321F010, 0xE329F01F}, 2); /* USR; msr cpsr_fc, #0x1F */
    CHECK((gba_get_cpsr(m) & 0x1F) == MODE_USR, "user msr cannot change mode");

    /* movs pc, lr from IRQ restores a Thumb CPSR. */
    m = (gba_destroy(m), machine());
    gba_set_mode(m, MODE_IRQ);
    m->cpu.spsr[BANK_IRQ] = MODE_SYS | CPSR_T;
    m->cpu.r[14] = CODE + 0x41;
    arm(m, (uint32_t[]){0xE1B0F00E}, 1);
    CHECK((gba_get_cpsr(m) & 0x3F) == (MODE_SYS | CPSR_T) && gba_cpu_pc(m) == CODE + 0x40, "movs pc, lr: %x pc %x",
          gba_get_cpsr(m), gba_cpu_pc(m));
    gba_destroy(m);
}

static void test_multiply_swap(void)
{
    gba_machine *m = machine();
    m->cpu.r[2] = 0xFFFFFFFFu;
    m->cpu.r[3] = 2;
    arm(m, (uint32_t[]){0xE0810392}, 1); /* umull r0, r1, r2, r3 */
    CHECK(m->cpu.r[0] == 0xFFFFFFFEu && m->cpu.r[1] == 1, "umull: %x:%x", m->cpu.r[1], m->cpu.r[0]);
    m->cpu.r[0] = 1;
    m->cpu.r[1] = 0;
    arm(m, (uint32_t[]){0xE0F10392}, 1); /* smlals r0, r1, r2, r3: -2 + 1 */
    CHECK(m->cpu.r[0] == 0xFFFFFFFFu && m->cpu.r[1] == 0xFFFFFFFFu && m->cpu.n == 1, "smlals: %x:%x",
          m->cpu.r[1], m->cpu.r[0]);
    /* C after a long multiply: bit 31 of the last Booth addend (the mGBA suite's hardware results) */
    m->cpu.r[2] = 0xFFFFFFFFu;
    m->cpu.r[3] = 0xFFFFFFFFu;
    m->cpu.c = 0;
    arm(m, (uint32_t[]){0xE0910392}, 1); /* umulls: digit +2, (2 * rm) bit 31 */
    CHECK(m->cpu.c == 1 && m->cpu.n == 1, "umulls C: %u", m->cpu.c);
    m->cpu.r[2] = 0x80000000u;
    m->cpu.r[3] = 0xFFFFFFFFu;
    arm(m, (uint32_t[]){0xE0910392}, 1); /* umulls: rm bit 30 clear */
    CHECK(m->cpu.c == 0, "umulls C: %u", m->cpu.c);
    m->cpu.r[2] = 0;
    m->cpu.r[3] = 0x80000000u;
    arm(m, (uint32_t[]){0xE0D10392}, 1); /* smulls: digit -1, ~rm bit 31 */
    CHECK(m->cpu.c == 1 && m->cpu.z == 1, "smulls C: %u", m->cpu.c);
    arm(m, (uint32_t[]){0xE0910392}, 1); /* umulls: digit +1, rm bit 31 */
    CHECK(m->cpu.c == 0, "umulls C: %u", m->cpu.c);
    m->cpu.r[1] = 7;
    m->cpu.r[2] = 6;
    m->cpu.r[3] = 100;
    arm(m, (uint32_t[]){0xE0203291}, 1); /* mla r0, r1, r2, r3 */
    CHECK(m->cpu.r[0] == 142, "mla: %u", m->cpu.r[0]);

    wr32(&m->iwram[DATA & 0x7FFF], 0x11223344);
    m->cpu.r[1] = DATA;
    m->cpu.r[2] = 0xCAFE;
    arm(m, (uint32_t[]){0xE1010092}, 1); /* swp r0, r2, [r1] */
    CHECK(m->cpu.r[0] == 0x11223344 && gba_peek32(m, DATA) == 0xCAFE, "swp");
    m->cpu.r[2] = 0x99;
    arm(m, (uint32_t[]){0xE1410092}, 1); /* swpb r0, r2, [r1] */
    CHECK(m->cpu.r[0] == 0xFE && gba_peek32(m, DATA) == 0xCA99, "swpb: %x %x", m->cpu.r[0], gba_peek32(m, DATA));
    gba_destroy(m);
}

static void test_thumb(void)
{
    gba_machine *m = machine();
    thumb(m, (uint16_t[]){0xF000, 0xF802}, 2, 2); /* bl +4 */
    CHECK(gba_cpu_pc(m) == CODE + 8 && m->cpu.r[14] == ((CODE + 4) | 1), "bl: pc %x lr %x", gba_cpu_pc(m),
          m->cpu.r[14]);

    m->cpu.r[0] = (uint32_t)-7;
    m->cpu.r[1] = 2;
    thumb(m, (uint16_t[]){0xDF06}, 1, 1); /* swi 6: Div (HLE, through the built-in BIOS's dispatcher) */
    for (int i = 0; i < 100 && gba_cpu_pc(m) < 0x4000; i++)
        gba_cpu_step(m);
    CHECK(m->cpu.r[0] == (uint32_t)-3 && m->cpu.r[1] == (uint32_t)-1 && m->cpu.r[3] == 3, "Div: %d %d %d",
          (int)m->cpu.r[0], (int)m->cpu.r[1], (int)m->cpu.r[3]);
    CHECK(gba_cpu_pc(m) == CODE + 2 && (gba_get_cpsr(m) & CPSR_T), "after swi: pc %x", gba_cpu_pc(m));

    m->cpu.r[0] = 1;
    m->cpu.r[1] = 33;
    m->cpu.c = 1;
    thumb(m, (uint16_t[]){0x4088}, 1, 1); /* lsls r0, r1 (33): 0, C = 0 */
    CHECK(m->cpu.r[0] == 0 && m->cpu.c == 0 && m->cpu.z == 1, "thumb lsl by 33");

    m->cpu.r[1] = 5;
    thumb(m, (uint16_t[]){0x4248}, 1, 1); /* negs r0, r1 */
    CHECK(m->cpu.r[0] == (uint32_t)-5 && m->cpu.c == 0 && m->cpu.n == 1, "neg: %x c %u", m->cpu.r[0], m->cpu.c);

    thumb(m, (uint16_t[]){0x4778}, 1, 1); /* bx pc: to ARM at the aligned PC + 4 */
    CHECK(!(gba_get_cpsr(m) & CPSR_T) && gba_cpu_pc(m) == CODE + 4, "bx pc: pc %x", gba_cpu_pc(m));

    /* push {r0, lr}; pop {r1, pc} */
    m->cpu.r[0] = 0x55;
    m->cpu.r[14] = CODE + 0x21;
    uint32_t sp = m->cpu.r[13];
    thumb(m, (uint16_t[]){0xB501, 0xBD02}, 2, 2);
    CHECK(m->cpu.r[1] == 0x55 && gba_cpu_pc(m) == CODE + 0x20 && m->cpu.r[13] == sp && (gba_get_cpsr(m) & CPSR_T),
          "push/pop: r1 %x pc %x", m->cpu.r[1], gba_cpu_pc(m));

    m->cpu.r[3] = DATA;
    wr32(&m->iwram[DATA & 0x7FFF], 0x77);
    thumb(m, (uint16_t[]){0xCB08}, 1, 1); /* ldmia r3!, {r3}: no writeback */
    CHECK(m->cpu.r[3] == 0x77, "thumb ldm base in list: %x", m->cpu.r[3]);

    thumb(m, (uint16_t[]){0x4800, 0, 0x5678, 0x1234}, 4, 1); /* ldr r0, [pc, #0]: from the aligned PC */
    CHECK(m->cpu.r[0] == 0x12345678u, "ldr pc-relative: %x", m->cpu.r[0]);
    gba_destroy(m);
}

static void test_irq(void)
{
    gba_machine *m = machine();
    /* An IRQ while running Thumb code: LR = next + 4, ARM, IRQ mode, I set, built-in vector. */
    thumb(m, (uint16_t[]){0x46C0, 0x46C0}, 2, 1); /* nop; then the IRQ */
    gba_raise_irq(m, IRQ_VBLANK);
    m->ie = IRQ_VBLANK;
    m->ime = 1;
    gba_update_irq(m);
    /* The request reaches the CPU 4 cycles later (XBA-201, the mGBA suite's timer IRQ tests). */
    CHECK(!m->irq_line && m->irq_at == m->now + 4, "irq latency: line %d at %llu (now %llu)", m->irq_line,
          (unsigned long long)m->irq_at, (unsigned long long)m->now);
    m->now += 4;
    gba_update_irq(m);
    gba_cpu_step(m);
    CHECK((gba_get_cpsr(m) & 0x1F) == MODE_IRQ && gba_cpu_pc(m) == 0x18 && m->cpu.r[14] == CODE + 2 + 4 &&
              (m->cpu.spsr[BANK_IRQ] & CPSR_T) && (gba_get_cpsr(m) & CPSR_I),
          "irq entry: cpsr %x pc %x lr %x", gba_get_cpsr(m), gba_cpu_pc(m), m->cpu.r[14]);
    gba_destroy(m);
}

static void test_dma_timer(void)
{
    gba_machine *m = machine();
    for (int i = 0; i < 4; i++)
        wr32(&m->iwram[(DATA & 0x7FFF) + 4 * (unsigned)i], 0x1000u + (unsigned)i);
    gba_write32(m, 0x040000D4, DATA, false);
    gba_write32(m, 0x040000D8, DATA + 0x100, false);
    gba_write32(m, 0x040000DC, 4 | (0x8400u << 16), false); /* 4 words, 32-bit, now */
    CHECK(gba_peek32(m, DATA + 0x10C) == 0 && m->dma_due == m->now + 2, "dma3 waits for its start delay");
    m->now += 2;
    (void)gba_read32(m, DATA, false); /* a CPU access after the delay: the transfer runs first */
    CHECK(gba_peek32(m, DATA + 0x10C) == 0x1003 && !(gba_io_read16(m, 0x040000DE) & 0x8000), "dma3 copy");

    /* Timer 0 at 1 cycle per tick from 0xFFF0 overflows after 16 cycles into the cascaded timer 1. */
    gba_write16(m, 0x04000104, 0, false);
    gba_write16(m, 0x04000106, 0x0084, false); /* cascade, enable */
    gba_write16(m, 0x04000100, 0xFFF0, false);
    gba_write16(m, 0x04000102, 0x00C0, false); /* IRQ, enable */
    uint64_t start = m->now;
    m->cpu.halted = true;   /* IE = 0: stays halted */
    gba_run_frame(m, NULL); /* runs to VBlank; timer 0 overflowed many times */
    CHECK(m->if_ & IRQ_TIMER0, "timer 0 IRQ");
    uint32_t elapsed = (uint32_t)(m->now - start);
    uint32_t t1 = gba_io_read16(m, 0x04000104);
    CHECK(t1 == (elapsed / 16) || t1 + 1 == elapsed / 16, "cascade count %u for %u cycles", t1, elapsed);
    gba_destroy(m);
}

static void test_frame_timing(void)
{
    gba_machine *m = machine();
    /* A halted CPU: two VBlanks are exactly 280896 cycles apart. */
    m->cpu.halted = true;
    gba_run_frame(m, NULL);
    uint64_t a = m->now;
    CHECK(rd16(&m->io[6]) == 160 && (m->io[4] & 1), "stops at VBlank: vcount %u", rd16(&m->io[6]));
    gba_run_frame(m, NULL);
    uint64_t d = m->now - a;
    CHECK(d == GBA_CYCLES_PER_FRAME, "frame length %llu", (unsigned long long)d);
    gba_destroy(m);
}

int main(void)
{
    test_shifter();
    test_pc_reads();
    test_misaligned();
    test_block_transfer();
    test_modes();
    test_multiply_swap();
    test_thumb();
    test_irq();
    test_dma_timer();
    test_frame_timing();
    if (failures) {
        printf("gba_cpu_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("gba_cpu_test: ok\n");
    return 0;
}
