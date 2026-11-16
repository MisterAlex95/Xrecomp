/* nds_bus.c -- the ARM9's memory map in the DS spike (XBA-223): ITCM / DTCM (CP15 regions), main RAM, shared WRAM,
 * I/O (display, DMA, timers, keypad, IPC, IRQ, VRAM banks, divider and square root, power), palette, VRAM through
 * the bank mapping, OAM, an empty GBA slot. Line timing and the VBlank / HBlank / VCount IRQs live here too. */
#include <stdio.h>
#include <string.h>

#include "nds_internal.h"

/* ---- IRQ ---- */

void nds_irq_update(nds_machine *m)
{
    m->h.irq_line = (m->ime & 1) && (m->ie & m->irqf);
    if (m->ie & m->irqf)
        m->h.cpu.halted = false; /* CP15 wait for interrupt ends on any enabled request */
    if (m->h.irq_line && !(m->h.cpu.cpsr_ctl & 0x80u))
        m->h.next_event = m->h.now; /* lifted code stops at its next check */
}

void nds_irq_raise(nds_machine *m, uint32_t bits)
{
    m->irqf |= bits;
    nds_irq_update(m);
}

/* ---- VRAM banks (GBATEK « DS Memory Control - VRAM ») ---- */

static const uint32_t bank_off[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
static const uint32_t bank_size[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};

static void map_pages(uint8_t **pages, unsigned npages, uint32_t at, uint8_t *src, uint32_t size)
{
    for (uint32_t o = 0; o < size; o += 0x4000) {
        unsigned p = (unsigned)((at + o) >> 14);
        if (p < npages)
            pages[p] = src + o;
    }
}

void nds_vram_remap(nds_machine *m)
{
    memset(m->vmap_abg, 0, sizeof m->vmap_abg);
    memset(m->vmap_bbg, 0, sizeof m->vmap_bbg);
    memset(m->vmap_aobj, 0, sizeof m->vmap_aobj);
    memset(m->vmap_bobj, 0, sizeof m->vmap_bobj);
    for (int b = 0; b < 9; b++) {
        uint8_t cnt = m->vramcnt[b];
        if (!(cnt & 0x80))
            continue;
        uint32_t mst = cnt & 7, ofs = (cnt >> 3) & 3;
        uint8_t *src = m->vram + bank_off[b];
        uint32_t sz = bank_size[b];
        if (b <= 3) { /* A-D */
            if (mst == 1)
                map_pages(m->vmap_abg, 32, ofs * 0x20000, src, sz);
            else if (mst == 2 && b <= 1)
                map_pages(m->vmap_aobj, 16, (ofs & 1) * 0x20000, src, sz);
            else if (mst == 4 && b == 2)
                map_pages(m->vmap_bbg, 8, 0, src, sz);
            else if (mst == 4 && b == 3)
                map_pages(m->vmap_bobj, 8, 0, src, sz);
        } else if (b == 4) { /* E */
            if (mst == 1)
                map_pages(m->vmap_abg, 32, 0, src, sz);
            else if (mst == 2)
                map_pages(m->vmap_aobj, 16, 0, src, sz);
        } else if (b <= 6) { /* F, G */
            uint32_t at = (ofs & 1) * 0x4000 + (ofs >> 1) * 0x10000;
            if (mst == 1)
                map_pages(m->vmap_abg, 32, at, src, sz);
            else if (mst == 2)
                map_pages(m->vmap_aobj, 16, at, src, sz);
        } else if (b == 7) { /* H */
            if (mst == 1)
                map_pages(m->vmap_bbg, 8, 0, src, sz);
        } else { /* I */
            if (mst == 1)
                map_pages(m->vmap_bbg, 8, 0x8000, src, sz);
            else if (mst == 2)
                map_pages(m->vmap_bobj, 8, 0, src, sz);
        }
    }
}

/* Host address of a VRAM byte as the ARM9 sees it, NULL when unmapped. */
static uint8_t *vram_ptr(nds_machine *m, uint32_t addr)
{
    uint32_t a = addr & 0xFFFFFF;
    switch (a >> 21) {
    case 0: {
        uint8_t *p = m->vmap_abg[(a >> 14) & 31];
        return p ? p + (a & 0x3FFF) : NULL;
    }
    case 1: {
        uint8_t *p = m->vmap_bbg[(a >> 14) & 7];
        return p ? p + (a & 0x3FFF) : NULL;
    }
    case 2: {
        uint8_t *p = m->vmap_aobj[(a >> 14) & 15];
        return p ? p + (a & 0x3FFF) : NULL;
    }
    case 3: {
        uint8_t *p = m->vmap_bobj[(a >> 14) & 7];
        return p ? p + (a & 0x3FFF) : NULL;
    }
    default: {
        uint32_t o = a - 0x800000;
        if (a < 0x800000 || o >= NDS_VRAM_SIZE)
            return NULL;
        for (int b = 0; b < 9; b++)
            if (o >= bank_off[b] && o < bank_off[b] + bank_size[b])
                return (m->vramcnt[b] & 0x87) == 0x80 ? m->vram + o : NULL; /* LCDC: enabled, MST 0 */
        return NULL;
    }
    }
}

/* ---- timers (GBATEK « DS Timers »): counted in 33 MHz cycles = 1 cycle here ---- */

static const uint32_t prescale[4] = {1, 64, 256, 1024};

static void timer_tick(nds_machine *m, int i, uint32_t ticks)
{
    nds_timer *t = &m->tm[i];
    while (ticks) {
        uint32_t left = 0x10000u - t->counter;
        if (ticks < left) {
            t->counter += ticks;
            return;
        }
        ticks -= left;
        t->counter = t->reload;
        if (t->ctl & 0x40)
            nds_irq_raise(m, 1u << (3 + i));
        if (i < 3 && (m->tm[i + 1].ctl & 0x84) == 0x84)
            timer_tick(m, i + 1, 1);
    }
}

void nds_timers_sync(nds_machine *m)
{
    for (int i = 0; i < 4; i++) {
        nds_timer *t = &m->tm[i];
        uint64_t el = m->h.now - t->since;
        t->since = m->h.now;
        if ((t->ctl & 0x80) && !(t->ctl & 0x04)) {
            uint64_t c = el + t->frac;
            uint32_t p = prescale[t->ctl & 3];
            t->frac = (uint32_t)(c % p);
            uint64_t ticks = c / p;
            while (ticks) {
                uint32_t step = ticks > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)ticks;
                timer_tick(m, i, step);
                ticks -= step;
            }
        }
    }
}

/* ---- DMA ---- */

static void dma_run(nds_machine *m, int i)
{
    nds_dma *d = &m->dma[i];
    uint32_t cnt = d->cnt;
    uint32_t n = cnt & 0x1FFFFF;
    if (!n)
        n = 0x200000;
    bool word = cnt & (1u << 26);
    int step = word ? 4 : 2;
    int ds = ((cnt >> 21) & 3) == 1 ? -step : ((cnt >> 21) & 3) == 2 ? 0 : step;
    int ss = ((cnt >> 23) & 3) == 1 ? -step : ((cnt >> 23) & 3) == 2 ? 0 : step;
    for (uint32_t k = 0; k < n; k++) {
        if (word)
            nds_write32(m, d->cur_d, nds_read32(m, d->cur_s));
        else
            nds_write16(m, d->cur_d, nds_read16(m, d->cur_s));
        d->cur_s += (uint32_t)ss;
        d->cur_d += (uint32_t)ds;
    }
    m->h.now += n;
    if (((cnt >> 21) & 3) == 3)
        d->cur_d = d->dad;
    if (!(cnt & (1u << 25)) || ((cnt >> 27) & 7) == 0) {
        d->cnt &= ~0x80000000u;
        d->armed = false;
        uint32_t off = 0xB0 + 12u * (unsigned)i + 8;
        uint32_t v = d->cnt;
        memcpy(&m->io[off], &v, 4);
    }
    if (cnt & (1u << 30))
        nds_irq_raise(m, 1u << (8 + i));
}

static void dma_written(nds_machine *m, int i)
{
    uint32_t base = 0xB0 + 12u * (unsigned)i;
    nds_dma *d = &m->dma[i];
    bool was = d->cnt & 0x80000000u;
    d->sad = nds_io32(m, base);
    d->dad = nds_io32(m, base + 4);
    d->cnt = nds_io32(m, base + 8);
    if (!(d->cnt & 0x80000000u)) {
        d->armed = false;
        return;
    }
    if (!was) {
        d->cur_s = d->sad;
        d->cur_d = d->dad;
    }
    d->armed = true;
    if (((d->cnt >> 27) & 7) == 0)
        dma_run(m, i);
}

void nds_dma_trigger(nds_machine *m, unsigned timing)
{
    for (int i = 0; i < 4; i++)
        if (m->dma[i].armed && ((m->dma[i].cnt >> 27) & 7) == timing)
            dma_run(m, i);
}

/* ---- divider / square root ---- */

static void div_run(nds_machine *m)
{
    uint32_t mode = nds_io16(m, 0x280) & 3;
    int64_t n, d;
    uint64_t nu = (uint64_t)nds_io32(m, 0x290) | (uint64_t)nds_io32(m, 0x294) << 32;
    uint64_t du = (uint64_t)nds_io32(m, 0x298) | (uint64_t)nds_io32(m, 0x29C) << 32;
    if (mode == 0) {
        n = (int32_t)nu;
        d = (int32_t)du;
    } else if (mode == 1) {
        n = (int64_t)nu;
        d = (int32_t)du;
    } else {
        n = (int64_t)nu;
        d = (int64_t)du;
    }
    int64_t q, r;
    uint16_t cnt = nds_io16(m, 0x280) & 3;
    if ((uint64_t)du == 0) {
        cnt |= 0x4000;
        q = n < 0 ? 1 : -1;
        r = n;
        if (mode == 0)
            q = (int64_t)(uint64_t)(uint32_t)(n < 0 ? 1 : -1) ^ (int64_t)0xFFFFFFFF00000000ull;
    } else if (n == INT64_MIN && d == -1) {
        q = INT64_MIN;
        r = 0;
    } else {
        q = n / d;
        r = n % d;
    }
    memcpy(&m->io[0x2A0], &q, 8);
    memcpy(&m->io[0x2A8], &r, 8);
    m->io[0x280] = (uint8_t)cnt;
    m->io[0x281] = (uint8_t)(cnt >> 8);
}

static void sqrt_run(nds_machine *m)
{
    uint64_t v = (uint64_t)nds_io32(m, 0x2B8);
    if (nds_io16(m, 0x2B0) & 1)
        v |= (uint64_t)nds_io32(m, 0x2BC) << 32;
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    uint32_t res = (uint32_t)r;
    memcpy(&m->io[0x2B4], &res, 4);
}

/* ---- I/O ---- */

static uint32_t io_read_reg(nds_machine *m, uint32_t off)
{
    /* off: halfword-aligned offset into 0x04000000; returns the 16-bit value there */
    switch (off) {
    case 0x004: { /* DISPSTAT */
        uint16_t v = nds_io16(m, 0x004) & 0xFFB8;
        uint32_t lyc = (uint32_t)(v >> 8) | (uint32_t)(v & 0x80) << 1;
        bool vb = m->vcount >= NDS_VISIBLE_LINES && m->vcount != NDS_LINES - 1;
        bool hb = m->h.now + NDS_LINE_CYCLES - m->line_end >= 256u * 6u;
        return v | (uint16_t)vb | (uint16_t)hb << 1 | (uint16_t)(m->vcount == lyc) << 2;
    }
    case 0x006: return m->vcount;
    case 0x100: case 0x104: case 0x108: case 0x10C:
        nds_timers_sync(m);
        return m->tm[(off - 0x100) >> 2].counter;
    case 0x102: case 0x106: case 0x10A: case 0x10E:
        return m->tm[(off - 0x100) >> 2].ctl;
    case 0x130: return 0x03FF;   /* KEYINPUT: nothing pressed */
    case 0x136: return 0x007F;   /* EXTKEYIN (ARM7's, mirrored by nothing here) */
    case 0x180: return (uint32_t)m->ipcsync_in9 | (m->ipcsync_out9 & 0x4F00u);
    case 0x184: {
        uint32_t v = m->fifocnt & 0x8404u;
        v |= 1u;                              /* send FIFO: the stub ARM7 drains it at once */
        if (m->n79 == 0)
            v |= 1u << 8;
        if (m->n79 == 16)
            v |= 1u << 9;
        if (m->fifo_error)
            v |= 1u << 14;
        return v;
    }
    case 0x208: return m->ime;
    case 0x210: return m->ie & 0xFFFF;
    case 0x212: return m->ie >> 16;
    case 0x214: return m->irqf & 0xFFFF;
    case 0x216: return m->irqf >> 16;
    case 0x240: return m->vramcnt[0] | m->vramcnt[1] << 8;
    case 0x242: return m->vramcnt[2] | m->vramcnt[3] << 8;
    case 0x244: return m->vramcnt[4] | m->vramcnt[5] << 8;
    case 0x246: return m->vramcnt[6] | (uint32_t)m->io[0x247] << 8;
    case 0x248: return m->vramcnt[7] | m->vramcnt[8] << 8;
    case 0x4000: case 0x4002: return 0; /* DSi SCFG: a DS */
    default:
        if (off < NDS_IO_SIZE)
            return nds_io16(m, off);
        return 0;
    }
}

static uint32_t fifo_pop(nds_machine *m)
{
    if (!(m->fifocnt & 0x8000))
        return m->fifo_last;
    if (m->n79 == 0) {
        m->fifo_error = true;
        return m->fifo_last;
    }
    m->fifo_last = m->fifo79[m->h79];
    m->h79 = (m->h79 + 1) & 15;
    m->n79--;
    return m->fifo_last;
}

static void fifo_irqs(nds_machine *m)
{
    if ((m->fifocnt & 0x400) && m->n79)
        nds_irq_raise(m, 1u << 18);
    if (m->fifocnt & 0x4)
        nds_irq_raise(m, 1u << 17); /* send FIFO empty: always, the stub drains it */
}

void nds_fifo_push79(nds_machine *m, uint32_t word)
{
    if (m->n79 == 16)
        return;
    m->fifo79[(m->h79 + m->n79) & 15] = word;
    m->n79++;
    fifo_irqs(m);
}

/* After a write of `size` bytes at io offset off (already stored in m->io): side effects per register. */
static void io_written(nds_machine *m, uint32_t off, unsigned size, uint32_t val)
{
    uint32_t end = off + size;
#define TOUCH(R, N) ((R) < end && (R) + (N) > off)
    if (TOUCH(0x004, 2)) { /* DISPSTAT: bits 0-2 read only */
        m->io[0x004] &= 0xB8;
    }
    for (int i = 0; i < 4; i++)
        if (TOUCH(0xB0u + 12u * (unsigned)i + 10u, 2))
            dma_written(m, i);
    for (int i = 0; i < 4; i++) {
        uint32_t r = 0x100u + 4u * (unsigned)i;
        if (TOUCH(r, 2))
            m->tm[i].reload = nds_io16(m, r);
        if (TOUCH(r + 2, 2)) {
            nds_timers_sync(m);
            uint16_t ctl = nds_io16(m, r + 2);
            if ((ctl & 0x80) && !(m->tm[i].ctl & 0x80)) {
                m->tm[i].counter = m->tm[i].reload;
                m->tm[i].frac = 0;
            }
            m->tm[i].ctl = ctl & 0xC7;
        }
    }
    if (TOUCH(0x180, 2)) {
        uint16_t v = nds_io16(m, 0x180);
        m->ipcsync_out9 = v & 0x4F00u; /* bit 13 (IRQ to the ARM7): the stub needs no ping */
        nds_arm7_sync_written(m);
    }
    if (TOUCH(0x184, 2)) {
        uint16_t v = nds_io16(m, 0x184);
        if (v & 0x4000)
            m->fifo_error = false;
        m->fifocnt = v & 0x8404u;
        fifo_irqs(m);
    }
    if (off == 0x188 && size == 4 && (m->fifocnt & 0x8000))
        nds_arm7_fifo_recv(m, val);
    if (TOUCH(0x208, 2)) {
        m->ime = m->io[0x208] & 1;
        nds_irq_update(m);
    }
    if (TOUCH(0x210, 4)) {
        m->ie = nds_io32(m, 0x210);
        nds_irq_update(m);
    }
    if (TOUCH(0x240, 10)) {
        for (int b = 0; b < 7; b++)
            m->vramcnt[b] = m->io[0x240 + b];
        m->vramcnt[7] = m->io[0x248];
        m->vramcnt[8] = m->io[0x249];
        nds_vram_remap(m);
    }
    if (TOUCH(0x280, 2) || TOUCH(0x290, 16))
        div_run(m);
    if (TOUCH(0x2B0, 2) || TOUCH(0x2B8, 8))
        sqrt_run(m);
#undef TOUCH
}

static void io_write(nds_machine *m, uint32_t addr, uint32_t v, unsigned size)
{
    uint32_t off = addr - 0x04000000u;
    if (off >= NDS_IO_SIZE)
        return; /* FIFO receive (read only), card, DSi registers: ignored */
    if (off >= 0x214 && off < 0x218) { /* IF: write 1 to clear */
        uint32_t keep = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
        m->irqf &= ~((v & keep) << ((off - 0x214) * 8));
        nds_irq_update(m);
        return;
    }
    if (off == 0x188) { /* FIFO send: 32-bit */
        if (size == 4)
            io_written(m, off, 4, v);
        return;
    }
    for (unsigned i = 0; i < size; i++)
        m->io[off + i] = (uint8_t)(v >> (8 * i));
    io_written(m, off, size, v);
}

static uint32_t io_read(nds_machine *m, uint32_t addr, unsigned size)
{
    if (addr == 0x04100000u)
        return fifo_pop(m);
    if (addr >= 0x04100000u)
        return 0;
    uint32_t off = addr - 0x04000000u;
    if (size == 4)
        return io_read_reg(m, off & ~3u) | io_read_reg(m, (off & ~3u) + 2) << 16;
    uint32_t v = io_read_reg(m, off & ~1u);
    return size == 2 ? v : (v >> ((off & 1) * 8)) & 0xFF;
}

/* ---- the map ---- */

static uint8_t *direct_ptr(nds_machine *m, uint32_t addr, bool write)
{
    (void)write;
    if (addr < m->itcm_size && (m->cp15_ctl & (1u << 18)))
        return m->itcm + (addr & (NDS_ITCM_SIZE - 1));
    if ((m->cp15_ctl & (1u << 16)) && addr - m->dtcm_base < m->dtcm_size)
        return m->dtcm + ((addr - m->dtcm_base) & (NDS_DTCM_SIZE - 1));
    switch (addr >> 24) {
    case 0x02: return m->main_ram + (addr & (NDS_MAIN_SIZE - 1));
    case 0x03: return m->wram + (addr & (NDS_WRAM_SIZE - 1));
    case 0x05: return m->palette + (addr & 0x7FF);
    case 0x06: return vram_ptr(m, addr);
    case 0x07: return m->oam + (addr & 0x7FF);
    default: return NULL;
    }
}

uint8_t nds_read8(nds_machine *m, uint32_t addr)
{
    uint8_t *p = direct_ptr(m, addr, false);
    if (p)
        return *p;
    if ((addr >> 24) == 0x04)
        return (uint8_t)io_read(m, addr, 1);
    if ((addr >> 24) == 0x08 || (addr >> 24) == 0x09)
        return 0xFF;
    return 0;
}

uint16_t nds_read16(nds_machine *m, uint32_t addr)
{
    addr &= ~1u;
    uint8_t *p = direct_ptr(m, addr, false);
    if (p) {
        uint16_t v;
        memcpy(&v, p, 2);
        return v;
    }
    if ((addr >> 24) == 0x04)
        return (uint16_t)io_read(m, addr, 2);
    if ((addr >> 24) == 0x08 || (addr >> 24) == 0x09)
        return 0xFFFF;
    return 0;
}

uint32_t nds_read32(nds_machine *m, uint32_t addr)
{
    addr &= ~3u;
    uint8_t *p = direct_ptr(m, addr, false);
    if (p) {
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }
    if ((addr >> 24) == 0x04)
        return io_read(m, addr, 4);
    if ((addr >> 24) == 0x08 || (addr >> 24) == 0x09)
        return 0xFFFFFFFFu;
    return 0;
}

void nds_write8(nds_machine *m, uint32_t addr, uint8_t v)
{
    uint32_t r = addr >> 24;
    if (r == 0x05 || r == 0x06 || r == 0x07) {
        bool tcm = (addr < m->itcm_size && (m->cp15_ctl & (1u << 18))) ||
                   ((m->cp15_ctl & (1u << 16)) && addr - m->dtcm_base < m->dtcm_size);
        if (!tcm)
            return; /* 8-bit writes to palette, VRAM and OAM are ignored on the ARM9 */
    }
    uint8_t *p = direct_ptr(m, addr, true);
    if (p) {
        *p = v;
        return;
    }
    if (r == 0x04)
        io_write(m, addr, v, 1);
}

void nds_write16(nds_machine *m, uint32_t addr, uint16_t v)
{
    addr &= ~1u;
    uint8_t *p = direct_ptr(m, addr, true);
    if (p) {
        memcpy(p, &v, 2);
        return;
    }
    if ((addr >> 24) == 0x04)
        io_write(m, addr, v, 2);
}

void nds_write32(nds_machine *m, uint32_t addr, uint32_t v)
{
    addr &= ~3u;
    uint8_t *p = direct_ptr(m, addr, true);
    if (p) {
        memcpy(p, &v, 4);
        return;
    }
    if ((addr >> 24) == 0x04)
        io_write(m, addr, v, 4);
}

uint8_t nds_debug_read8(nds_machine *m, uint32_t addr)
{
    uint8_t *p = direct_ptr(m, addr, false);
    return p ? *p : 0;
}

uint32_t nds_peek32(nds_machine *m, uint32_t addr)
{
    if ((addr >> 24) == 0x04 && (addr & 0xFFFFFF) + 4 <= NDS_IO_SIZE)
        return nds_io32(m, addr & 0xFFFFFC);
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)nds_debug_read8(m, addr + (uint32_t)i) << (8 * i);
    return v;
}

/* ---- lines ---- */

void nds_line_event(nds_machine *m)
{
    uint16_t stat = nds_io16(m, 0x004);
    /* HBlank of the line ending */
    if (stat & 0x10)
        nds_irq_raise(m, 1u << 1);
    if (m->vcount < NDS_VISIBLE_LINES)
        nds_dma_trigger(m, 2);
    m->vcount = (m->vcount + 1) % NDS_LINES;
    m->line_end += NDS_LINE_CYCLES;
    if (m->vcount == NDS_VISIBLE_LINES) {
        nds_gpu_render(m);
        if (stat & 0x08)
            nds_irq_raise(m, 1u << 0);
        nds_dma_trigger(m, 1);
    }
    uint32_t lyc = (uint32_t)(stat >> 8) | (uint32_t)(stat & 0x80) << 1;
    if ((stat & 0x20) && m->vcount == lyc)
        nds_irq_raise(m, 1u << 2);
    nds_timers_sync(m);
}

void nds_bus_reset(nds_machine *m)
{
    memset(m->io, 0, sizeof m->io);
    memset(m->vramcnt, 0, sizeof m->vramcnt);
    nds_vram_remap(m);
    m->ime = m->ie = m->irqf = 0;
    m->vcount = 0;
    m->line_end = NDS_LINE_CYCLES;
    memset(m->tm, 0, sizeof m->tm);
    memset(m->dma, 0, sizeof m->dma);
    /* What the firmware leaves: POWCNT1 with both engines on, engine A on the top screen. */
    m->io[0x304] = 0x0F;
    m->io[0x305] = 0x82;
    m->io[0x247] = 3; /* WRAMCNT: all shared WRAM to the ARM7 */
    /* CP15 as the firmware leaves it for a direct boot (the program sets its own regions at once). */
    m->cp15_ctl = 0x00052078;
    m->cp15_dtcm = 0x0080000A;
    m->cp15_itcm = 0x0000000C;
    m->dtcm_base = 0x00800000;
    m->dtcm_size = 0x4000;
    m->itcm_size = 0x2000000;
}
