/* xenos_cp.h -- the Xenos command processor, decode and trace (XBA-270, epic XBA-229 step 3).
 *
 * The CPU hands the GPU work through PM4 packets in a ring buffer (and indirect buffers it points at), in physical
 * memory. This walks them as the Xenos front end does: type 0 (consecutive register writes, or one register many
 * times), type 1 (two registers), type 2 (no-op), type 3 (an opcode and its words): indirect buffers, register
 * read-modify-write, memory writes with the GPU's endian swaps, conditional writes, event writes (the fences a title
 * waits on), shader constants, draw initiators (recorded, not drawn), interrupts, waits on a register or memory
 * value (the processor stalls there until the condition holds), and the swap token VdSwap leaves in the ring.
 *
 * The register file is kept, draws and swaps are counted and traced, and handed to a renderer when one is attached
 * (draw_hook / swap_hook: xenos_gl.c, GLES 3.0). Packet formats and semantics follow Xenia's xenos.h /
 * command_processor.cc (BSD);
 * the opcode values are the R400-family PM4 ones they list. The state is plain data (save states). */
#ifndef XENOS_CP_H
#define XENOS_CP_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XENOS_REGS 0x6000

/* type-3 opcodes (Xenia's xenos.h) */
enum {
    PM4_NOP = 0x10, PM4_REG_RMW = 0x21, PM4_DRAW_INDX = 0x22, PM4_VIZ_QUERY = 0x23, PM4_SET_STATE = 0x25,
    PM4_WAIT_FOR_IDLE = 0x26, PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B, PM4_SET_CONSTANT = 0x2D,
    PM4_LOAD_CONSTANT_CONTEXT = 0x2E, PM4_LOAD_ALU_CONSTANT = 0x2F, PM4_DRAW_INDX_2 = 0x36,
    PM4_INDIRECT_BUFFER_PFD = 0x37, PM4_INVALIDATE_STATE = 0x3B, PM4_WAIT_REG_MEM = 0x3C, PM4_MEM_WRITE = 0x3D,
    PM4_REG_TO_MEM = 0x3E, PM4_INDIRECT_BUFFER = 0x3F, PM4_COND_EXEC = 0x44, PM4_COND_WRITE = 0x45,
    PM4_EVENT_WRITE = 0x46, PM4_ME_INIT = 0x48, PM4_SET_SHADER_BASES = 0x4A, PM4_MEM_WRITE_CNTR = 0x4F,
    PM4_INTERRUPT = 0x54, PM4_SET_CONSTANT2 = 0x55, PM4_SET_SHADER_CONSTANTS = 0x56, PM4_EVENT_WRITE_SHD = 0x58,
    PM4_EVENT_WRITE_CFL = 0x59, PM4_EVENT_WRITE_EXT = 0x5A, PM4_EVENT_WRITE_ZPD = 0x5B, PM4_CONTEXT_UPDATE = 0x5E,
    PM4_XE_SWAP = 0x64, /* not hardware: the token this runtime's VdSwap writes (Xenia uses the same value) */
};

/* registers this file names (Xenia's register_table.inc) */
enum {
    XENOS_REG_CP_RB_WPTR = 0x01C5, XENOS_REG_SCRATCH_UMSK = 0x01DC, XENOS_REG_SCRATCH_ADDR = 0x01DD,
    XENOS_REG_SCRATCH_REG0 = 0x0578, XENOS_REG_COHER_STATUS_HOST = 0x0A31,
    XENOS_REG_VGT_EVENT_INITIATOR = 0x21F9, XENOS_REG_VGT_DMA_BASE = 0x21FA, XENOS_REG_VGT_DMA_SIZE = 0x21FB,
    XENOS_REG_VGT_DRAW_INITIATOR = 0x21FC,
};

typedef struct xenos_draw {
    uint32_t prim_type, source_select, index_size, num_indices, dma_base, dma_words;
    /* source_select 1: the packet's index dwords as read from the ring, valid during the draw hook only (NULL in
     * last_draw) */
    const uint32_t *imm;
    uint32_t imm_words;
} xenos_draw;

#define XENOS_SHADER_WORDS 0x4000 /* the sequencer's instruction memory: 4096 instructions of 3 dwords, rounded up */

typedef struct xenos_shader {
    uint32_t code[XENOS_SHADER_WORDS];
    uint32_t words, address, loads;
} xenos_shader;

/* an indirect buffer being run: physical address, length and position in dwords, dwords done */
#define XENOS_IB_DEPTH 4
typedef struct xenos_ib {
    uint32_t base, len, pos, done;
} xenos_ib;

typedef struct xenos_cp {
    uint32_t regs[XENOS_REGS];
    /* the primary ring buffer: physical address, size in bytes, read / write pointers in dwords */
    uint32_t rb_base, rb_size, rptr, wptr;
    uint32_t rptr_writeback; /* physical address the read pointer is written back to (0: none) */
    uint32_t counter;        /* CP_PROG_COUNTER-like event counter */
    int stalled;             /* a WAIT_REG_MEM did not hold: the ring stops there until the next update */
    xenos_ib ib[XENOS_IB_DEPTH]; /* the indirect buffers that wait stopped, outermost first: resumed before the ring */
    uint32_t ib_depth;
    uint32_t last_swap_fb, last_swap_w, last_swap_h;
    xenos_draw last_draw;
    xenos_shader shader[2]; /* the active vertex (0) and pixel (1) shader microcode, as last loaded */
    uint64_t shader_loads;
    uint32_t op_count[128]; /* type-3 packets by opcode (the trace summary's histogram) */
    uint64_t packets, reg_writes, draws, swaps, interrupts, unknown;
    /* guest physical memory (big-endian words) and the interrupt sink, given by the runtime */
    void *ctx;
    uint32_t (*read32)(void *ctx, uint32_t phys);
    void (*write32)(void *ctx, uint32_t phys, uint32_t value);
    void (*interrupt)(void *ctx, uint32_t source, uint32_t cpu_mask);
    void (*shader_loaded)(void *ctx, int pixel, const uint32_t *code, uint32_t words); /* optional */
    /* a renderer (xenos_gl.c): each draw initiator and each swap, with the state as it stands (optional) */
    void *render_ctx;
    void (*draw_hook)(void *render_ctx, const struct xenos_cp *cp, const xenos_draw *d);
    void (*swap_hook)(void *render_ctx, const struct xenos_cp *cp);
    FILE *trace;
} xenos_cp;

void xenos_cp_init(xenos_cp *cp);
void xenos_cp_ring_init(xenos_cp *cp, uint32_t base_phys, uint32_t size_log2);
/* the CPU moved the write pointer: run the packets up to it (returns the number of packets run) */
uint32_t xenos_cp_set_wptr(xenos_cp *cp, uint32_t wptr);
/* run ndwords of packets at a physical address (an indirect buffer; also used by tests) */
uint32_t xenos_cp_execute(xenos_cp *cp, uint32_t phys, uint32_t ndwords);
void xenos_cp_write_reg(xenos_cp *cp, uint32_t index, uint32_t value);
const char *xenos_cp_opcode_name(uint32_t opcode);

#ifdef __cplusplus
}
#endif

#endif
