/* gc_bus.c -- the GameCube spike's memory map off MEM1 and its hardware registers (XBA-225).
 *
 * Physical addresses (gc_mmu.c translates effective ones; EA & 0x3FFFFFFF with the default BATs):
 *   0x00000000  MEM1, 24 MB (inline in gc_recomp_rt.h; here only when an access crosses its end)
 *   0x0C000000  CP, PE (16-bit): the GPU's command processor and pixel engine (gx/gx_fifo.c, XBA-262)
 *   0x0C002000  VI (16-bit): timing registers stored, beam position computed, display interrupts DI0..DI3 raised on
 *               their line, framebuffer address and picture configuration read by gc_xfb_rgb
 *   0x0C003000  PI (32-bit): interrupt cause / mask, the reset switch (never pressed), the Flipper revision
 *   0x0C004000  MI (16-bit): stored, no memory protection
 *   0x0C005000  DSP (16-bit): mailboxes, control register, ARAM DMA, audio DMA, the microcode's HLE: gc_dsp.c
 *               (XBA-263)
 *   0x0C006000  DI (32-bit): the DVD drive, gc_di.c (XBA-261)
 *   0x0C006400  SI: standard controllers (gc_si.c, XBA-259); EXI (32-bit): memory cards, RTC, SRAM (gc_exi.c,
 *               XBA-264)
 *   0x0C006C00  AI (32-bit): rates, streaming volume and sample counter, gc_ai.c (XBA-263)
 *   0x0C008000  the write-gather pipe: into the GX FIFO (gx_fifo_write, XBA-262)
 *   0x08000000  the EFB as the CPU peeks and pokes it (gx_efb_peek / gx_efb_poke)
 *   0x20000000  the locked cache (16 KB; gc_mmu.c maps EA 0xE0000000 onto it while HID2[LCE] is set)
 * Anything else reads 0 and drops writes (logged with -v). */
#include <stdio.h>
#include <string.h>

#include "gc_internal.h"

/* ---- interrupts ---- */

static void vi_int_update(gc_machine *m)
{
    bool on = false;
    for (int n = 0; n < 4; n++) {
        uint16_t hi = m->vi[0x18 + 2 * n];
        on |= (hi & 0x8000) && (hi & 0x1000);
    }
    if (on)
        m->pi_intsr |= GC_INT_VI;
    else
        m->pi_intsr &= ~GC_INT_VI;
}

/* A line starts: DIn whose line it is raise their interrupt. */
void gc_vi_line(gc_machine *m)
{
    m->line = (m->line + 1) % m->frame_lines;
    uint32_t vct = m->line + 1;
    for (int n = 0; n < 4; n++) {
        uint16_t *hi = &m->vi[0x18 + 2 * n];
        if ((*hi & 0x1000) && (uint32_t)(*hi & 0x3FF) == vct)
            *hi |= 0x8000;
    }
    vi_int_update(m);
    gc_check_interrupts(m);
}

/* ---- 16-bit register blocks ---- */

static uint16_t vi_read16(gc_machine *m, unsigned idx)
{
    switch (idx) {
    case 0x16: /* VCT */
        return (uint16_t)(m->line + 1);
    case 0x17: { /* HCT */
        uint64_t start = m->next_line - m->line_cycles;
        uint64_t in = m->h.now > start ? m->h.now - start : 0;
        return (uint16_t)(1 + in * 858 / m->line_cycles);
    }
    default:
        return m->vi[idx];
    }
}

static void vi_write16(gc_machine *m, unsigned idx, uint16_t v)
{
    if (m->verbose >= 2)
        fprintf(stderr, "gc: VI[%02X] = %04X\n", idx * 2, v);
    m->vi[idx] = v;
    if (idx >= 0x18 && idx < 0x20)
        vi_int_update(m), gc_check_interrupts(m);
}

/* ---- 32-bit register blocks ---- */

static uint32_t pi_read32(gc_machine *m, unsigned idx)
{
    switch (idx) {
    case 0:
        return m->pi_intsr | 0x00010000u; /* RSWST: the reset switch is up */
    case 1:
        return m->pi_intmr;
    case 11:
        return 0x246500B1u; /* Flipper revision C */
    case 3: case 4: case 5: /* the CPU's GX FIFO: base, end, write pointer */
        return gx_pi_read(m->gx, idx);
    default:
        return m->pi_reg[idx];
    }
}

static void pi_write32(gc_machine *m, unsigned idx, uint32_t v)
{
    switch (idx) {
    case 0:
        m->pi_intsr &= ~(v & (GC_INT_ERR | GC_INT_RSW));
        break;
    case 1:
        m->pi_intmr = v;
        break;
    case 3: case 4: case 5:
        gx_pi_write(m->gx, idx, v);
        break;
    default:
        m->pi_reg[idx] = v;
        break;
    }
    gc_check_interrupts(m);
}

/* ---- dispatch ---- */

static uint16_t hw_read16(gc_machine *m, uint32_t pa)
{
    uint32_t off = pa & 0xFFFF;
    if (off >= 0x2000 && off < 0x2100)
        return vi_read16(m, (off & 0xFF) >> 1);
    if (off >= 0x5000 && off < 0x5200)
        return gc_dsp_read16(m, (off & 0x1FF) >> 1); /* gc_dsp.c (XBA-263) */
    if (off >= 0x4000 && off < 0x4080)
        return (uint16_t)m->mi_reg[(off & 0x7F) >> 1];
    if (off < 0x1000)
        return gx_cp_read16(m->gx, off);
    if (off < 0x2000)
        return gx_pe_read16(m->gx, off);
    if (off < 0x3000 || (off >= 0x4000 && off < 0x6000) || off >= 0x7000)
        return 0; /* CP, PE, holes */
    /* a 32-bit block read 16 bits at a time */
    uint32_t v = gc_hw_read(m, pa & ~3u, 4);
    return (uint16_t)((pa & 2) ? v : v >> 16);
}

static void hw_write16(gc_machine *m, uint32_t pa, uint16_t v)
{
    uint32_t off = pa & 0xFFFF;
    if (off >= 0x2000 && off < 0x2100) {
        vi_write16(m, (off & 0xFF) >> 1, v);
        return;
    }
    if (off >= 0x5000 && off < 0x5200) {
        gc_dsp_write16(m, (off & 0x1FF) >> 1, v);
        return;
    }
    if (off >= 0x4000 && off < 0x4080) {
        m->mi_reg[(off & 0x7F) >> 1] = v;
        return;
    }
    if (off < 0x1000) {
        gx_cp_write16(m->gx, off, v);
        return;
    }
    if (off < 0x2000) {
        gx_pe_write16(m->gx, off, v);
        return;
    }
    if (off < 0x3000 || (off >= 0x4000 && off < 0x6000) || off >= 0x7000)
        return; /* CP, PE, holes */
    uint32_t cur = gc_hw_read(m, pa & ~3u, 4);
    cur = (pa & 2) ? (cur & 0xFFFF0000u) | v : (cur & 0xFFFFu) | (uint32_t)v << 16;
    gc_hw_write(m, pa & ~3u, cur, 4);
}

uint32_t gc_hw_read(gc_machine *m, uint32_t pa, unsigned size)
{
    uint32_t off = pa & 0xFFFF;
    if (size == 1) {
        uint16_t h = hw_read16(m, pa & ~1u);
        return (pa & 1) ? h & 0xFF : h >> 8;
    }
    if (size == 2)
        return hw_read16(m, pa);
    if (off < 0x3000 || (off >= 0x4000 && off < 0x6000)) /* 16-bit blocks */
        return (uint32_t)hw_read16(m, pa) << 16 | hw_read16(m, pa + 2);
    if (off >= 0x3000 && off < 0x3100)
        return pi_read32(m, (off & 0xFF) >> 2);
    if (off >= 0x6000 && off < 0x6040)
        return m->di ? gc_di_read32(m, (off & 0x3F) >> 2) : 0; /* DI (gc_di.c); no disc yet: no drive */
    if (off >= 0x6040 && off < 0x6400)
        return 0;
    if (off >= 0x6400 && off < 0x6500)
        return gc_si_read(m, (off & 0xFF) >> 2);
    if (off >= 0x6800 && off < 0x683C)
        return gc_exi_read32(m, (off & 0x3F) >> 2);
    if (off >= 0x6C00 && off < 0x6C20)
        return gc_ai_read32(m, (off & 0x1F) >> 2); /* gc_ai.c (XBA-263) */
    if (m->verbose)
        fprintf(stderr, "gc: read%u of unmapped HW %08X\n", size * 8, pa);
    return 0;
}

void gc_hw_write(gc_machine *m, uint32_t pa, uint32_t v, unsigned size)
{
    uint32_t off = pa & 0xFFFF;
    if (off >= 0x8000) { /* the write-gather pipe */
        gx_fifo_write(m->gx, v, size);
        return;
    }
    if (size == 1) {
        uint16_t h = hw_read16(m, pa & ~1u);
        h = (pa & 1) ? (uint16_t)((h & 0xFF00) | (v & 0xFF)) : (uint16_t)((h & 0xFF) | (v & 0xFF) << 8);
        hw_write16(m, pa & ~1u, h);
        return;
    }
    if (size == 2) {
        hw_write16(m, pa, (uint16_t)v);
        return;
    }
    if (off < 0x3000 || (off >= 0x4000 && off < 0x6000)) {
        hw_write16(m, pa, (uint16_t)(v >> 16));
        hw_write16(m, pa + 2, (uint16_t)v);
        return;
    }
    if (m->verbose >= 2)
        fprintf(stderr, "gc: HW %08X = %08X\n", pa, v);
    if (off >= 0x3000 && off < 0x3100)
        pi_write32(m, (off & 0xFF) >> 2, v);
    else if (off >= 0x6000 && off < 0x6040)
        gc_di_write32(m, (off & 0x3F) >> 2, v);
    else if (off >= 0x6040 && off < 0x6400)
        ;
    else if (off >= 0x6400 && off < 0x6500)
        gc_si_write(m, (off & 0xFF) >> 2, v);
    else if (off >= 0x6800 && off < 0x683C)
        gc_exi_write32(m, (off & 0x3F) >> 2, v);
    else if (off >= 0x6C00 && off < 0x6C20)
        gc_ai_write32(m, (off & 0x1F) >> 2, v);
    else if (m->verbose)
        fprintf(stderr, "gc: write%u to unmapped HW %08X = %08X\n", size * 8, pa, v);
}

/* ---- physical accesses off the inline MEM1 path (gc_mmu.c translates first) ---- */

uint32_t gc_phys_read(gc_machine *m, uint32_t pa, unsigned size)
{
    if (pa < GC_RAM_SIZE) { /* crosses the end of MEM1 */
        uint32_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v = v << 8 | (pa + i < GC_RAM_SIZE ? m->h.ram[pa + i] : 0);
        return v;
    }
    if ((pa & 0xFFFF0000u) == 0x0C000000u)
        return gc_hw_read(m, pa, size);
    if ((pa & 0xFF000000u) == 0x08000000u) {
        uint32_t v = gx_efb_peek(m->gx, pa & ~3u);
        return size == 4 ? v : size == 2 ? (v >> (16 - 8 * (pa & 2))) & 0xFFFF : (v >> (24 - 8 * (pa & 3))) & 0xFF;
    }
    if (pa >= 0x20000000u && pa + size <= 0x20000000u + GC_LC_SIZE) {
        uint32_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v = v << 8 | m->lc[pa - 0x20000000u + i];
        return v;
    }
    if (m->verbose)
        fprintf(stderr, "gc: read%u of unmapped %08X (pc %08X)\n", size * 8, pa, m->h.cpu.pc);
    return 0;
}

void gc_phys_write(gc_machine *m, uint32_t pa, uint32_t v, unsigned size)
{
    if (pa < GC_RAM_SIZE) {
        for (unsigned i = 0; i < size; i++)
            if (pa + i < GC_RAM_SIZE)
                m->h.ram[pa + i] = (uint8_t)(v >> (8 * (size - 1 - i)));
        return;
    }
    if ((pa & 0xFFFF0000u) == 0x0C000000u) {
        gc_hw_write(m, pa, v, size);
        return;
    }
    if ((pa & 0xFF000000u) == 0x08000000u) {
        gx_efb_poke(m->gx, pa & ~3u, v, size);
        return;
    }
    if (pa >= 0x20000000u && pa + size <= 0x20000000u + GC_LC_SIZE) {
        for (unsigned i = 0; i < size; i++)
            m->lc[pa - 0x20000000u + i] = (uint8_t)(v >> (8 * (size - 1 - i)));
        return;
    }
    if (m->verbose)
        fprintf(stderr, "gc: write%u to unmapped %08X = %08X (pc %08X)\n", size * 8, pa, v, m->h.cpu.pc);
}
