/* xenos_cp_test -- the PM4 command processor on hand-built packets (XBA-270). Expected values follow the packet
 * definitions (Xenia's xenos.h / command_processor.cc, BSD): register blocks, constants, read-modify-write, memory
 * writes and their endian modes, event fences, conditional writes, draw initiators, indirect buffers, interrupts,
 * the swap token, scratch register write-back; on the ring: a wait that stalls then resumes, also inside nested
 * indirect buffers, a packet across the end of the ring, a packet the CPU has not finished writing, the read pointer
 * written back; tiled rendering's predicate (the bin mask and select, a header's bit 0); occlusion queries. */
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

    /* ---- scratch register write-back: mask bits 0, 1 and 4, 8in32 mode, at 0xC000 ---- */
    at = 0;
    put(B, t0(XENOS_REG_SCRATCH_REG0 + 1, 1, 0)), put(B, 0x0BADF00D); /* before the mask is set: register only */
    put(B, t0(XENOS_REG_SCRATCH_ADDR, 1, 0)), put(B, 0xC000);
    put(B, t0(XENOS_REG_SCRATCH_UMSK, 1, 0)), put(B, 2u << 16 | 0x13);
    put(B, t0(XENOS_REG_SCRATCH_REG0, 3, 0)), put(B, 0x11223344), put(B, 1), put(B, 0x55);
    put(B, t0(XENOS_REG_SCRATCH_REG0 + 4, 1, 0)), put(B, 0xCAFEF00D);
    /* what Direct3D does next: the GPU waits on the memory copy, here to write a register when it matches */
    put(B, t3(PM4_COND_WRITE, 6)), put(B, 0x13), put(B, (0xC000 + 4) | 2), put(B, 1), put(B, 0xFFFFFFFFu),
        put(B, 0x2210), put(B, 0x99);
    put(B, t0(XENOS_REG_SCRATCH_UMSK, 1, 0)), put(B, 0u << 16 | 0x01);
    put(B, t0(XENOS_REG_SCRATCH_REG0, 2, 0)), put(B, 0xA1B2C3D4), put(B, 7);
    mem[0xC004 / 4] = mem[0xC008 / 4] = mem[0xC00C / 4] = mem[0xC010 / 4] = 0xEEEEEEEEu;
    n = at;
    xenos_cp_execute(&cp, B, n);
    CHECK(cp.regs[XENOS_REG_SCRATCH_REG0 + 2] == 0x55 && cp.regs[XENOS_REG_SCRATCH_REG0 + 4] == 0xCAFEF00D,
          "the scratch registers keep their values");
    CHECK(mem[0xC008 / 4] == 0xEEEEEEEEu && mem[0xC00C / 4] == 0xEEEEEEEEu, "unmasked registers are not written back");
    CHECK(mem[0xC010 / 4] == 0xCAFEF00D, "register 4 written back as the guest reads it: %08X", mem[0xC010 / 4]);
    CHECK(cp.regs[0x2210] == 0x99, "the GPU reads its own write-back");
    CHECK(mem[0xC000 / 4] == 0xD4C3B2A1 && mem[0xC004 / 4] == 1, "mask and mode changed: %08X %08X",
          mem[0xC000 / 4], mem[0xC004 / 4]);

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
    /* a wait inside an indirect buffer stops the command processor there, nested or not: what came before it ran
     * once, what follows (in the buffers, then in the ring) runs when the wait holds. Buffer 0xD000 draws, calls
     * 0xD100 (a wait on memory 0xC100, then a register) and writes a register; 0xD200 is nothing but a call. */
    uint32_t w0 = at, draws0 = (uint32_t)cp.draws;
    put(R, t3(PM4_INDIRECT_BUFFER, 2)), put(R, 0xD000), put(R, 8);
    put(R, t0(0x2702, 1, 0)), put(R, 0x73);
    put(R, t3(PM4_INDIRECT_BUFFER, 2)), put(R, 0xD200), put(R, 3);
    put(R, t0(0x2705, 1, 0)), put(R, 0x76);
    uint32_t w1 = at;
    at = 0;
    put(0xD000, t3(PM4_DRAW_INDX_2, 1)), put(0xD000, 8 | 2u << 6 | 3u << 16);
    put(0xD000, t3(PM4_INDIRECT_BUFFER, 2)), put(0xD000, 0xD100), put(0xD000, 10);
    put(0xD000, t0(0x2701, 1, 0)), put(0xD000, 0x72);
    at = 0;
    put(0xD100, t0(0x2703, 1, 0)), put(0xD100, 0x74);
    put(0xD100, t3(PM4_WAIT_REG_MEM, 5)), put(0xD100, 0x13), put(0xD100, 0xC100 | 2), put(0xD100, 1),
        put(0xD100, 0xFFFFFFFFu), put(0xD100, 0x10);
    put(0xD100, t0(0x2700, 1, 0)), put(0xD100, 0x71);
    at = 0;
    put(0xD200, t3(PM4_INDIRECT_BUFFER, 2)), put(0xD200, 0xD100), put(0xD200, 10);
    mem[0xC100 / 4] = 0;
    cp.regs[0x2703] = 0;
    xenos_cp_set_wptr(&cp, w1);
    CHECK(cp.stalled && cp.draws == draws0 + 1 && cp.regs[0x2703] == 0x74, "stopped inside the nested buffer");
    CHECK(cp.regs[0x2700] == 0 && cp.regs[0x2701] == 0 && cp.regs[0x2702] == 0, "nothing after the wait has run");
    xenos_cp_set_wptr(&cp, w1); /* polled again while it does not hold */
    CHECK(cp.stalled && cp.draws == draws0 + 1 && cp.regs[0x2700] == 0, "still stopped, nothing run twice");
    mem[0xC100 / 4] = 1;
    cp.regs[0x2703] = 0;
    xenos_cp_set_wptr(&cp, w1);
    CHECK(!cp.stalled && cp.draws == draws0 + 1, "resumed: the draw before the wait is not repeated");
    CHECK(cp.regs[0x2700] == 0x71 && cp.regs[0x2701] == 0x72 && cp.regs[0x2702] == 0x73,
          "the rest of both buffers, then the ring: %X %X %X", cp.regs[0x2700], cp.regs[0x2701], cp.regs[0x2702]);
    CHECK(cp.regs[0x2703] == 0x74 && cp.regs[0x2705] == 0x76 && cp.rptr == w1 && mem[0xF000 / 4] == w1,
          "the second call ran the buffer from its start, the ring to its end: rptr %u", cp.rptr);
    /* stopped in a buffer that is all of its caller: resumed once, the caller not run again */
    mem[0xC100 / 4] = 0;
    cp.regs[0x2700] = cp.regs[0x2703] = 0;
    at = w1;
    put(R, t3(PM4_INDIRECT_BUFFER, 2)), put(R, 0xD200), put(R, 3);
    put(R, t0(0x2706, 1, 0)), put(R, 0x77);
    xenos_cp_set_wptr(&cp, at);
    CHECK(cp.stalled && cp.regs[0x2703] == 0x74 && cp.regs[0x2706] == 0, "stopped again");
    mem[0xC100 / 4] = 1;
    cp.regs[0x2703] = 0;
    xenos_cp_set_wptr(&cp, at);
    CHECK(!cp.stalled && cp.regs[0x2700] == 0x71 && cp.regs[0x2703] == 0 && cp.regs[0x2706] == 0x77 && cp.rptr == at,
          "resumed after the wait only: %X %X %X", cp.regs[0x2700], cp.regs[0x2703], cp.regs[0x2706]);
    (void)w0;

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

    /* Tiled rendering's predicate (Xenia's command_processor.cc): a type-3 packet whose header has bit 0 set runs
     * only while the 64-bit bin select and bin mask share a bit; both start as 0xFFFFFFFF. SET_BIN_MASK / SELECT
     * take the high word first, the _LO / _HI opcodes one half. A predicated swap never runs. */
    const uint32_t P = 0xE000;
    uint64_t swaps0 = cp.swaps;
    CHECK(cp.bin_mask == 0xFFFFFFFFu && cp.bin_select == 0xFFFFFFFFu, "the bin registers' initial values");
    at = 0;
    put(P, t3(PM4_SET_CONSTANT2, 2) | 1), put(P, 0x2800), put(P, 0x44); /* runs: the initial values */
    put(P, t3(PM4_SET_BIN_MASK, 2)), put(P, 0), put(P, 0x10);
    put(P, t3(PM4_SET_BIN_SELECT_LO, 1)), put(P, 0x01);
    put(P, t3(PM4_SET_CONSTANT2, 2) | 1), put(P, 0x2801), put(P, 0x55); /* skipped: 0x01 & 0x10 */
    put(P, t3(PM4_SET_CONSTANT2, 2)), put(P, 0x2802), put(P, 0x66);     /* not predicated */
    put(P, t3(PM4_SET_BIN_SELECT_LO, 1)), put(P, 0x30);
    put(P, t3(PM4_SET_CONSTANT2, 2) | 1), put(P, 0x2803), put(P, 0x77); /* runs: 0x30 & 0x10 */
    put(P, t3(PM4_SET_BIN_MASK_LO, 1)), put(P, 0);
    put(P, t3(PM4_SET_BIN_MASK_HI, 1)), put(P, 1);
    put(P, t3(PM4_SET_CONSTANT2, 2) | 1), put(P, 0x2804), put(P, 0x88); /* skipped: bit 32 against 0x30 */
    put(P, t3(PM4_SET_BIN_SELECT_HI, 1)), put(P, 3);
    put(P, t3(PM4_SET_CONSTANT2, 2) | 1), put(P, 0x2805), put(P, 0x99); /* runs: bit 32 in both */
    put(P, t3(PM4_SET_BIN_SELECT, 2)), put(P, 2), put(P, 0xFFFFFFFFu);
    put(P, t3(PM4_SET_CONSTANT2, 2) | 1), put(P, 0x2806), put(P, 0xAA); /* skipped: high word 2 against 1 */
    put(P, t3(PM4_SET_BIN_MASK, 2)), put(P, 0xFFFFFFFFu), put(P, 0xFFFFFFFFu);
    put(P, t3(PM4_XE_SWAP, 4) | 1), put(P, 0x53574150), put(P, 0x01000000), put(P, 1280), put(P, 720);
    put(P, t0(0x2807, 1, 0)), put(P, 0xBB); /* the packet after each skipped one starts where it should */
    for (uint32_t k = 0x2800; k <= 0x2807; k++)
        cp.regs[k] = 0;
    uint64_t pred0 = cp.predicated, skip0 = cp.predicated_skipped;
    xenos_cp_execute(&cp, P, at);
    CHECK(cp.regs[0x2800] == 0x44 && cp.regs[0x2802] == 0x66 && cp.regs[0x2803] == 0x77 && cp.regs[0x2805] == 0x99 &&
              cp.regs[0x2807] == 0xBB,
          "predicated packets that run: %X %X %X %X %X", cp.regs[0x2800], cp.regs[0x2802], cp.regs[0x2803],
          cp.regs[0x2805], cp.regs[0x2807]);
    CHECK(cp.regs[0x2801] == 0 && cp.regs[0x2804] == 0 && cp.regs[0x2806] == 0,
          "predicated packets that are skipped: %X %X %X", cp.regs[0x2801], cp.regs[0x2804], cp.regs[0x2806]);
    CHECK(cp.swaps == swaps0, "a predicated swap does not run");
    CHECK(cp.bin_mask == 0xFFFFFFFFFFFFFFFFull && cp.bin_select == 0x2FFFFFFFFull, "bin registers %llX %llX",
          (unsigned long long)cp.bin_mask, (unsigned long long)cp.bin_select);
    CHECK(cp.predicated - pred0 == 7 && cp.predicated_skipped - skip0 == 4, "counted: %llu predicated, %llu skipped",
          (unsigned long long)(cp.predicated - pred0), (unsigned long long)(cp.predicated_skipped - skip0));
    CHECK(!strcmp(xenos_cp_opcode_name(PM4_SET_BIN_MASK_LO), "SET_BIN_MASK_LO") &&
              xenos_cp_opcode_name(PM4_SET_BIN_SELECT_HI) && xenos_cp_opcode_name(PM4_SET_BIN_SELECT),
          "the bin opcodes have names");

    /* ---- occlusion queries: EVENT_WRITE_ZPD on a sample-count block at 0xD000 (eight little-endian dwords:
     * Total, ZFail, ZPass, StencilFail, each A and B). Begin: the block is cleared. End (Direct3D's 0xFFFFFEED in
     * both ZPass halves, or both ZFail halves): cleared, then 1000 passed samples in Total_A and ZPass_A. ---- */
    for (int k = 0; k < 8; k++)
        mem[0xD000 / 4 + k] = 0x12345678u;
    at = 0;
    put(B, t0(XENOS_REG_RB_SAMPLE_COUNT_ADDR, 1, 0)), put(B, 0xD000);
    put(B, t3(PM4_EVENT_WRITE_ZPD, 1)), put(B, 0x40 | 21); /* ZPASS_DONE, a high bit the register drops */
    xenos_cp_execute(&cp, B, at);
    CHECK(cp.regs[XENOS_REG_VGT_EVENT_INITIATOR] == 21, "zpd initiator %u", cp.regs[XENOS_REG_VGT_EVENT_INITIATOR]);
    int zero = 1;
    for (int k = 0; k < 8; k++)
        zero &= mem[0xD000 / 4 + k] == 0;
    CHECK(zero, "query begin: the block cleared");
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < 8; k++)
            mem[0xD000 / 4 + k] = 0x12345678u;
        mem[0xD000 / 4 + (pass ? 2 : 4)] = mem[0xD000 / 4 + (pass ? 3 : 5)] = 0xFFFFFEEDu;
        at = 0;
        put(B, t3(PM4_EVENT_WRITE_ZPD, 1)), put(B, 21);
        xenos_cp_execute(&cp, B, at);
        int ok = mem[0xD000 / 4] == 0xE8030000u && mem[0xD000 / 4 + 4] == 0xE8030000u; /* 1000, little-endian */
        for (int k = 0; k < 8; k++)
            ok &= k == 0 || k == 4 || mem[0xD000 / 4 + k] == 0;
        CHECK(ok, "query end via %s: %08X %08X %08X %08X %08X %08X", pass ? "ZFail" : "ZPass", mem[0xD000 / 4],
              mem[0xD000 / 4 + 1], mem[0xD000 / 4 + 2], mem[0xD000 / 4 + 3], mem[0xD000 / 4 + 4], mem[0xD000 / 4 + 5]);
    }
    mem[0xD000 / 4 + 4] = 0xFFFFFEEDu, mem[0xD000 / 4 + 5] = 0; /* one half only: not an end */
    at = 0;
    put(B, t3(PM4_EVENT_WRITE_ZPD, 1)), put(B, 21);
    xenos_cp_execute(&cp, B, at);
    CHECK(mem[0xD000 / 4] == 0 && mem[0xD000 / 4 + 4] == 0, "one sentinel half is a begin");

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "all command processor checks passed", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
