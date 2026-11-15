/* Placeholder RSP (n64_devices.h), built when n64_rsp.c (N-5) is absent, so the core builds and boots without it: IMEM
 * / DMEM, the SP registers, SP DMA in both directions (length / count / skip, instantly complete, RDRAM beyond its size
 * reads as zeros: IPL3 clears memory that way), SP_STATUS with its halt / broke / interrupt / signal bits, the
 * semaphore and SP_PC. There is no scalar or vector unit: once the CPU starts the RSP it halts at once with BROKE set
 * (and the SP interrupt if "interrupt on break" is set), as if the microcode ended immediately. Enough for IPL3 and
 * libdragon to reach main without graphics or audio microcode. */
#include "n64_devices.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    ST_HALT = 1u << 0,
    ST_BROKE = 1u << 1,
    ST_INTR_BREAK = 1u << 6,
};

struct n64_rsp {
    n64_rcp_bus bus;
    n64_rdp *dp;
    uint8_t mem[0x2000];
    uint32_t mem_addr, dram_addr, rd_len, wr_len;
    uint32_t status;
    uint32_t semaphore;
    uint32_t pc;
};

n64_rsp *n64_rsp_create(n64_rcp_bus bus)
{
    n64_rsp *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->bus = bus;
    n64_rsp_reset(s);
    return s;
}

void n64_rsp_destroy(n64_rsp *s) { free(s); }

void n64_rsp_reset(n64_rsp *s)
{
    memset(s->mem, 0, sizeof s->mem);
    s->mem_addr = s->dram_addr = 0;
    s->rd_len = s->wr_len = 0xFF8;
    s->status = ST_HALT;
    s->semaphore = 0;
    s->pc = 0;
}

void n64_rsp_attach_rdp(n64_rsp *s, n64_rdp *dp) { s->dp = dp; }

/* Save states (XBA-183): everything after the host side (the bus, the RDP). */
#define RSP_STATE_OFF offsetof(n64_rsp, mem)
#define RSP_STATE_SIZE (sizeof(n64_rsp) - RSP_STATE_OFF)
size_t n64_rsp_state_size(n64_rsp *s)
{
    (void)s;
    return RSP_STATE_SIZE;
}
void n64_rsp_state_save(n64_rsp *s, uint8_t *out) { memcpy(out, (uint8_t *)s + RSP_STATE_OFF, RSP_STATE_SIZE); }
int n64_rsp_state_load(n64_rsp *s, const uint8_t *in, size_t n)
{
    if (n != RSP_STATE_SIZE)
        return -1;
    memcpy((uint8_t *)s + RSP_STATE_OFF, in, RSP_STATE_SIZE);
    return 0;
}
uint8_t *n64_rsp_mem(n64_rsp *s) { return s->mem; }
int n64_rsp_halted(n64_rsp *s) { return (s->status & ST_HALT) != 0; }
uint32_t n64_rsp_get_gpr(n64_rsp *s, int r)
{
    (void)s;
    (void)r;
    return 0; /* no scalar unit */
}
void n64_rsp_set_gpr(n64_rsp *s, int r, uint32_t v)
{
    (void)s;
    (void)r;
    (void)v;
}
int n64_rsp_semaphore_held(n64_rsp *s) { return s->semaphore != 0; }

static void dma(n64_rsp *s, uint32_t v, int to_rdram)
{
    uint32_t len = (v & 0xFFF) + 1, count = ((v >> 12) & 0xFF) + 1, skip = (v >> 20) & 0xFFF;
    len = (len + 7) & ~7u;
    uint32_t bank = s->mem_addr & 0x1000, mem = s->mem_addr & 0xFF8;
    uint32_t dram = s->dram_addr & 0xFFFFF8;
    for (uint32_t c = 0; c < count; c++) {
        for (uint32_t i = 0; i < len; i++) {
            uint8_t *p = s->mem + bank + ((mem + i) & 0xFFF);
            uint32_t a = dram + i;
            if (to_rdram) {
                if (a < s->bus.rdram_size) {
                    s->bus.rdram[a] = *p;
                    if (s->bus.wrote)
                        s->bus.wrote(s->bus.ctx, a, 1);
                }
            } else {
                *p = a < s->bus.rdram_size ? s->bus.rdram[a] : 0;
            }
        }
        mem = (mem + len) & 0xFF8;
        dram = (dram + len + skip) & 0xFFFFF8;
    }
    s->mem_addr = bank | mem;
    s->dram_addr = dram;
    uint32_t done = (skip << 20) | 0xFF8;
    if (to_rdram)
        s->wr_len = done;
    else
        s->rd_len = done;
}

uint32_t n64_rsp_read_reg(n64_rsp *s, uint32_t reg)
{
    switch (reg) {
    case 0: return s->mem_addr;
    case 1: return s->dram_addr;
    case 2: return s->rd_len;
    case 3: return s->wr_len;
    case 4: return s->status;
    case 5: case 6: return 0; /* DMA_FULL, DMA_BUSY: DMAs complete at once */
    case 7: {
        uint32_t v = s->semaphore;
        s->semaphore = 1;
        return v;
    }
    case 8: return s->pc;
    default: return 0;
    }
}

void n64_rsp_write_reg(n64_rsp *s, uint32_t reg, uint32_t v)
{
    switch (reg) {
    case 0: s->mem_addr = v & 0x1FF8; break;
    case 1: s->dram_addr = v & 0xFFFFF8; break;
    case 2: dma(s, v, 0); break;
    case 3: dma(s, v, 1); break;
    case 4: { /* each set / clear pair: one alone acts, both at once change nothing */
        uint32_t st = s->status;
#define PAIR(clr, set, bit)                                                                                           \
    do {                                                                                                              \
        int c_ = (v >> (clr)) & 1, s_ = (v >> (set)) & 1;                                                             \
        if (c_ && !s_) st &= ~(bit);                                                                                  \
        if (s_ && !c_) st |= (bit);                                                                                   \
    } while (0)
        PAIR(0, 1, ST_HALT);
        if (v & (1u << 2))
            st &= ~ST_BROKE;
        if ((v & (1u << 3)) && !(v & (1u << 4)))
            s->bus.clear(s->bus.ctx, N64_MI_SP);
        if ((v & (1u << 4)) && !(v & (1u << 3)))
            s->bus.raise(s->bus.ctx, N64_MI_SP);
        PAIR(5, 6, 1u << 5);
        PAIR(7, 8, ST_INTR_BREAK);
        for (int i = 0; i < 8; i++)
            PAIR(9 + 2 * i, 10 + 2 * i, 1u << (7 + i));
#undef PAIR
        s->status = st;
        break;
    }
    case 7: s->semaphore = 0; break;
    case 8: s->pc = v & 0xFFC; break;
    default: break;
    }
}

uint32_t n64_rsp_run(n64_rsp *s, uint32_t cycles)
{
    if (s->status & ST_HALT)
        return 0;
    s->status |= ST_HALT | ST_BROKE;
    if (s->status & ST_INTR_BREAK)
        s->bus.raise(s->bus.ctx, N64_MI_SP);
    return cycles ? 1 : 0;
}
