/* ps2_bus.c -- the EE's memory map as the PS2 spike models it (XBA-226).
 *
 * - main RAM: 32 MB, the low 32 MB of every segment (KUSEG as PS2SDK's TLB maps it, the uncached and
 *   uncached-accelerated mirrors at 0x20000000 / 0x30000000, KSEG0 / KSEG1);
 * - the scratchpad: 16 KB at 0x70000000;
 * - EE registers (0x10000000-0x1000FFFF) and the GS's privileged registers (0x12000000-0x12001FFF): latched (a read
 *   gives what was last written), except the SIO: a byte written to its TX FIFO (0x1000F180) is the program's
 *   output, and its status registers say the FIFO is empty and never full;
 * - anything else (the BIOS ROM, the IOP's memory, VU memory) is unmapped: the access faults and the interpreter
 *   stops on it. */
#include <stdlib.h>
#include <string.h>

#include "ps2_internal.h"

uint8_t *ps2_ptr(ps2_machine *m, uint32_t addr, uint32_t len)
{
    if ((addr & 0x1E000000u) == 0) {
        uint32_t off = addr & 0x01FFFFFFu;
        if (off + len <= PS2_RAM_SIZE)
            return m->h.ram + off;
        return NULL;
    }
    if (addr >= PS2_SCRATCH_BASE && addr - PS2_SCRATCH_BASE + len <= PS2_SCRATCH_SIZE)
        return m->scratch + (addr - PS2_SCRATCH_BASE);
    return NULL;
}

static void sio_out(ps2_machine *m, uint8_t c)
{
    if (c == '\r')
        return;
    if (m->out_len + 2 > m->out_cap) {
        size_t cap = m->out_cap ? m->out_cap * 2 : 4096;
        char *p = realloc(m->out, cap);
        if (!p)
            return;
        m->out = p;
        m->out_cap = cap;
    }
    m->out[m->out_len++] = (char)c;
    m->out[m->out_len] = 0;
    if (m->verbose > 1)
        fputc(c, stderr);
}

/* The latched register file holding addr (physical), or NULL. */
static uint8_t *io_reg(ps2_machine *m, uint32_t phys, int width)
{
    if (phys >= PS2_IO_BASE && phys - PS2_IO_BASE + (uint32_t)width <= PS2_IO_SIZE)
        return m->io + (phys - PS2_IO_BASE);
    if (phys >= PS2_GS_BASE && phys - PS2_GS_BASE + (uint32_t)width <= PS2_GS_SIZE)
        return m->gs + (phys - PS2_GS_BASE);
    return NULL;
}

int ps2_read(ps2_machine *m, uint32_t addr, int width, void *out)
{
    if (addr & (uint32_t)(width - 1))
        return 1;
    uint8_t *p = ps2_ptr(m, addr, (uint32_t)width);
    if (p) {
        memcpy(out, p, (size_t)width);
        return 0;
    }
    uint32_t phys = addr & 0x1FFFFFFFu;
    if ((addr >> 28) == 7) /* the scratchpad's segment, beyond its 16 KB */
        return 1;
    uint8_t *r = io_reg(m, phys, width);
    if (!r)
        return 1;
    memcpy(out, r, (size_t)width);
    if ((phys & ~0xFFu) == 0x1000F100u) { /* SIO: TX FIFO empty (ISR bit 1, LSR bits 5-6), nothing received */
        uint64_t v = 0;
        if ((phys & ~3u) == 0x1000F130u)
            v = 0x02u;
        else if ((phys & ~3u) == 0x1000F110u)
            v = 0x60u;
        memcpy(out, &v, (size_t)(width < 8 ? width : 8));
    }
    return 0;
}

int ps2_write(ps2_machine *m, uint32_t addr, int width, const void *in)
{
    if (addr & (uint32_t)(width - 1))
        return 1;
    uint8_t *p = ps2_ptr(m, addr, (uint32_t)width);
    if (p) {
        memcpy(p, in, (size_t)width);
        return 0;
    }
    if ((addr >> 28) == 7)
        return 1;
    uint32_t phys = addr & 0x1FFFFFFFu;
    if ((phys & ~3u) == PS2_SIO_TXFIFO) {
        sio_out(m, *(const uint8_t *)in);
        return 0;
    }
    uint8_t *r = io_reg(m, phys, width);
    if (!r)
        return 1;
    memcpy(r, in, (size_t)width);
    return 0;
}

uint32_t ps2_r32(ps2_machine *m, uint32_t addr)
{
    uint32_t v = 0;
    (void)ps2_read(m, addr, 4, &v);
    return v;
}

void ps2_w32(ps2_machine *m, uint32_t addr, uint32_t v)
{
    (void)ps2_write(m, addr, 4, &v);
}

uint32_t ps2_peek32(ps2_machine *m, uint32_t addr)
{
    return ps2_r32(m, addr);
}

const char *ps2_sio_output(ps2_machine *m)
{
    return m->out ? m->out : "";
}

/* ---- for lifted code ---------------------------------------------------------------------------------------------- */

int ps2_rc_read(ps2_machine *m, uint32_t addr, int width, void *out)
{
    return ps2_read(m, addr, width, out);
}

int ps2_rc_write(ps2_machine *m, uint32_t addr, int width, const void *in)
{
    return ps2_write(m, addr, width, in);
}

int ps2_rc_unaligned(ps2_machine *m, int kind, uint32_t addr, uint64_t rt, uint64_t *out)
{
    if (kind == 0 || kind == 1 || kind == 4 || kind == 5) {
        uint32_t a = addr & ~3u, w = 0;
        if (ps2_read(m, a, 4, &w))
            return 1;
        switch (kind) {
        case 0: *out = ps2_lwl(rt, w, addr); return 0;
        case 1: *out = ps2_lwr(rt, w, addr); return 0;
        case 4: w = ps2_swl(rt, w, addr); break;
        default: w = ps2_swr(rt, w, addr); break;
        }
        return ps2_write(m, a, 4, &w);
    }
    uint32_t a = addr & ~7u;
    uint64_t d = 0;
    if (ps2_read(m, a, 8, &d))
        return 1;
    switch (kind) {
    case 2: *out = ps2_ldl(rt, d, addr); return 0;
    case 3: *out = ps2_ldr(rt, d, addr); return 0;
    case 6: d = ps2_sdl(rt, d, addr); break;
    case 7: d = ps2_sdr(rt, d, addr); break;
    default: return 1;
    }
    return ps2_write(m, a, 8, &d);
}
