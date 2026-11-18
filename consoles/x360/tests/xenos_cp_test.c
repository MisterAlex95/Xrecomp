/* xenos_cp_test -- the PM4 command processor on hand-built packets (XBA-270). Expected values follow the packet
 * definitions (Xenia's xenos.h / command_processor.cc, BSD): register blocks, constants, read-modify-write, memory
 * writes and their endian modes, event fences, conditional writes, draw initiators, indirect buffers, interrupts,
 * the swap token; on the ring: a wait that stalls then resumes, a packet across the end of the ring, a packet the
 * CPU has not finished writing, the read pointer written back. */
#include <stdio.h>
#include <string.h>

#include "xenos_cp.h"

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static uint32_t mem[0x10000]; /* physical memory, one big-endian word per entry (as the guest reads it) */
static int irqs;

static uint32_t rd(void *ctx, uint32_t a) { (void)ctx; return mem[(a / 4) & 0xFFFF]; }
static void wr(void *ctx, uint32_t a, uint32_t v) { (void)ctx; mem[(a / 4) & 0xFFFF] = v; }
static void irq(void *ctx, uint32_t source, uint32_t cpu) { (void)ctx; (void)source; (void)cpu; irqs++; }

static uint32_t t0(uint32_t index, uint32_t count, int one) { return (count - 1) << 16 | (one ? 1u << 15 : 0) | index; }
static uint32_t t1(uint32_t a, uint32_t b) { return 1u << 30 | b << 11 | a; }
static uint32_t t3(uint32_t op, uint32_t count) { return 3u << 30 | (count - 1) << 16 | op << 8; }

static uint32_t at;
static void put(uint32_t base, uint32_t v) { mem[base / 4 + at++] = v; }

static uint32_t imm_got[4], imm_seen;
static void imm_hook(void *ctx, const xenos_cp *c, const xenos_draw *d)
{
    (void)ctx;
    (void)c;
    imm_seen = d->imm_words;
    for (uint32_t i = 0; d->imm && i < d->imm_words && i < 4; i++)
        imm_got[i] = d->imm[i];
}

int main(void)
{
    static xenos_cp cp;
    cp.read32 = rd;
    cp.write32 = wr;
    cp.interrupt = irq;
    xenos_cp_init(&cp);

    /* ---- a linear buffer at 0x2000 ---- */
    const uint32_t B = 0x2000;
    at = 0;
    put(B, t0(0x2000, 3, 0)), put(B, 1), put(B, 2), put(B, 3);
    put(B, t0(0x2100, 3, 1)), put(B, 7), put(B, 8), put(B, 9);
    put(B, t1(0x10, 0x20)), put(B, 0xAA), put(B, 0xBB);
    put(B, 2u << 30);
    put(B, t3(PM4_SET_CONSTANT, 3)), put(B, 1u << 16 | 5), put(B, 0x11), put(B, 0x22);
    put(B, t3(PM4_SET_CONSTANT, 2)), put(B, 4u << 16 | 0x81), put(B, 0x33);
    put(B, t3(PM4_REG_RMW, 3)), put(B, 0x10), put(B, 0xFFFF0000u), put(B, 5); /* 13-bit register index */
    put(B, t3(PM4_MEM_WRITE, 3)), put(B, 0x8000 | 2), put(B, 0x11223344), put(B, 0x55667788);
    put(B, t3(PM4_MEM_WRITE, 2)), put(B, 0x8010 | 0), put(B, 0x11223344);
    put(B, t3(PM4_MEM_WRITE, 2)), put(B, 0x8014 | 1), put(B, 0x11223344);
    put(B, t3(PM4_MEM_WRITE, 2)), put(B, 0x8018 | 3), put(B, 0x11223344);
    put(B, t3(PM4_EVENT_WRITE_SHD, 3)), put(B, 5), put(B, 0x8020 | 2), put(B, 0xCAFE);
    put(B, t3(PM4_COND_WRITE, 6)), put(B, 0x13), put(B, 0x8020 | 2), put(B, 0xCAFE), put(B, 0xFFFFFFFFu),
        put(B, 0x2200), put(B, 0x77);
    put(B, t3(PM4_COND_WRITE, 6)), put(B, 0x113), put(B, 0x8020 | 2), put(B, 0xBEEF), put(B, 0xFFFFFFFFu),
        put(B, 0x8024 | 2), put(B, 0x99);
    put(B, t3(PM4_DRAW_INDX, 4)), put(B, 0), put(B, 4 | 0u << 6 | 1u << 11 | 36u << 16), put(B, 0x9000), put(B, 36);
    put(B, t3(PM4_DRAW_INDX_2, 1)), put(B, 8 | 2u << 6 | 3u << 16);
    put(B, t3(PM4_INDIRECT_BUFFER, 2)), put(B, 0xA000), put(B, 3);
    put(B, t3(PM4_INTERRUPT, 1)), put(B, 0x3);
    put(B, t3(PM4_XE_SWAP, 4)), put(B, 0x53574150), put(B, 0x01000000), put(B, 1280), put(B, 720);
    put(B, t3(0x7E, 2)), put(B, 0), put(B, 0);
    put(B, t3(PM4_EVENT_WRITE_EXT, 2)), put(B, 6), put(B, 0x8030 | 2);
    put(B, t3(PM4_IM_LOAD_IMMEDIATE, 5)), put(B, 1), put(B, 0u << 16 | 3), put(B, 0xC0DE0001), put(B, 0xC0DE0002),
        put(B, 0xC0DE0003);
    put(B, t3(PM4_IM_LOAD, 2)), put(B, 0xB000 | 0), put(B, 0u << 16 | 2);
    mem[0xB000 / 4] = 0x5EED0001, mem[0xB004 / 4] = 0x5EED0002;
    uint32_t n = at;
    at = 0;
    put(0xA000, t0(0x2300, 2, 0)), put(0xA000, 0x1), put(0xA000, 0x2);

    uint32_t packets = xenos_cp_execute(&cp, B, n);
    CHECK(packets == 23, "packets %u", packets);
    CHECK(cp.regs[0x2000] == 1 && cp.regs[0x2001] == 2 && cp.regs[0x2002] == 3, "type 0 block: %X %X %X",
          cp.regs[0x2000], cp.regs[0x2001], cp.regs[0x2002]);
    CHECK(cp.regs[0x2100] == 9 && cp.regs[0x2101] == 0, "type 0 one register");
    CHECK(cp.regs[0x10] == 5 && cp.regs[0x20] == 0xBB, "type 1, then read-modify-write");
    CHECK(cp.regs[0x4805] == 0x11 && cp.regs[0x4806] == 0x22 && cp.regs[0x2081] == 0x33, "constants");
    CHECK(mem[0x8000 / 4] == 0x11223344 && mem[0x8004 / 4] == 0x55667788, "8in32: as written %08X", mem[0x2000]);
    CHECK(mem[0x8010 / 4] == 0x44332211, "no swap: little-endian %08X", mem[0x8010 / 4]);
    CHECK(mem[0x8014 / 4] == 0x33441122, "8in16 %08X", mem[0x8014 / 4]);
    CHECK(mem[0x8018 / 4] == 0x22114433, "16in32 %08X", mem[0x8018 / 4]);
    CHECK(mem[0x8020 / 4] == 0xCAFE && cp.regs[XENOS_REG_VGT_EVENT_INITIATOR] == 6, "event fence, initiator %u",
          cp.regs[XENOS_REG_VGT_EVENT_INITIATOR]);
    CHECK(cp.regs[0x2200] == 0x77 && mem[0x8024 / 4] == 0, "conditional writes: met to a register, unmet to memory");
    CHECK(cp.draws == 2 && cp.last_draw.prim_type == 8 && cp.last_draw.source_select == 2 &&
              cp.last_draw.num_indices == 3, "draws");
    CHECK(cp.regs[XENOS_REG_VGT_DMA_BASE] == 0x9000 && cp.regs[XENOS_REG_VGT_DMA_SIZE] == 36, "index buffer");
    CHECK(cp.regs[0x2300] == 1 && cp.regs[0x2301] == 2, "indirect buffer");
    CHECK(irqs == 2 && cp.interrupts == 1, "interrupt on two CPUs");
    CHECK(cp.swaps == 1 && cp.last_swap_fb == 0x01000000 && cp.last_swap_w == 1280 && cp.last_swap_h == 720, "swap");
    CHECK(cp.unknown == 1, "unknown opcode counted");
    CHECK(mem[0x8030 / 4] == (0u << 16 | 1024u) && mem[0x8034 / 4] == (0u << 16 | 1024u) &&
              mem[0x8038 / 4] == (0u << 16 | 1u), "screen extents %08X %08X %08X", mem[0x8030 / 4],
          mem[0x8034 / 4], mem[0x8038 / 4]);

    CHECK(cp.shader[1].words == 3 && cp.shader[1].code[2] == 0xC0DE0003 && cp.shader[0].words == 2 &&
              cp.shader[0].code[1] == 0x5EED0002 && cp.shader[0].address == 0xB000 && cp.shader_loads == 2,
          "shader microcode kept");

    /* ---- the ring: 0x10000, 2^10 quadwords = 8 KB = 2048 dwords ---- */
    const uint32_t R = 0x10000;
    xenos_cp_ring_init(&cp, R, 10);
    cp.rptr_writeback = 0xF000;
    CHECK(cp.rb_size == 8192, "ring size %u", cp.rb_size);
    at = 0;
    put(R, t3(PM4_WAIT_REG_MEM, 5)), put(R, 3), put(R, 0x2400), put(R, 1), put(R, 0xFFFFFFFFu), put(R, 0x10);
    put(R, t0(0x2401, 1, 0)), put(R, 0x42);
    uint32_t w = at;
    xenos_cp_set_wptr(&cp, w);
    CHECK(cp.stalled && cp.rptr == 0 && cp.regs[0x2401] == 0 && mem[0xF000 / 4] == 0, "stalled on the wait");
    xenos_cp_write_reg(&cp, 0x2400, 1);
    xenos_cp_set_wptr(&cp, w);
    CHECK(!cp.stalled && cp.rptr == w && cp.regs[0x2401] == 0x42 && mem[0xF000 / 4] == w, "resumed: rptr %u",
          cp.rptr);
    /* a packet the CPU has not finished: header and one of three values */
    at = w;
    put(R, t0(0x2500, 3, 0)), put(R, 0xA);
    xenos_cp_set_wptr(&cp, at);
    CHECK(cp.rptr == w && cp.regs[0x2500] == 0, "incomplete packet waits");
    put(R, 0xB), put(R, 0xC);
    xenos_cp_set_wptr(&cp, at);
    CHECK(cp.rptr == at && cp.regs[0x2502] == 0xC, "then runs");
    /* across the end of the ring */
    cp.rptr = 2046;
    mem[R / 4 + 2046] = t0(0x2600, 2, 0);
    mem[R / 4 + 2047] = 0x61;
    mem[R / 4 + 0] = 0x62;
    xenos_cp_set_wptr(&cp, 1);
    CHECK(cp.rptr == 1 && cp.regs[0x2600] == 0x61 && cp.regs[0x2601] == 0x62, "wrapped packet");

    /* XBA-296: immediate indices reach the draw hook, also across the end of the ring, and do not outlive it */
    cp.draw_hook = imm_hook;
    cp.rptr = 2045;
    mem[R / 4 + 2045] = t3(PM4_DRAW_INDX_2, 3);
    mem[R / 4 + 2046] = 4 | 1u << 6 | 1u << 11 | 2u << 16;
    mem[R / 4 + 2047] = 0x11;
    mem[R / 4 + 0] = 0x22;
    xenos_cp_set_wptr(&cp, 1);
    CHECK(imm_seen == 2 && imm_got[0] == 0x11 && imm_got[1] == 0x22, "immediate indices: %u words, %X %X", imm_seen,
          imm_got[0], imm_got[1]);
    CHECK(cp.last_draw.source_select == 1 && cp.last_draw.imm == NULL && cp.rptr == 1, "the draw kept no pointer");
    cp.draw_hook = NULL;

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "all command processor checks passed", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
