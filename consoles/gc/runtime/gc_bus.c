/* gc_bus.c -- the GameCube spike's memory map off MEM1 and its hardware registers (XBA-225).
 *
 * Physical addresses (EA & 0x3FFFFFFF, gc_recomp_rt.h):
 *   0x00000000  MEM1, 24 MB (inline in gc_recomp_rt.h; here only when an access crosses its end)
 *   0x0C000000  CP, PE (16-bit): stored, no GX
 *   0x0C002000  VI (16-bit): timing registers stored, beam position computed, display interrupts DI0..DI3 raised on
 *               their line, framebuffer address and picture configuration read by gc_xfb_rgb
 *   0x0C003000  PI (32-bit): interrupt cause / mask, the reset switch (never pressed), the Flipper revision
 *   0x0C004000  MI (16-bit): stored, no memory protection
 *   0x0C005000  DSP (16-bit): mailboxes (the DSP answers 0x8071FEED when it leaves halt, as its ROM does), control
 *               register, ARAM DMA (16 MB of ARAM, copies at once)
 *   0x0C006000  DI, SI, EXI, AI (32-bit): transfers complete at once with nothing on the other end (no disc, no
 *               pads, no memory cards; EXI reads give zeros: blank SRAM / RTC)
 *   0x0C008000  GX FIFO: writes dropped
 *   0x20000000  the locked cache (EA 0xE0000000, 16 KB)
 * Anything else reads 0 and drops writes (logged with -v). */
#include <stdio.h>
#include <string.h>

#include "gc_internal.h"

/* ---- interrupts ---- */

static void dsp_int_update(gc_machine *m)
{
    uint16_t cr = m->dsp[5];
    bool on = ((cr & 0x08) && (cr & 0x10)) || ((cr & 0x20) && (cr & 0x40)) || ((cr & 0x80) && (cr & 0x100));
    if (on)
        m->pi_intsr |= GC_INT_DSP;
    else
        m->pi_intsr &= ~GC_INT_DSP;
}

static void exi_int_update(gc_machine *m)
{
    bool on = false;
    for (int ch = 0; ch < 3; ch++) {
        uint32_t csr = m->exi[ch * 5];
        on |= ((csr & 2) && (csr & 1)) || ((csr & 8) && (csr & 4)) || ((csr & 0x800) && (csr & 0x400));
    }
    if (on)
        m->pi_intsr |= GC_INT_EXI;
    else
        m->pi_intsr &= ~GC_INT_EXI;
}

static void si_int_update(gc_machine *m)
{
    uint32_t cs = m->si[13];
    bool on = ((cs & 0x80000000u) && (cs & 0x40000000u)) || ((cs & 0x20000000u) && (cs & 0x08000000u));
    if (on)
        m->pi_intsr |= GC_INT_SI;
    else
        m->pi_intsr &= ~GC_INT_SI;
}

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
    m->line = (m->line + 1) % GC_LINES;
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
        uint64_t start = m->next_line - GC_CYCLES_PER_LINE;
        uint64_t in = m->h.now > start ? m->h.now - start : 0;
        return (uint16_t)(1 + in * 858 / GC_CYCLES_PER_LINE);
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

static void aram_dma(gc_machine *m)
{
    uint32_t mm = ((uint32_t)m->dsp[0x10] << 16 | m->dsp[0x11]) & 0x03FFFFE0u;
    uint32_t ar = ((uint32_t)m->dsp[0x12] << 16 | m->dsp[0x13]) & 0x03FFFFE0u;
    uint32_t cnt = ((uint32_t)(m->dsp[0x14] & 0x7FFF) << 16 | m->dsp[0x15]) & ~31u;
    bool to_mram = m->dsp[0x14] & 0x8000;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t a = (ar + i) & (GC_ARAM_SIZE - 1), p = mm + i;
        if (p >= GC_RAM_SIZE)
            break;
        if (to_mram)
            m->h.ram[p] = m->aram[a];
        else
            m->aram[a] = m->h.ram[p];
    }
    m->dsp[0x14] &= 0x8000;
    m->dsp[0x15] = 0;
    m->dsp[5] |= 0x20; /* ARINT */
    dsp_int_update(m);
    gc_check_interrupts(m);
}

static uint16_t dsp_read16(gc_machine *m, unsigned idx)
{
    switch (idx) {
    case 0: /* CPU -> DSP mailbox: the DSP takes mail at once */
        return m->dsp[0] & 0x7FFF;
    case 2:
        return (uint16_t)((m->dsp_mail_full ? 0x8000 : 0) | ((m->dsp_mail >> 16) & 0x7FFF));
    case 3:
        m->dsp_mail_full = false;
        return (uint16_t)m->dsp_mail;
    case 5:
        return m->dsp[5] & ~0x0401; /* RES and the boot-busy bit read as done */
    default:
        return m->dsp[idx];
    }
}

static void dsp_write16(gc_machine *m, unsigned idx, uint16_t v)
{
    if (m->verbose >= 2)
        fprintf(stderr, "gc: DSP[%02X] = %04X\n", idx * 2, v);
    if (idx == 5) {
        uint16_t old = m->dsp[5];
        uint16_t ints = (uint16_t)(old & 0xA8 & ~v); /* AIINT ARINT DSPINT: write 1 to clear */
        m->dsp[5] = (uint16_t)((v & ~0xA8 & ~0x0401) | ints);
        if ((old & 0x04) && !(v & 0x04) && !(v & 0x01)) { /* leaves halt: the DSP ROM's greeting */
            m->dsp_mail = 0x8071FEEDu;
            m->dsp_mail_full = true;
        }
        dsp_int_update(m);
        gc_check_interrupts(m);
        return;
    }
    m->dsp[idx] = v;
    if (idx == 0x15)
        aram_dma(m);
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
    default:
        m->pi_reg[idx] = v;
        break;
    }
    gc_check_interrupts(m);
}

static void exi_write32(gc_machine *m, unsigned idx, uint32_t v)
{
    unsigned ch = idx / 5, reg = idx % 5;
    uint32_t *r = &m->exi[ch * 5];
    if (reg == 0) {
        uint32_t ints = r[0] & 0x80Au & ~v; /* EXIINT TCINT EXTINT: write 1 to clear */
        r[0] = (v & ~0x180Au) | ints;
    } else if (reg == 3) {
        r[3] = v;
        if (v & 1) { /* TSTART: done at once, nothing answers */
            if ((v & 2) && ((v >> 2) & 3) == 0) { /* DMA read: zeros */
                uint32_t mar = r[1] & 0x03FFFFE0u, len = r[2] & 0x03FFFFE0u;
                for (uint32_t i = 0; i < len && mar + i < GC_RAM_SIZE; i++)
                    m->h.ram[mar + i] = 0;
            } else if (!(v & 2) && ((v >> 2) & 3) != 1) {
                r[4] = 0;
            }
            r[3] &= ~1u;
            r[0] |= 8; /* TCINT */
        }
    } else {
        r[reg] = v;
    }
    exi_int_update(m);
    gc_check_interrupts(m);
}

static uint32_t exi_read32(gc_machine *m, unsigned idx)
{
    if (idx % 5 == 0)
        return m->exi[idx] & ~0x1000u; /* EXT: nothing plugged */
    return m->exi[idx];
}

static void si_write32(gc_machine *m, unsigned idx, uint32_t v)
{
    if (idx == 13) { /* COMCSR */
        uint32_t ints = m->si[13] & 0xA0000000u & ~v;
        m->si[13] = (v & ~0xA0000001u) | ints;
        if (v & 1)
            m->si[13] |= 0x80000000u | 0x20000000u; /* TCINT, and no device: the reply has an error */
    } else if (idx == 14) { /* SISR */
        m->si[14] = 0;
    } else {
        m->si[idx] = v;
    }
    si_int_update(m);
    gc_check_interrupts(m);
}

/* ---- dispatch ---- */

static uint16_t hw_read16(gc_machine *m, uint32_t pa)
{
    uint32_t off = pa & 0xFFFF;
    if (off >= 0x2000 && off < 0x2100)
        return vi_read16(m, (off & 0xFF) >> 1);
    if (off >= 0x5000 && off < 0x5080)
        return dsp_read16(m, (off & 0x7F) >> 1);
    if (off >= 0x4000 && off < 0x4080)
        return (uint16_t)m->mi_reg[(off & 0x7F) >> 1];
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
    if (off >= 0x5000 && off < 0x5080) {
        dsp_write16(m, (off & 0x7F) >> 1, v);
        return;
    }
    if (off >= 0x4000 && off < 0x4080) {
        m->mi_reg[(off & 0x7F) >> 1] = v;
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
    if (off >= 0x6000 && off < 0x6400)
        return 0; /* DI: no drive */
    if (off >= 0x6400 && off < 0x6500)
        return m->si[(off & 0xFF) >> 2];
    if (off >= 0x6800 && off < 0x683C)
        return exi_read32(m, (off & 0x3F) >> 2);
    if (off >= 0x6C00 && off < 0x6C20)
        return m->ai[(off & 0x1F) >> 2];
    if (m->verbose)
        fprintf(stderr, "gc: read%u of unmapped HW %08X\n", size * 8, pa);
    return 0;
}

void gc_hw_write(gc_machine *m, uint32_t pa, uint32_t v, unsigned size)
{
    uint32_t off = pa & 0xFFFF;
    if (off >= 0x8000)
        return; /* GX FIFO */
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
    else if (off >= 0x6000 && off < 0x6400)
        ;
    else if (off >= 0x6400 && off < 0x6500)
        si_write32(m, (off & 0xFF) >> 2, v);
    else if (off >= 0x6800 && off < 0x683C)
        exi_write32(m, (off & 0x3F) >> 2, v);
    else if (off >= 0x6C00 && off < 0x6C20)
        m->ai[(off & 0x1F) >> 2] = v;
    else if (m->verbose)
        fprintf(stderr, "gc: write%u to unmapped HW %08X = %08X\n", size * 8, pa, v);
}

/* ---- the slow path of every access ---- */

static uint32_t bus_read(gc_machine *m, uint32_t ea, unsigned size)
{
    uint32_t pa = ea & 0x3FFFFFFFu;
    if (pa < GC_RAM_SIZE) { /* crosses the end of MEM1 */
        uint32_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v = v << 8 | (pa + i < GC_RAM_SIZE ? m->h.ram[pa + i] : 0);
        return v;
    }
    if ((pa & 0xFFFF0000u) == 0x0C000000u)
        return gc_hw_read(m, pa, size);
    if (pa >= 0x20000000u && pa + size <= 0x20000000u + GC_LC_SIZE) {
        uint32_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v = v << 8 | m->lc[pa - 0x20000000u + i];
        return v;
    }
    if (m->verbose)
        fprintf(stderr, "gc: read%u of unmapped %08X (pc %08X)\n", size * 8, ea, m->h.cpu.pc);
    return 0;
}

static void bus_write(gc_machine *m, uint32_t ea, uint32_t v, unsigned size)
{
    uint32_t pa = ea & 0x3FFFFFFFu;
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
    if (pa >= 0x20000000u && pa + size <= 0x20000000u + GC_LC_SIZE) {
        for (unsigned i = 0; i < size; i++)
            m->lc[pa - 0x20000000u + i] = (uint8_t)(v >> (8 * (size - 1 - i)));
        return;
    }
    if (m->verbose)
        fprintf(stderr, "gc: write%u to unmapped %08X = %08X (pc %08X)\n", size * 8, ea, v, m->h.cpu.pc);
}

uint8_t gc_read8(gc_machine *m, uint32_t ea) { return (uint8_t)bus_read(m, ea, 1); }
uint16_t gc_read16(gc_machine *m, uint32_t ea) { return (uint16_t)bus_read(m, ea, 2); }
uint32_t gc_read32(gc_machine *m, uint32_t ea) { return bus_read(m, ea, 4); }
void gc_write8(gc_machine *m, uint32_t ea, uint8_t v) { bus_write(m, ea, v, 1); }
void gc_write16(gc_machine *m, uint32_t ea, uint16_t v) { bus_write(m, ea, v, 2); }
void gc_write32(gc_machine *m, uint32_t ea, uint32_t v) { bus_write(m, ea, v, 4); }
