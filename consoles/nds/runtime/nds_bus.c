/* nds_bus.c -- the DS buses (XBA-223, XBA-272): the devices both CPUs have (IRQ controller, timers, DMA, the IPC
 * sync register and FIFOs, DISPSTAT), the ARM9's memory map (ITCM / DTCM, main RAM, shared WRAM per WRAMCNT, I/O,
 * palette, VRAM through the bank mapping, OAM, an empty GBA slot), the VRAM bank mapping and line timing. The ARM7's
 * map is nds_bus7.c. Register meanings: GBATEK. */
#include <stdio.h>
#include <string.h>

#include "nds_internal.h"

/* ---- IRQ ---- */

void nds_irq_update(nds_arm *c)
{
    c->h.irq_line = (c->ime & 1) && (c->ie & c->irqf);
    if (c->ie & c->irqf)
        c->h.cpu.halted = false; /* Halt / wait for interrupt ends on any enabled request, whatever IME */
    if (c->h.irq_line && !(c->h.cpu.cpsr_ctl & 0x80u))
        c->h.next_event = c->h.now; /* lifted code stops at its next check */
}

void nds_irq_raise(nds_arm *c, uint32_t bits)
{
    c->irqf |= bits;
    nds_irq_update(c);
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
    memset(m->vmap_arm7, 0, sizeof m->vmap_arm7);
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
            else if (mst == 2) /* C / D: ARM7 */
                m->vmap_arm7[ofs & 1] = src;
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
    /* Mirrors: engine B BG (H at 0, I at 32 KB) repeats over its 128 KB; a 16 KB F/G bank in an empty page. */
    for (int p = 0; p < 8; p++)
        if (!m->vmap_bbg[p] && m->vramcnt[7] == 0x81 && p >= 2 && (p & 3) < 2)
            m->vmap_bbg[p] = m->vmap_bbg[p & 1];
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

/* ---- timers (GBATEK « DS Timers »): counted in 33 MHz units ---- */

static const uint32_t prescale[4] = {1, 64, 256, 1024};

static void timer_tick(nds_arm *c, int i, uint32_t ticks)
{
    nds_timer *t = &c->tm[i];
    while (ticks) {
        uint32_t left = 0x10000u - t->counter;
        if (ticks < left) {
            t->counter += ticks;
            return;
        }
        ticks -= left;
        t->counter = t->reload;
        if (t->ctl & 0x40)
            nds_irq_raise(c, 1u << (3 + i));
        if (i < 3 && (c->tm[i + 1].ctl & 0x84) == 0x84)
            timer_tick(c, i + 1, 1);
    }
}

void nds_timers_sync(nds_arm *c)
{
    for (int i = 0; i < 4; i++) {
        nds_timer *t = &c->tm[i];
        uint64_t el = c->h.now - t->since;
        t->since = c->h.now;
        if ((t->ctl & 0x80) && !(t->ctl & 0x04)) {
            uint64_t n = el + t->frac;
            uint32_t p = prescale[t->ctl & 3];
            t->frac = (uint32_t)(n % p);
            uint64_t ticks = n / p;
            while (ticks) {
                uint32_t step = ticks > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)ticks;
                timer_tick(c, i, step);
                ticks -= step;
            }
        }
    }
}

/* ---- DMA ---- */

static unsigned dma_timing(const nds_arm *c, uint32_t cnt)
{
    return c->id ? ((cnt >> 28) & 3) : ((cnt >> 27) & 7);
}

static void dma_run(nds_arm *c, int i)
{
    nds_dma *d = &c->dma[i];
    uint32_t cnt = d->cnt;
    uint32_t n = cnt & (c->id ? (i == 3 ? 0xFFFFu : 0x3FFFu) : 0x1FFFFFu);
    if (!n)
        n = c->id ? (i == 3 ? 0x10000u : 0x4000u) : 0x200000u;
    bool word = cnt & (1u << 26);
    int step = word ? 4 : 2;
    int ds = ((cnt >> 21) & 3) == 1 ? -step : ((cnt >> 21) & 3) == 2 ? 0 : step;
    int ss = ((cnt >> 23) & 3) == 1 ? -step : ((cnt >> 23) & 3) == 2 ? 0 : step;
    for (uint32_t k = 0; k < n; k++) {
        if (word)
            nds_write32(c, d->cur_d, nds_read32(c, d->cur_s));
        else
            nds_write16(c, d->cur_d, nds_read16(c, d->cur_s));
        d->cur_s += (uint32_t)ss;
        d->cur_d += (uint32_t)ds;
    }
    c->h.now += n;
    if (((cnt >> 21) & 3) == 3)
        d->cur_d = d->dad;
    if (!(cnt & (1u << 25)) || dma_timing(c, cnt) == 0) {
        d->cnt &= ~0x80000000u;
        d->armed = false;
        uint32_t off = 0xB0 + 12u * (unsigned)i + 8;
        uint32_t v = d->cnt;
        memcpy(&c->io[off], &v, 4);
    }
    if (cnt & (1u << 30))
        nds_irq_raise(c, 1u << (8 + i));
}

static void dma_written(nds_arm *c, int i)
{
    uint32_t base = 0xB0 + 12u * (unsigned)i;
    nds_dma *d = &c->dma[i];
    bool was = d->cnt & 0x80000000u;
    d->sad = nds_cio32(c, base);
    d->dad = nds_cio32(c, base + 4);
    d->cnt = nds_cio32(c, base + 8);
    if (!(d->cnt & 0x80000000u)) {
        d->armed = false;
        return;
    }
    if (!was) {
        d->cur_s = d->sad;
        d->cur_d = d->dad;
    }
    d->armed = true;
    if (dma_timing(c, d->cnt) == 0)
        dma_run(c, i);
}

void nds_dma_trigger(nds_arm *c, unsigned timing)
{
    for (int i = 0; i < 4; i++)
        if (c->dma[i].armed && dma_timing(c, c->dma[i].cnt) == timing)
            dma_run(c, i);
}

/* ---- IPC (GBATEK « DS Inter Process Communication ») ---- */

static void fifo_reset(nds_arm *c)
{
    c->fifo_n = c->fifo_h = 0;
}

uint32_t nds_fifo_pop(nds_arm *c)
{
    nds_arm *o = nds_other(c);
    if (!(c->fifocnt & 0x8000))
        return c->fifo_last;
    if (o->fifo_n == 0) {
        c->fifo_error = true;
        return c->fifo_last;
    }
    c->fifo_last = o->fifo[o->fifo_h];
    o->fifo_h = (o->fifo_h + 1) & 15;
    o->fifo_n--;
    if (o->fifo_n == 0 && (o->fifocnt & 0x4))
        nds_irq_raise(o, 1u << 17); /* the sender's FIFO is empty */
    return c->fifo_last;
}

static void fifo_push(nds_arm *c, uint32_t v)
{
    nds_arm *o = nds_other(c);
    if (!(c->fifocnt & 0x8000))
        return;
    if (c->fifo_n == 16) {
        c->fifo_error = true;
        return;
    }
    c->fifo[(c->fifo_h + c->fifo_n) & 15] = v;
    c->fifo_n++;
    if (c->fifo_n == 1 && (o->fifocnt & 0x400))
        nds_irq_raise(o, 1u << 18); /* the receiver's FIFO is not empty */
}

static uint32_t fifocnt_read(nds_arm *c)
{
    nds_arm *o = nds_other(c);
    uint32_t v = c->fifocnt & 0x8404u;
    v |= (uint32_t)(c->fifo_n == 0) | (uint32_t)(c->fifo_n == 16) << 1;
    v |= (uint32_t)(o->fifo_n == 0) << 8 | (uint32_t)(o->fifo_n == 16) << 9;
    v |= (uint32_t)c->fifo_error << 14;
    return v;
}

static void fifocnt_write(nds_arm *c, uint16_t v)
{
    nds_arm *o = nds_other(c);
    uint16_t old = c->fifocnt;
    if (v & 0x4000)
        c->fifo_error = false;
    if (v & 0x0008)
        fifo_reset(c);
    c->fifocnt = v & 0x8404u;
    if ((c->fifocnt & 0x4) && !(old & 0x4) && c->fifo_n == 0)
        nds_irq_raise(c, 1u << 17);
    if ((c->fifocnt & 0x400) && !(old & 0x400) && o->fifo_n)
        nds_irq_raise(c, 1u << 18);
}

/* ---- DISPSTAT / VCOUNT, the same counter for both CPUs ---- */

static uint32_t dispstat_read(nds_arm *c)
{
    nds_machine *m = c->m;
    uint16_t v = nds_cio16(c, 0x004) & 0xFFB8;
    uint32_t lyc = (uint32_t)(v >> 8) | (uint32_t)(v & 0x80) << 1;
    bool vb = m->vcount >= NDS_VISIBLE_LINES && m->vcount != NDS_LINES - 1;
    bool hb = c->h.now + NDS_LINE_CYCLES - m->line_end >= 256u * 6u;
    return v | (uint16_t)vb | (uint16_t)hb << 1 | (uint16_t)(m->vcount == lyc) << 2;
}

/* Registers both CPUs have at the same offset. off: halfword aligned. */
uint32_t nds_common_io_read(nds_arm *c, uint32_t off, bool *handled)
{
    *handled = true;
    switch (off) {
    case 0x004: return dispstat_read(c);
    case 0x006: return c->m->vcount;
    case 0x100: case 0x104: case 0x108: case 0x10C:
        nds_timers_sync(c);
        return c->tm[(off - 0x100) >> 2].counter;
    case 0x102: case 0x106: case 0x10A: case 0x10E:
        return c->tm[(off - 0x100) >> 2].ctl;
    case 0x130: return c->m->keys;
    case 0x180: return (uint32_t)((nds_other(c)->ipcsync >> 8) & 15) | (c->ipcsync & 0x4F00u);
    case 0x184: return fifocnt_read(c);
    case 0x208: return c->ime;
    case 0x20A: return 0;
    case 0x210: return c->ie & 0xFFFF;
    case 0x212: return c->ie >> 16;
    case 0x214: return c->irqf & 0xFFFF;
    case 0x216: return c->irqf >> 16;
    default:
        *handled = false;
        return 0;
    }
}

/* After a write of size bytes at off (already stored in c->io, except IF and the FIFO). */
bool nds_common_io_write(nds_arm *c, uint32_t off, unsigned size, uint32_t val)
{
    uint32_t end = off + size;
    bool any = false;
#define TOUCH(R, N) ((R) < end && (R) + (N) > off)
    if (off >= 0x214 && off < 0x218) { /* IF: write 1 to clear */
        uint32_t keep = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
        c->irqf &= ~((val & keep) << ((off - 0x214) * 8));
        nds_irq_update(c);
        return true;
    }
    if (off == 0x188) { /* FIFO send: 32-bit (a 16-bit write sends the halfword) */
        fifo_push(c, val);
        return true;
    }
    if (TOUCH(0x004, 2)) {
        c->io[0x004] &= 0xB8; /* bits 0-2 read only */
        any = true;
    }
    for (int i = 0; i < 4; i++)
        if (TOUCH(0xB0u + 12u * (unsigned)i + 10u, 2)) {
            dma_written(c, i);
            any = true;
        }
    for (int i = 0; i < 4; i++) {
        uint32_t r = 0x100u + 4u * (unsigned)i;
        if (TOUCH(r, 2)) {
            c->tm[i].reload = nds_cio16(c, r);
            any = true;
        }
        if (TOUCH(r + 2, 2)) {
            nds_timers_sync(c);
            uint16_t ctl = nds_cio16(c, r + 2);
            if ((ctl & 0x80) && !(c->tm[i].ctl & 0x80)) {
                c->tm[i].counter = c->tm[i].reload;
                c->tm[i].frac = 0;
            }
            c->tm[i].ctl = ctl & 0xC7;
            any = true;
        }
    }
    if (TOUCH(0x180, 2)) {
        uint16_t v = nds_cio16(c, 0x180);
        c->ipcsync = v & 0x4F00u;
        nds_arm *o = nds_other(c);
        if ((v & 0x2000) && (o->ipcsync & 0x4000))
            nds_irq_raise(o, 1u << 16);
        any = true;
    }
    if (TOUCH(0x184, 2)) {
        fifocnt_write(c, nds_cio16(c, 0x184));
        any = true;
    }
    if (TOUCH(0x208, 2)) {
        c->ime = c->io[0x208] & 1;
        nds_irq_update(c);
        any = true;
    }
    if (TOUCH(0x210, 4)) {
        c->ie = nds_cio32(c, 0x210);
        nds_irq_update(c);
        any = true;
    }
#undef TOUCH
    return any;
}

/* ---- ARM9: divider / square root ---- */

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
    if ((mode == 0 ? (uint32_t)du : du) == 0) {
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
    if ((uint64_t)du == 0)
        cnt |= 0x4000;
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

/* ---- ARM9 I/O ---- */

static uint32_t io9_read_reg(nds_machine *m, uint32_t off)
{
    bool handled;
    uint32_t v = nds_common_io_read(&m->a9, off, &handled);
    if (handled)
        return v;
    switch (off) {
    case 0x240: return m->vramcnt[0] | m->vramcnt[1] << 8;
    case 0x242: return m->vramcnt[2] | m->vramcnt[3] << 8;
    case 0x244: return m->vramcnt[4] | m->vramcnt[5] << 8;
    case 0x246: return m->vramcnt[6] | (uint32_t)m->wramcnt << 8;
    case 0x248: return m->vramcnt[7] | m->vramcnt[8] << 8;
    case 0x4000: case 0x4002: return 0; /* DSi SCFG: a DS */
    default:
        if (off < NDS_IO_SIZE)
            return nds_io16(m, off);
        return 0;
    }
}

static void io9_written(nds_machine *m, uint32_t off, unsigned size, uint32_t val)
{
    if (nds_common_io_write(&m->a9, off, size, val))
        return;
    uint32_t end = off + size;
#define TOUCH(R, N) ((R) < end && (R) + (N) > off)
    if (TOUCH(0x240, 10)) {
        for (int b = 0; b < 7; b++)
            m->vramcnt[b] = m->io[0x240 + b];
        m->vramcnt[7] = m->io[0x248];
        m->vramcnt[8] = m->io[0x249];
        m->wramcnt = m->io[0x247] & 3;
        nds_vram_remap(m);
    }
    if (TOUCH(0x280, 2) || TOUCH(0x290, 16))
        div_run(m);
    if (TOUCH(0x2B0, 2) || TOUCH(0x2B8, 8))
        sqrt_run(m);
#undef TOUCH
}

static void io9_write(nds_machine *m, uint32_t addr, uint32_t v, unsigned size)
{
    uint32_t off = addr - 0x04000000u;
    if (off >= NDS_IO_SIZE)
        return; /* FIFO receive (read only), card, DSi registers: ignored */
    if ((off >= 0x214 && off < 0x218) || (off & ~3u) == 0x188) {
        if ((off & ~3u) != 0x188 || off == 0x188)
            nds_common_io_write(&m->a9, off, size, v);
        return;
    }
    for (unsigned i = 0; i < size; i++)
        m->io[off + i] = (uint8_t)(v >> (8 * i));
    io9_written(m, off, size, v);
}

static uint32_t io9_read(nds_machine *m, uint32_t addr, unsigned size)
{
    if (addr == 0x04100000u)
        return nds_fifo_pop(&m->a9);
    if (addr >= 0x04100000u)
        return 0;
    uint32_t off = addr - 0x04000000u;
    if (size == 4)
        return io9_read_reg(m, off & ~3u) | io9_read_reg(m, (off & ~3u) + 2) << 16;
    uint32_t v = io9_read_reg(m, off & ~1u);
    return size == 2 ? v : (v >> ((off & 1) * 8)) & 0xFF;
}

/* ---- the ARM9 map ---- */

uint8_t *nds9_ptr(nds_machine *m, uint32_t addr)
{
    if (addr < m->itcm_size && (m->cp15_ctl & (1u << 18)))
        return m->itcm + (addr & (NDS_ITCM_SIZE - 1));
    if ((m->cp15_ctl & (1u << 16)) && addr - m->dtcm_base < m->dtcm_size)
        return m->dtcm + ((addr - m->dtcm_base) & (NDS_DTCM_SIZE - 1));
    switch (addr >> 24) {
    case 0x02: return m->main_ram + (addr & (NDS_MAIN_SIZE - 1));
    case 0x03:
        switch (m->wramcnt) {
        case 0: return m->wram + (addr & 0x7FFF);
        case 1: return m->wram + 0x4000 + (addr & 0x3FFF);
        case 2: return m->wram + (addr & 0x3FFF);
        default: return NULL;
        }
    case 0x05: return m->palette + (addr & 0x7FF);
    case 0x06: return vram_ptr(m, addr);
    case 0x07: return m->oam + (addr & 0x7FF);
    default: return NULL;
    }
}

uint8_t nds9_read8(nds_machine *m, uint32_t addr)
{
    uint8_t *p = nds9_ptr(m, addr);
    if (p)
        return *p;
    if ((addr >> 24) == 0x04)
        return (uint8_t)io9_read(m, addr, 1);
    if ((addr >> 24) == 0x08 || (addr >> 24) == 0x09)
        return 0xFF;
    return 0;
}

uint16_t nds9_read16(nds_machine *m, uint32_t addr)
{
    addr &= ~1u;
    uint8_t *p = nds9_ptr(m, addr);
    if (p) {
        uint16_t v;
        memcpy(&v, p, 2);
        return v;
    }
    if ((addr >> 24) == 0x04)
        return (uint16_t)io9_read(m, addr, 2);
    if ((addr >> 24) == 0x08 || (addr >> 24) == 0x09)
        return 0xFFFF;
    return 0;
}

uint32_t nds9_read32(nds_machine *m, uint32_t addr)
{
    addr &= ~3u;
    uint8_t *p = nds9_ptr(m, addr);
    if (p) {
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }
    if ((addr >> 24) == 0x04)
        return io9_read(m, addr, 4);
    if ((addr >> 24) == 0x08 || (addr >> 24) == 0x09)
        return 0xFFFFFFFFu;
    return 0;
}

void nds9_write8(nds_machine *m, uint32_t addr, uint8_t v)
{
    uint32_t r = addr >> 24;
    if (r == 0x05 || r == 0x06 || r == 0x07) {
        bool tcm = (addr < m->itcm_size && (m->cp15_ctl & (1u << 18))) ||
                   ((m->cp15_ctl & (1u << 16)) && addr - m->dtcm_base < m->dtcm_size);
        if (!tcm)
            return; /* 8-bit writes to palette, VRAM and OAM are ignored on the ARM9 */
    }
    uint8_t *p = nds9_ptr(m, addr);
    if (p) {
        *p = v;
        return;
    }
    if (r == 0x04)
        io9_write(m, addr, v, 1);
}

void nds9_write16(nds_machine *m, uint32_t addr, uint16_t v)
{
    addr &= ~1u;
    uint8_t *p = nds9_ptr(m, addr);
    if (p) {
        memcpy(p, &v, 2);
        return;
    }
    if ((addr >> 24) == 0x04)
        io9_write(m, addr, v, 2);
}

void nds9_write32(nds_machine *m, uint32_t addr, uint32_t v)
{
    addr &= ~3u;
    uint8_t *p = nds9_ptr(m, addr);
    if (p) {
        memcpy(p, &v, 4);
        return;
    }
    if ((addr >> 24) == 0x04)
        io9_write(m, addr, v, 4);
}

uint8_t nds9_debug_read8(nds_machine *m, uint32_t addr)
{
    uint8_t *p = nds9_ptr(m, addr);
    return p ? *p : 0;
}

/* ---- the bus lifted code and the interpreter call: the CPU's own map ---- */

uint8_t nds_read8(nds_arm *c, uint32_t a) { return c->id ? nds7_read8(c->m, a) : nds9_read8(c->m, a); }
uint16_t nds_read16(nds_arm *c, uint32_t a) { return c->id ? nds7_read16(c->m, a) : nds9_read16(c->m, a); }
uint32_t nds_read32(nds_arm *c, uint32_t a) { return c->id ? nds7_read32(c->m, a) : nds9_read32(c->m, a); }
void nds_write8(nds_arm *c, uint32_t a, uint8_t v)
{
    if (c->id)
        nds7_write8(c->m, a, v);
    else
        nds9_write8(c->m, a, v);
}
void nds_write16(nds_arm *c, uint32_t a, uint16_t v)
{
    if (c->id)
        nds7_write16(c->m, a, v);
    else
        nds9_write16(c->m, a, v);
}
void nds_write32(nds_arm *c, uint32_t a, uint32_t v)
{
    if (c->id)
        nds7_write32(c->m, a, v);
    else
        nds9_write32(c->m, a, v);
}

uint32_t nds_peek32(nds_machine *m, uint32_t addr)
{
    if ((addr >> 24) == 0x04 && (addr & 0xFFFFFF) + 4 <= NDS_IO_SIZE)
        return nds_io32(m, addr & 0xFFFFFC);
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)nds9_debug_read8(m, addr + (uint32_t)i) << (8 * i);
    return v;
}

uint32_t nds_peek32_arm7(nds_machine *m, uint32_t addr)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)nds7_debug_read8(m, addr + (uint32_t)i) << (8 * i);
    return v;
}

/* ---- lines ---- */

static void line_cpu(nds_arm *c, bool vblank_start, bool visible_hblank)
{
    nds_machine *m = c->m;
    uint16_t stat = nds_cio16(c, 0x004);
    if (stat & 0x10)
        nds_irq_raise(c, 1u << 1); /* HBlank of the line ending */
    if (visible_hblank && !c->id)
        nds_dma_trigger(c, 2);
    if (vblank_start) {
        if (stat & 0x08)
            nds_irq_raise(c, 1u << 0);
        nds_dma_trigger(c, 1);
    }
    uint32_t lyc = (uint32_t)(stat >> 8) | (uint32_t)(stat & 0x80) << 1;
    if ((stat & 0x20) && m->vcount == lyc)
        nds_irq_raise(c, 1u << 2);
    nds_timers_sync(c);
}

void nds_line_event(nds_machine *m)
{
    bool visible = m->vcount < NDS_VISIBLE_LINES;
    m->vcount = (m->vcount + 1) % NDS_LINES;
    m->line_end += NDS_LINE_CYCLES;
    bool vb = m->vcount == NDS_VISIBLE_LINES;
    if (vb)
        nds_gpu_render(m);
    line_cpu(&m->a9, vb, visible);
    line_cpu(&m->a7, vb, visible);
}

void nds_bus_reset(nds_machine *m)
{
    memset(m->io, 0, sizeof m->io);
    memset(m->io7, 0, sizeof m->io7);
    memset(m->vramcnt, 0, sizeof m->vramcnt);
    nds_vram_remap(m);
    m->vcount = 0;
    m->line_end = NDS_LINE_CYCLES;
    nds_arm *cs[2] = {&m->a9, &m->a7};
    for (int i = 0; i < 2; i++) {
        nds_arm *c = cs[i];
        c->ime = c->ie = c->irqf = 0;
        memset(c->tm, 0, sizeof c->tm);
        memset(c->dma, 0, sizeof c->dma);
        c->ipcsync = 0;
        c->fifocnt = 0;
        c->fifo_n = c->fifo_h = 0;
        c->fifo_last = 0;
        c->fifo_error = false;
    }
    /* What the firmware leaves: POWCNT1 with both engines on, engine A on the top screen; all shared WRAM to the
     * ARM7 (WRAMCNT 3); POSTFLG set. */
    m->io[0x304] = 0x0F;
    m->io[0x305] = 0x82;
    m->io[0x247] = 3;
    m->wramcnt = 3;
    m->io[0x300] = 1;
    m->postflg7 = 1;
    m->keys = 0x03FF;
    m->extkeys = 0x007F;
    /* CP15 as the firmware leaves it for a direct boot (the program sets its own regions at once). */
    m->cp15_ctl = 0x00052078;
    m->cp15_dtcm = 0x0080000A;
    m->cp15_itcm = 0x0000000C;
    m->dtcm_base = 0x00800000;
    m->dtcm_size = 0x4000;
    m->itcm_size = 0x2000000;
}
