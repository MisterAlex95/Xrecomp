/* xenos_cp.c -- the Xenos command processor: PM4 decode, register file, trace (XBA-270). See xenos_cp.h. */
#include <stdlib.h>
#include <string.h>

#include "xenos_cp.h"

void xenos_cp_init(xenos_cp *cp)
{
    void *ctx = cp->ctx;
    uint32_t (*r)(void *, uint32_t) = cp->read32;
    void (*w)(void *, uint32_t, uint32_t) = cp->write32;
    void (*irq)(void *, uint32_t, uint32_t) = cp->interrupt;
    void (*sh)(void *, int, const uint32_t *, uint32_t) = cp->shader_loaded;
    FILE *trace = cp->trace;
    void *rctx = cp->render_ctx;
    void (*dh)(void *, const xenos_cp *, const xenos_draw *) = cp->draw_hook;
    void (*sw)(void *, const xenos_cp *) = cp->swap_hook;
    memset(cp, 0, sizeof *cp);
    cp->render_ctx = rctx;
    cp->draw_hook = dh;
    cp->swap_hook = sw;
    cp->ctx = ctx;
    cp->read32 = r;
    cp->write32 = w;
    cp->interrupt = irq;
    cp->shader_loaded = sh;
    cp->trace = trace;
    cp->bin_mask = cp->bin_select = 0xFFFFFFFFu;
}

void xenos_cp_ring_init(xenos_cp *cp, uint32_t base_phys, uint32_t size_log2)
{
    cp->rb_base = base_phys & 0x1FFFFFFFu;
    cp->rb_size = 1u << (size_log2 + 3); /* bytes (Xenia: 1 << (size_log2 + 3)) */
    cp->rptr = cp->wptr = 0;
    cp->stalled = 0;
    cp->ib_depth = 0;
}

const char *xenos_cp_opcode_name(uint32_t op)
{
    switch (op) {
    case PM4_NOP: return "NOP";
    case PM4_REG_RMW: return "REG_RMW";
    case PM4_DRAW_INDX: return "DRAW_INDX";
    case PM4_VIZ_QUERY: return "VIZ_QUERY";
    case PM4_SET_STATE: return "SET_STATE";
    case PM4_WAIT_FOR_IDLE: return "WAIT_FOR_IDLE";
    case PM4_IM_LOAD: return "IM_LOAD";
    case PM4_IM_LOAD_IMMEDIATE: return "IM_LOAD_IMMEDIATE";
    case PM4_SET_CONSTANT: return "SET_CONSTANT";
    case PM4_LOAD_CONSTANT_CONTEXT: return "LOAD_CONSTANT_CONTEXT";
    case PM4_LOAD_ALU_CONSTANT: return "LOAD_ALU_CONSTANT";
    case PM4_DRAW_INDX_2: return "DRAW_INDX_2";
    case PM4_INDIRECT_BUFFER_PFD: return "INDIRECT_BUFFER_PFD";
    case PM4_INVALIDATE_STATE: return "INVALIDATE_STATE";
    case PM4_WAIT_REG_MEM: return "WAIT_REG_MEM";
    case PM4_MEM_WRITE: return "MEM_WRITE";
    case PM4_REG_TO_MEM: return "REG_TO_MEM";
    case PM4_INDIRECT_BUFFER: return "INDIRECT_BUFFER";
    case PM4_COND_EXEC: return "COND_EXEC";
    case PM4_COND_WRITE: return "COND_WRITE";
    case PM4_EVENT_WRITE: return "EVENT_WRITE";
    case PM4_ME_INIT: return "ME_INIT";
    case PM4_SET_SHADER_BASES: return "SET_SHADER_BASES";
    case PM4_MEM_WRITE_CNTR: return "MEM_WRITE_CNTR";
    case PM4_SET_BIN_MASK: return "SET_BIN_MASK";
    case PM4_SET_BIN_SELECT: return "SET_BIN_SELECT";
    case PM4_SET_BIN_MASK_LO: return "SET_BIN_MASK_LO";
    case PM4_SET_BIN_MASK_HI: return "SET_BIN_MASK_HI";
    case PM4_SET_BIN_SELECT_LO: return "SET_BIN_SELECT_LO";
    case PM4_SET_BIN_SELECT_HI: return "SET_BIN_SELECT_HI";
    case PM4_INTERRUPT: return "INTERRUPT";
    case PM4_SET_CONSTANT2: return "SET_CONSTANT2";
    case PM4_SET_SHADER_CONSTANTS: return "SET_SHADER_CONSTANTS";
    case PM4_EVENT_WRITE_SHD: return "EVENT_WRITE_SHD";
    case PM4_EVENT_WRITE_CFL: return "EVENT_WRITE_CFL";
    case PM4_EVENT_WRITE_EXT: return "EVENT_WRITE_EXT";
    case PM4_EVENT_WRITE_ZPD: return "EVENT_WRITE_ZPD";
    case PM4_CONTEXT_UPDATE: return "CONTEXT_UPDATE";
    case PM4_XE_SWAP: return "XE_SWAP";
    default: return NULL;
    }
}

/* the GPU's endian modes (the low 2 bits of an address): none, 8in16, 8in32, 16in32 */
static uint32_t gpu_swap(uint32_t v, uint32_t mode)
{
    switch (mode & 3) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
    }
}

/* a memory word as the GPU writes it: the packet's value, swapped by the address's mode, stored little-endian
 * (Xenia: xe::store of GpuSwap(value)); written through the runtime's big-endian accessor */
static void mem_write(xenos_cp *cp, uint32_t addr, uint32_t value)
{
    if (cp->write32)
        cp->write32(cp->ctx, addr & ~3u & 0x1FFFFFFFu, __builtin_bswap32(gpu_swap(value, addr & 3)));
}

void xenos_cp_write_reg(xenos_cp *cp, uint32_t index, uint32_t value)
{
    if (index >= XENOS_REGS)
        return;
    cp->regs[index] = value;
    cp->reg_writes++;
    /* scratch register write-back: a scratch register whose bit is set in SCRATCH_UMSK (bits 0-7) is also written
     * to memory at SCRATCH_ADDR + 4 * its number, in the mask's endian mode (bits 16-17). Direct3D passes values to
     * its interrupt handler this way and makes the GPU wait on the memory copy. */
    if (index >= XENOS_REG_SCRATCH_REG0 && index < XENOS_REG_SCRATCH_REG0 + 8) {
        uint32_t n = index - XENOS_REG_SCRATCH_REG0, umsk = cp->regs[XENOS_REG_SCRATCH_UMSK];
        if (umsk >> n & 1)
            mem_write(cp, ((cp->regs[XENOS_REG_SCRATCH_ADDR] & ~3u) + 4 * n) | (umsk >> 16 & 3), value);
    }
}

static uint32_t mem_read(xenos_cp *cp, uint32_t addr)
{
    uint32_t raw = cp->read32 ? cp->read32(cp->ctx, addr & ~3u & 0x1FFFFFFFu) : 0;
    return gpu_swap(__builtin_bswap32(raw), addr & 3);
}

static uint32_t constant_base(uint32_t type)
{
    switch (type) {
    case 0: return 0x4000; /* ALU */
    case 1: return 0x4800; /* FETCH */
    case 2: return 0x4900; /* BOOL */
    case 3: return 0x4908; /* LOOP */
    case 4: return 0x2000; /* REGISTERS */
    default: return 0xFFFFFFFFu;
    }
}

static int compare(uint32_t func, uint32_t value, uint32_t ref)
{
    switch (func & 7) {
    case 0: return 0;           /* never */
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return 1;          /* always */
    }
}

/* a packet source: a ring (wraps) or a linear buffer, in physical memory */
typedef struct {
    uint32_t base, size_dw, pos; /* pos: dwords from base, modulo size_dw */
} reader;

static uint32_t rd(xenos_cp *cp, reader *r)
{
    uint32_t a = r->base + 4 * (r->pos % r->size_dw);
    r->pos = (r->pos + 1) % r->size_dw;
    return cp->read32 ? cp->read32(cp->ctx, a) : 0;
}

#define TRACE(...)                                                                                                     \
    do {                                                                                                               \
        if (cp->trace)                                                                                                 \
            fprintf(cp->trace, __VA_ARGS__);                                                                           \
    } while (0)

static void draw(xenos_cp *cp, reader *r, uint32_t words, const char *name)
{
    xenos_draw d;
    memset(&d, 0, sizeof d);
    uint32_t init = words ? rd(cp, r) : 0;
    xenos_cp_write_reg(cp, XENOS_REG_VGT_DRAW_INITIATOR, init);
    d.prim_type = init & 0x3F;
    d.source_select = (init >> 6) & 3;
    d.index_size = (init >> 11) & 1; /* 0: 16-bit, 1: 32-bit */
    d.num_indices = init >> 16;
    uint32_t used = 1;
    if (d.source_select == 0 && words >= 3) { /* DMA: index buffer base and size */
        d.dma_base = rd(cp, r);
        uint32_t size = rd(cp, r);
        d.dma_words = size & 0xFFFFFF;
        xenos_cp_write_reg(cp, XENOS_REG_VGT_DMA_BASE, d.dma_base);
        xenos_cp_write_reg(cp, XENOS_REG_VGT_DMA_SIZE, size); /* the swap mode (bits 30-31) kept */
        used = 3;
    }
    /* immediate indices: copied out of the packet (the ring may wrap inside it) for the renderer */
    uint32_t *imm = NULL;
    if (d.source_select == 1 && used < words && (imm = malloc(4 * (size_t)(words - used))) != NULL)
        while (used < words) {
            imm[d.imm_words++] = rd(cp, r);
            used++;
        }
    for (; used < words; used++)
        rd(cp, r);
    cp->last_draw = d; /* no pointer into a buffer that goes away */
    d.imm = imm;
    cp->draws++;
    if (cp->draw_hook)
        cp->draw_hook(cp->render_ctx, cp, &d);
    free(imm);
    TRACE("draw %s prim %u source %s indices %u%s", name, d.prim_type,
          d.source_select == 0 ? "dma" : d.source_select == 1 ? "immediate" : "auto", d.num_indices,
          d.source_select == 0 ? "" : "\n");
    if (d.source_select == 0)
        TRACE(" (%s, base %08X, %u words)\n", d.index_size ? "32-bit" : "16-bit", d.dma_base, d.dma_words);
}

static int packet(xenos_cp *cp, reader *r, int in_ring, int depth);

/* run the indirect buffer at nesting level d from where it stands; returns 1 when a wait in it (or in a buffer it
 * calls) does not hold: its position is kept in cp->ib and xenos_cp_set_wptr goes on from there */
static int run_ib(xenos_cp *cp, int d, int in_ring)
{
    xenos_ib *ib = &cp->ib[d];
    reader r = {ib->base, ib->len, ib->pos};
    while (ib->done < ib->len) {
        uint32_t before = r.pos;
        int stall = packet(cp, &r, in_ring, d + 1);
        uint32_t step = (r.pos + ib->len - before) % ib->len;
        if (stall && !step && cp->ib_depth > (uint32_t)d + 1)
            step = ib->len; /* a nested buffer stalled: its packet (all of this buffer) is behind us */
        ib->done += step;
        ib->pos = r.pos;
        if (stall)
            return 1;
        if (!step)
            break;
    }
    return 0;
}

/* run one packet; returns 0, or 1 when the command processor must stop here (an unmet wait under the ring: the
 * wait itself is retried, an indirect buffer that stalled is resumed from cp->ib) */
static int packet(xenos_cp *cp, reader *r, int in_ring, int depth)
{
    uint32_t start = r->pos;
    uint32_t h = rd(cp, r);
    uint32_t type = h >> 30;
    cp->packets++;
    if (type == 0) {
        uint32_t count = ((h >> 16) & 0x3FFF) + 1, index = h & 0x7FFF, one = (h >> 15) & 1;
        TRACE("type0 reg %04X x%u%s\n", index, count, one ? " (one register)" : "");
        for (uint32_t m = 0; m < count; m++)
            xenos_cp_write_reg(cp, one ? index : index + m, rd(cp, r));
        return 0;
    }
    if (type == 1) {
        uint32_t i1 = h & 0x7FF, i2 = (h >> 11) & 0x7FF;
        uint32_t v1 = rd(cp, r), v2 = rd(cp, r);
        xenos_cp_write_reg(cp, i1, v1);
        xenos_cp_write_reg(cp, i2, v2);
        TRACE("type1 reg %04X reg %04X\n", i1, i2);
        return 0;
    }
    if (type == 2)
        return 0;
    uint32_t op = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
    uint32_t end = (start + 1 + count) % r->size_dw;
    cp->op_count[op]++;
    if (h & 1) {
        /* a predicated packet: run only for a bin that is selected (Xenia: skipped unless bin_select & bin_mask;
         * a predicated swap is never run) */
        cp->predicated++;
        if (!(cp->bin_select & cp->bin_mask) || op == PM4_XE_SWAP) {
            cp->predicated_skipped++;
            TRACE("type3 opcode %02X x%u predicated: skipped\n", op, count);
            r->pos = end;
            return 0;
        }
    }
    switch (op) {
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD: {
        uint32_t ptr = rd(cp, r) & 0x1FFFFFFFu, len = rd(cp, r) & 0xFFFFF;
        TRACE("indirect buffer %08X, %u words\n", ptr, len);
        if (depth < XENOS_IB_DEPTH && len) {
            cp->ib[depth] = (xenos_ib){ptr, len, 0, 0};
            cp->ib_depth = depth + 1;
            if (run_ib(cp, depth, in_ring)) {
                r->pos = end; /* this packet is done with: the buffer it started goes on from cp->ib */
                return 1;
            }
            cp->ib_depth = depth;
        }
        break;
    }
    case PM4_WAIT_REG_MEM: {
        uint32_t info = rd(cp, r), addr = rd(cp, r), ref = rd(cp, r), mask = rd(cp, r);
        rd(cp, r); /* wait interval */
        uint32_t v = info & 0x10 ? mem_read(cp, addr) : addr < XENOS_REGS ? cp->regs[addr] : 0;
        int ok = compare(info, v & mask, ref);
        TRACE("wait %s %08X & %08X vs %08X: %s\n", info & 0x10 ? "memory" : "register", addr, mask, ref,
              ok ? "met" : in_ring ? "stall" : "not met (not under the ring: passed)");
        if (!ok && in_ring) {
            r->pos = start;
            cp->stalled = 1;
            cp->packets--; /* retried: counted when it holds */
            cp->op_count[op]--;
            return 1;
        }
        break;
    }
    case PM4_REG_RMW: {
        uint32_t info = rd(cp, r), and_mask = rd(cp, r), or_mask = rd(cp, r);
        uint32_t reg = info & 0x1FFF, v = cp->regs[reg];
        v &= info >> 31 ? cp->regs[and_mask & 0x1FFF] : and_mask;
        v |= (info >> 30) & 1 ? cp->regs[or_mask & 0x1FFF] : or_mask;
        xenos_cp_write_reg(cp, reg, v);
        TRACE("reg_rmw %04X = %08X\n", reg, v);
        break;
    }
    case PM4_REG_TO_MEM: {
        uint32_t reg = rd(cp, r), addr = rd(cp, r);
        mem_write(cp, addr, reg < XENOS_REGS ? cp->regs[reg] : 0);
        TRACE("reg_to_mem %04X -> %08X\n", reg, addr);
        break;
    }
    case PM4_MEM_WRITE: {
        uint32_t addr = rd(cp, r);
        TRACE("mem_write %08X x%u\n", addr, count - 1);
        for (uint32_t k = 1; k < count; k++, addr += 4)
            mem_write(cp, addr, rd(cp, r));
        break;
    }
    case PM4_COND_WRITE: {
        uint32_t info = rd(cp, r), poll = rd(cp, r), ref = rd(cp, r), mask = rd(cp, r), dst = rd(cp, r),
                 data = rd(cp, r);
        uint32_t v = info & 0x10 ? mem_read(cp, poll) : poll < XENOS_REGS ? cp->regs[poll] : 0;
        if (compare(info, v & mask, ref)) {
            if (info & 0x100)
                mem_write(cp, dst, data);
            else
                xenos_cp_write_reg(cp, dst, data);
        }
        TRACE("cond_write %s\n", compare(info, v & mask, ref) ? "written" : "skipped");
        break;
    }
    case PM4_EVENT_WRITE: {
        uint32_t init = rd(cp, r);
        xenos_cp_write_reg(cp, XENOS_REG_VGT_EVENT_INITIATOR, init & 0x3F);
        TRACE("event %u\n", init & 0x3F);
        break;
    }
    case PM4_EVENT_WRITE_SHD: {
        uint32_t init = rd(cp, r), addr = rd(cp, r), value = rd(cp, r);
        xenos_cp_write_reg(cp, XENOS_REG_VGT_EVENT_INITIATOR, init & 0x3F);
        uint32_t data = init >> 31 ? cp->counter : value;
        mem_write(cp, addr, data);
        TRACE("event_shd %u -> %08X = %08X\n", init & 0x3F, addr & ~3u, data);
        break;
    }
    case PM4_EVENT_WRITE_EXT: {
        uint32_t init = rd(cp, r), addr = rd(cp, r);
        xenos_cp_write_reg(cp, XENOS_REG_VGT_EVENT_INITIATOR, init & 0x3F);
        /* screen extents of the previous draws: the whole 8192-texel range, z 0..1 (as Xenia fakes it), six
         * big-endian 16-bit values */
        static const uint16_t ext[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
        for (int k = 0; k < 6; k += 2)
            if (cp->write32)
                cp->write32(cp->ctx, ((addr & ~3u) + 2 * (uint32_t)k) & 0x1FFFFFFFu,
                            (uint32_t)ext[k] << 16 | ext[k + 1]);
        TRACE("event_ext %u -> %08X\n", init & 0x3F, addr & ~3u);
        break;
    }
    case PM4_DRAW_INDX:
        if (count >= 2) {
            rd(cp, r); /* viz query condition */
            draw(cp, r, count - 1, "DRAW_INDX");
        }
        break;
    case PM4_DRAW_INDX_2:
        draw(cp, r, count, "DRAW_INDX_2");
        break;
    case PM4_SET_CONSTANT: {
        uint32_t ot = rd(cp, r), base = constant_base((ot >> 16) & 0xFF);
        if (base != 0xFFFFFFFFu) {
            uint32_t index = base + (ot & 0x7FF);
            for (uint32_t k = 1; k < count; k++)
                xenos_cp_write_reg(cp, index++, rd(cp, r));
            TRACE("set_constant %04X x%u\n", base + (ot & 0x7FF), count - 1);
        }
        break;
    }
    case PM4_SET_CONSTANT2:
    case PM4_SET_SHADER_CONSTANTS: {
        uint32_t index = rd(cp, r) & 0xFFFF;
        TRACE("%s %04X x%u\n", xenos_cp_opcode_name(op), index, count - 1);
        for (uint32_t k = 1; k < count; k++)
            xenos_cp_write_reg(cp, index++, rd(cp, r));
        break;
    }
    case PM4_LOAD_ALU_CONSTANT: {
        uint32_t addr = rd(cp, r) & 0x3FFFFFFFu, ot = rd(cp, r), n = rd(cp, r) & 0xFFF;
        uint32_t base = constant_base((ot >> 16) & 0xFF);
        if (base != 0xFFFFFFFFu)
            for (uint32_t k = 0; k < n; k++)
                xenos_cp_write_reg(cp, base + (ot & 0x7FF) + k,
                                   cp->read32 ? cp->read32(cp->ctx, (addr + 4 * k) & 0x1FFFFFFFu) : 0);
        TRACE("load_alu_constant %08X -> %04X x%u\n", addr, base + (ot & 0x7FF), n);
        break;
    }
    case PM4_IM_LOAD:
    case PM4_IM_LOAD_IMMEDIATE: {
        /* shader microcode: (type | address) or (type), then start << 16 | size in dwords; the words follow the
         * packet (immediate) or sit at the address (physical) */
        uint32_t at = rd(cp, r), ss = rd(cp, r), n = ss & 0xFFFF, type = op == PM4_IM_LOAD ? at & 3 : at;
        xenos_shader *sh = &cp->shader[type == 0 ? 0 : 1];
        if (n > XENOS_SHADER_WORDS)
            n = XENOS_SHADER_WORDS;
        sh->words = n;
        sh->address = op == PM4_IM_LOAD ? at & ~3u : 0;
        for (uint32_t k = 0; k < n; k++)
            sh->code[k] = op == PM4_IM_LOAD ? (cp->read32 ? cp->read32(cp->ctx, ((at & ~3u) + 4 * k) & 0x1FFFFFFFu) : 0)
                                            : rd(cp, r);
        sh->loads++;
        cp->shader_loads++;
        if (cp->shader_loaded)
            cp->shader_loaded(cp->ctx, type != 0, sh->code, n);
        TRACE("%s %s start %u, %u words\n", xenos_cp_opcode_name(op), type == 0 ? "vertex" : "pixel", ss >> 16,
              ss & 0xFFFF);
        break;
    }
    case PM4_INTERRUPT: {
        uint32_t mask = rd(cp, r);
        cp->interrupts++;
        TRACE("interrupt cpus %02X\n", mask);
        for (uint32_t cpu = 0; cpu < 6; cpu++)
            if ((mask >> cpu) & 1 && cp->interrupt)
                cp->interrupt(cp->ctx, 1, cpu);
        break;
    }
    case PM4_SET_BIN_MASK_LO:
        cp->bin_mask = (cp->bin_mask & 0xFFFFFFFF00000000ull) | rd(cp, r);
        break;
    case PM4_SET_BIN_MASK_HI:
        cp->bin_mask = (cp->bin_mask & 0xFFFFFFFFull) | (uint64_t)rd(cp, r) << 32;
        break;
    case PM4_SET_BIN_SELECT_LO:
        cp->bin_select = (cp->bin_select & 0xFFFFFFFF00000000ull) | rd(cp, r);
        break;
    case PM4_SET_BIN_SELECT_HI:
        cp->bin_select = (cp->bin_select & 0xFFFFFFFFull) | (uint64_t)rd(cp, r) << 32;
        break;
    case PM4_SET_BIN_MASK:
    case PM4_SET_BIN_SELECT:
        if (count >= 2) {
            uint64_t hi = rd(cp, r), lo = rd(cp, r);
            *(op == PM4_SET_BIN_MASK ? &cp->bin_mask : &cp->bin_select) = hi << 32 | lo;
        }
        break;
    case PM4_XE_SWAP: {
        rd(cp, r); /* magic */
        cp->last_swap_fb = rd(cp, r);
        cp->last_swap_w = rd(cp, r);
        cp->last_swap_h = rd(cp, r);
        cp->swaps++;
        cp->counter++;
        if (cp->swap_hook)
            cp->swap_hook(cp->render_ctx, cp);
        TRACE("swap front buffer %08X %ux%u\n", cp->last_swap_fb, cp->last_swap_w, cp->last_swap_h);
        break;
    }
    default:
        if (!xenos_cp_opcode_name(op)) {
            cp->unknown++;
            TRACE("type3 opcode %02X (unknown) x%u\n", op, count);
        } else if (op != PM4_NOP) {
            TRACE("%s x%u\n", xenos_cp_opcode_name(op), count);
        }
        break;
    }
    r->pos = end; /* whatever an opcode read, the next packet starts after its count */
    return 0;
}

uint32_t xenos_cp_execute(xenos_cp *cp, uint32_t phys, uint32_t ndwords)
{
    if (!ndwords)
        return 0;
    reader r = {phys & 0x1FFFFFFFu, ndwords, 0};
    uint32_t done = 0, n = 0;
    while (done < ndwords) {
        uint32_t before = r.pos;
        packet(cp, &r, 0, 0);
        n++;
        uint32_t step = (r.pos + ndwords - before) % ndwords;
        if (!step)
            break;
        done += step;
    }
    return n;
}

/* words the packet at pos needs, header included (0 when its header says nothing useful) */
static uint32_t packet_words(xenos_cp *cp, reader *r)
{
    reader t = *r;
    uint32_t h = rd(cp, &t), type = h >> 30;
    if (type == 0 || type == 3)
        return ((h >> 16) & 0x3FFF) + 2;
    if (type == 1)
        return 3;
    return 1;
}

uint32_t xenos_cp_set_wptr(xenos_cp *cp, uint32_t wptr)
{
    if (!cp->rb_size)
        return 0;
    uint32_t size_dw = cp->rb_size / 4, n = 0;
    cp->wptr = wptr % size_dw;
    cp->stalled = 0;
    reader r = {cp->rb_base, size_dw, cp->rptr};
    /* indirect buffers a wait stopped: the innermost first, then those that called it */
    while (cp->ib_depth && !cp->stalled) {
        if (!run_ib(cp, (int)cp->ib_depth - 1, 1))
            cp->ib_depth--;
    }
    while (!cp->stalled && r.pos != cp->wptr) {
        uint32_t avail = (cp->wptr + size_dw - r.pos) % size_dw;
        if (packet_words(cp, &r) > avail)
            break; /* the CPU has not written the whole packet yet */
        if (packet(cp, &r, 1, 0))
            break;
        n++;
    }
    cp->rptr = r.pos;
    if (cp->rptr_writeback && cp->write32)
        cp->write32(cp->ctx, cp->rptr_writeback & 0x1FFFFFFFu, cp->rptr);
    return n;
}
