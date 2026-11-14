/* GBA IO registers (XBA-153): LCD timing (DISPSTAT / VCOUNT), the four timers with prescalers and cascade, the
 * four DMA channels, interrupts (IE / IF / IME), HALTCNT, KEYINPUT / KEYCNT, WAITCNT. GBATEK « GBA I/O Map ».
 *
 * m->io keeps the 0x400-byte register image as the CPU wrote it (the PPU reads it directly); registers with
 * hardware state (timer counters, IF, KEYINPUT, DISPSTAT flags, VCOUNT) are kept in sync or computed on read.
 * Sound: no audio output yet. The sound registers are stored, and the FIFO DMAs (DMA1/2 special timing) are
 * driven by timer overflows so games that stream audio keep running. */
#include "gba_internal.h"

static const int timer_shift[4] = {0, 6, 8, 10};

static inline int32_t sext28(uint32_t v)
{
    return (int32_t)(v << 4) >> 4;
}

/* ---- interrupts ----------------------------------------------------------------------------------------- */

void gba_update_irq(gba_machine *m)
{
    m->irq_line = (m->ime & 1) && (m->ie & m->if_ & 0x3FFF);
    wr16(&m->io[0x200], m->ie);
    wr16(&m->io[0x202], m->if_);
    wr16(&m->io[0x208], m->ime);
    if (m->ie & m->if_ & 0x3FFF)
        m->cpu.halted = false; /* halt ends on IE & IF, whatever IME and CPSR.I say */
}

void gba_raise_irq(gba_machine *m, uint16_t bits)
{
    m->if_ |= bits;
    gba_update_irq(m);
}

static void check_keypad_irq(gba_machine *m)
{
    uint16_t kc = m->keycnt;
    if (!(kc & 0x4000))
        return;
    uint16_t sel = kc & 0x3FF, pressed = (uint16_t)(~m->keys & 0x3FF);
    bool hit = (kc & 0x8000) ? (sel && (pressed & sel) == sel) : (pressed & sel) != 0;
    if (hit) {
        m->cpu.stopped = false;
        gba_raise_irq(m, IRQ_KEYPAD);
    }
}

void gba_set_keys(gba_machine *m, uint16_t keys_pressed_mask)
{
    m->keys = (uint16_t)(~keys_pressed_mask & 0x3FF);
    wr16(&m->io[0x130], m->keys);
    check_keypad_irq(m);
}

/* ---- scheduling ----------------------------------------------------------------------------------------- */

static const uint32_t phase_offset[3] = {960, 1006, GBA_CYCLES_PER_LINE};

static inline uint64_t ppu_next(const gba_machine *m)
{
    return m->line_start + phase_offset[m->line_phase];
}

void gba_schedule(gba_machine *m)
{
    uint64_t next = ppu_next(m);
    for (int i = 0; i < 4; i++)
        if (m->timer[i].overflow_at < next)
            next = m->timer[i].overflow_at;
    m->next_event = next;
}

/* ---- DMA ------------------------------------------------------------------------------------------------ */

static void dma_run(gba_machine *m, int i)
{
    gba_dma *d = &m->dma[i];
    uint16_t ctl = d->cnt_h;
    int timing = (ctl >> 12) & 3;
    bool fifo = timing == DMA_SPECIAL && (i == 1 || i == 2);
    bool w32 = fifo || (ctl & 0x400);
    int dst_mode = fifo ? 2 : (ctl >> 5) & 3, src_mode = (ctl >> 7) & 3;
    int32_t step = w32 ? 4 : 2;
    int32_t src_inc = src_mode == 1 ? -step : src_mode == 2 ? 0 : step;
    int32_t dst_inc = dst_mode == 1 ? -step : dst_mode == 2 ? 0 : step;
    uint32_t count = fifo ? 4 : d->count;
    uint32_t src = d->src, dst = d->dst;

    if (src >= 0x08000000 && src < 0x0E000000)
        src_inc = step; /* the cartridge bus only counts up */

    bool seq = false;
    for (uint32_t n = 0; n < count; n++) {
        if (w32) {
            uint32_t v = src >= 0x02000000 ? gba_read32(m, src & ~3u, seq) : m->dma_latch;
            m->dma_latch = v;
            gba_write32(m, dst & ~3u, v, seq);
        } else {
            uint16_t v;
            if (src >= 0x02000000) {
                v = gba_read16(m, src & ~1u, seq);
                m->dma_latch = v * 0x00010001u;
            } else {
                v = (uint16_t)(m->dma_latch >> ((dst & 2) * 8));
            }
            gba_write16(m, dst & ~1u, v, seq);
        }
        seq = true;
        src += (uint32_t)src_inc;
        dst += (uint32_t)dst_inc;
    }
    m->now += 2;
    d->src = src;
    if (!fifo)
        d->dst = dst;

    if (ctl & 0x4000)
        gba_raise_irq(m, (uint16_t)(IRQ_DMA0 << i));
    if ((ctl & 0x200) && timing != DMA_NOW) {
        uint32_t c = rd16(&m->io[0xB8 + 12 * i]) & (i == 3 ? 0xFFFF : 0x3FFF);
        d->count = c ? c : (i == 3 ? 0x10000 : 0x4000);
        if (dst_mode == 3)
            d->dst = rd32(&m->io[0xB4 + 12 * i]) & (i == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
    } else {
        d->cnt_h &= 0x7FFF;
        wr16(&m->io[0xBA + 12 * i], d->cnt_h);
    }
}

static void dma_write_ctl(gba_machine *m, int i, uint16_t v)
{
    gba_dma *d = &m->dma[i];
    uint16_t old = d->cnt_h;
    d->cnt_h = v;
    if (!(old & 0x8000) && (v & 0x8000)) {
        d->src = rd32(&m->io[0xB0 + 12 * i]) & (i == 0 ? 0x07FFFFFF : 0x0FFFFFFF);
        d->dst = rd32(&m->io[0xB4 + 12 * i]) & (i == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
        uint32_t c = rd16(&m->io[0xB8 + 12 * i]) & (i == 3 ? 0xFFFF : 0x3FFF);
        d->count = c ? c : (i == 3 ? 0x10000 : 0x4000);
        if (((v >> 12) & 3) == DMA_NOW)
            dma_run(m, i);
    }
}

static void dma_trigger(gba_machine *m, int timing)
{
    for (int i = 0; i < 4; i++) {
        uint16_t ctl = m->dma[i].cnt_h;
        if ((ctl & 0x8000) && ((ctl >> 12) & 3) == timing)
            dma_run(m, i);
    }
}

/* Sound FIFO `f` (0 = A, 1 = B) consumed a sample: refill it through the DMA channel pointed at it. */
static void fifo_tick(gba_machine *m, int f)
{
    if (m->fifo_count[f] > 0)
        m->fifo_count[f]--;
    if (m->fifo_count[f] > 16)
        return;
    uint32_t fifo_addr = 0x040000A0u + 4u * (uint32_t)f;
    for (int i = 1; i <= 2; i++) {
        gba_dma *d = &m->dma[i];
        if ((d->cnt_h & 0x8000) && ((d->cnt_h >> 12) & 3) == DMA_SPECIAL && (d->dst & ~3u) == fifo_addr) {
            dma_run(m, i);
            break;
        }
    }
}

/* ---- timers --------------------------------------------------------------------------------------------- */

static inline bool timer_counting(const gba_machine *m, int i)
{
    uint16_t ctl = m->timer[i].ctl;
    return (ctl & 0x80) && !(i > 0 && (ctl & 0x04));
}

static uint16_t timer_value(const gba_machine *m, int i)
{
    const gba_timer *t = &m->timer[i];
    if (!timer_counting(m, i))
        return t->counter;
    uint64_t v = t->counter + ((m->now - t->base_time) >> t->shift);
    if (v >= 0x10000) { /* overflow due but not processed yet (mid-instruction read) */
        uint32_t period = 0x10000u - t->reload;
        v = t->reload + (v - 0x10000) % period;
    }
    return (uint16_t)v;
}

static void timer_set_due(gba_machine *m, int i)
{
    gba_timer *t = &m->timer[i];
    t->overflow_at = timer_counting(m, i) ? t->base_time + ((uint64_t)(0x10000u - t->counter) << t->shift) : UINT64_MAX;
}

static void timer_overflow(gba_machine *m, int i, uint64_t when)
{
    gba_timer *t = &m->timer[i];
    t->counter = t->reload;
    t->base_time = when;
    timer_set_due(m, i);
    if (t->ctl & 0x40)
        gba_raise_irq(m, (uint16_t)(IRQ_TIMER0 << i));
    if (i < 2) {
        uint16_t sh = rd16(&m->io[0x82]); /* SOUNDCNT_H: FIFO A / B timer select */
        if ((sh & 0x300) && ((sh >> 10) & 1) == i)
            fifo_tick(m, 0);
        if ((sh & 0x3000) && ((sh >> 14) & 1) == i)
            fifo_tick(m, 1);
    }
    if (i < 3) {
        gba_timer *n = &m->timer[i + 1];
        if ((n->ctl & 0x80) && (n->ctl & 0x04)) {
            if (++n->counter == 0)
                timer_overflow(m, i + 1, when);
        }
    }
}

static void timer_write_ctl(gba_machine *m, int i, uint16_t v)
{
    gba_timer *t = &m->timer[i];
    bool was = timer_counting(m, i);
    uint16_t old = t->ctl;
    uint64_t frac = 0;
    if (was) {
        frac = (m->now - t->base_time) & ((1u << t->shift) - 1);
        t->counter = timer_value(m, i);
    }
    t->ctl = v & 0xC7;
    if (!(old & 0x80) && (v & 0x80))
        t->counter = t->reload;
    int shift = timer_shift[v & 3];
    t->base_time = (was && shift == t->shift) ? m->now - frac : m->now;
    t->shift = shift;
    timer_set_due(m, i);
    gba_schedule(m);
}

/* ---- LCD timing ----------------------------------------------------------------------------------------- */

static void set_dispstat_flags(gba_machine *m, uint16_t set, uint16_t clear)
{
    uint16_t ds = rd16(&m->io[4]);
    ds = (uint16_t)((ds | set) & ~clear);
    wr16(&m->io[4], ds);
}

static void vcount_check(gba_machine *m)
{
    uint16_t ds = rd16(&m->io[4]);
    if (m->vcount == (uint32_t)(ds >> 8)) {
        if (!(ds & 4) && (ds & 0x20))
            gba_raise_irq(m, IRQ_VCOUNT);
        set_dispstat_flags(m, 4, 0);
    } else {
        set_dispstat_flags(m, 0, 4);
    }
}

static void latch_affine(gba_machine *m)
{
    m->bg2x = sext28(rd32(&m->io[0x28]));
    m->bg2y = sext28(rd32(&m->io[0x2C]));
    m->bg3x = sext28(rd32(&m->io[0x38]));
    m->bg3y = sext28(rd32(&m->io[0x3C]));
}

static void ppu_event(gba_machine *m)
{
    switch (m->line_phase) {
    case 0: /* end of HDraw: draw the line */
        if (m->vcount < GBA_SCREEN_H && m->fb) {
            gba_ppu_inputs in = {m->io, m->pal, m->vram, m->oam, m->bg2x, m->bg2y, m->bg3x, m->bg3y};
            gba_ppu_render_line(&in, (int)m->vcount, m->fb + m->vcount * GBA_SCREEN_W);
        }
        m->line_phase = 1;
        break;
    case 1: /* HBlank */
        set_dispstat_flags(m, 2, 0);
        if (m->io[4] & 0x10)
            gba_raise_irq(m, IRQ_HBLANK);
        if (m->vcount < GBA_SCREEN_H) {
            dma_trigger(m, DMA_HBLANK);
            m->bg2x += (int16_t)rd16(&m->io[0x22]);
            m->bg2y += (int16_t)rd16(&m->io[0x26]);
            m->bg3x += (int16_t)rd16(&m->io[0x32]);
            m->bg3y += (int16_t)rd16(&m->io[0x36]);
        }
        m->line_phase = 2;
        break;
    default: /* next line */
        m->line_start += GBA_CYCLES_PER_LINE;
        m->line_phase = 0;
        set_dispstat_flags(m, 0, 2);
        if (++m->vcount == GBA_LINES)
            m->vcount = 0;
        wr16(&m->io[6], (uint16_t)m->vcount);
        if (m->vcount == GBA_SCREEN_H) {
            set_dispstat_flags(m, 1, 0);
            if (m->io[4] & 0x08)
                gba_raise_irq(m, IRQ_VBLANK);
            dma_trigger(m, DMA_VBLANK);
            latch_affine(m);
            m->frames++;
            m->frame_done = true;
        } else if (m->vcount == GBA_LINES - 1) {
            set_dispstat_flags(m, 0, 1);
        }
        vcount_check(m);
        break;
    }
}

/* Handles every event due at m->now, then schedules the next one. */
static void run_events(gba_machine *m)
{
    bool any;
    do {
        any = false;
        if (m->now >= ppu_next(m)) {
            ppu_event(m);
            any = true;
        }
        for (int i = 0; i < 4; i++) {
            if (m->timer[i].overflow_at <= m->now) {
                timer_overflow(m, i, m->timer[i].overflow_at);
                any = true;
            }
        }
    } while (any);
    gba_schedule(m);
}

void gba_run_frame(gba_machine *m, uint16_t *framebuffer)
{
    m->fb = framebuffer;
    m->frame_done = false;
    while (!m->frame_done) {
        gba_cpu_run(m);
        run_events(m);
    }
    m->fb = NULL;
}

/* ---- register access ------------------------------------------------------------------------------------ */

uint16_t gba_io_read16(gba_machine *m, uint32_t addr)
{
    uint32_t a = addr & 0x3FE;
    switch (a) {
    case 0x100: case 0x104: case 0x108: case 0x10C:
        return timer_value(m, (int)(a - 0x100) >> 2);
    case 0x102: case 0x106: case 0x10A: case 0x10E:
        return m->timer[(a - 0x102) >> 2].ctl;
    case 0xBA: case 0xC6: case 0xD2: case 0xDE:
        return m->dma[(a - 0xBA) / 12].cnt_h;
    default:
        break;
    }
    if ((a >= 0x10 && a < 0x48) || a == 0x4C || (a >= 0x54 && a < 0x60) || (a >= 0xA0 && a < 0xBA) ||
        (a >= 0xBC && a < 0xC6) || (a >= 0xC8 && a < 0xD2) || (a >= 0xD4 && a < 0xDE) || (a >= 0xE0 && a < 0x100) ||
        (a >= 0x110 && a < 0x120) || (a >= 0x20C && a < 0x300) || a >= 0x304)
        return 0; /* write-only or unused */
    return rd16(&m->io[a]);
}

void gba_io_write16(gba_machine *m, uint32_t addr, uint16_t v)
{
    uint32_t a = addr & 0x3FE;
    switch (a) {
    case 0x004: /* DISPSTAT: flags are read-only */
        wr16(&m->io[4], (uint16_t)((rd16(&m->io[4]) & 7) | (v & 0xFF38)));
        vcount_check(m);
        return;
    case 0x006:
    case 0x130:
        return; /* VCOUNT, KEYINPUT: read-only */
    case 0x028: case 0x02A: case 0x02C: case 0x02E:
    case 0x038: case 0x03A: case 0x03C: case 0x03E:
        wr16(&m->io[a], v);
        latch_affine(m); /* writing a reference point reloads the internal one */
        return;
    case 0x0A0: case 0x0A2: case 0x0A4: case 0x0A6: {
        int f = a >= 0xA4;
        if (m->fifo_count[f] < 32)
            m->fifo_count[f] += 2;
        wr16(&m->io[a], v);
        return;
    }
    case 0x0BA: case 0x0C6: case 0x0D2: case 0x0DE:
        wr16(&m->io[a], v);
        dma_write_ctl(m, (int)(a - 0xBA) / 12, v);
        return;
    case 0x100: case 0x104: case 0x108: case 0x10C:
        m->timer[(a - 0x100) >> 2].reload = v;
        wr16(&m->io[a], v);
        return;
    case 0x102: case 0x106: case 0x10A: case 0x10E:
        wr16(&m->io[a], v);
        timer_write_ctl(m, (int)(a - 0x102) >> 2, v);
        return;
    case 0x132:
        m->keycnt = v;
        wr16(&m->io[a], v);
        check_keypad_irq(m);
        return;
    case 0x200:
        m->ie = v;
        gba_update_irq(m);
        return;
    case 0x202:
        m->if_ &= (uint16_t)~v;
        gba_update_irq(m);
        return;
    case 0x204:
        wr16(&m->io[a], v & 0x5FFF);
        gba_update_waitstates(m);
        return;
    case 0x208:
        m->ime = v & 1;
        gba_update_irq(m);
        return;
    case 0x300:
        m->io[0x300] = (uint8_t)v;
        gba_io_write8(m, addr + 1, (uint8_t)(v >> 8));
        return;
    default:
        wr16(&m->io[a], v);
        return;
    }
}

void gba_io_write8(gba_machine *m, uint32_t addr, uint8_t v)
{
    uint32_t a = addr & 0x3FF;
    switch (a) {
    case 0x202:
    case 0x203:
        gba_io_write16(m, addr & ~1u, (uint16_t)(v << ((a & 1) * 8))); /* IF: only these bits are acknowledged */
        return;
    case 0x300:
        m->io[0x300] = v;
        return;
    case 0x301: /* HALTCNT */
        if (v & 0x80)
            m->cpu.stopped = true;
        else if (!(m->ie & m->if_ & 0x3FFF))
            m->cpu.halted = true;
        return;
    case 0x0A0: case 0x0A1: case 0x0A2: case 0x0A3:
    case 0x0A4: case 0x0A5: case 0x0A6: case 0x0A7: {
        int f = a >= 0xA4;
        if (m->fifo_count[f] < 32)
            m->fifo_count[f]++;
        m->io[a] = v;
        return;
    }
    default: {
        uint16_t cur = rd16(&m->io[a & ~1u]);
        if (a & 1)
            cur = (uint16_t)((cur & 0x00FF) | v << 8);
        else
            cur = (uint16_t)((cur & 0xFF00) | v);
        gba_io_write16(m, addr & ~1u, cur);
        return;
    }
    }
}

void gba_io_reset(gba_machine *m)
{
    for (unsigned i = 0; i < GBA_IO_SIZE; i++)
        m->io[i] = 0;
    for (int i = 0; i < 4; i++) {
        m->timer[i] = (gba_timer){0};
        m->timer[i].overflow_at = UINT64_MAX;
        m->dma[i] = (gba_dma){0};
    }
    m->ie = m->if_ = m->ime = 0;
    m->irq_line = false;
    m->keycnt = 0;
    m->fifo_count[0] = m->fifo_count[1] = 0;
    gba_set_keys(m, 0);
    wr16(&m->io[0x20], 0x100); /* BG2PA, BG2PD, BG3PA, BG3PD: identity */
    wr16(&m->io[0x26], 0x100);
    wr16(&m->io[0x30], 0x100);
    wr16(&m->io[0x36], 0x100);
    wr16(&m->io[0x134], 0x8000); /* RCNT */
    m->vcount = 0;
    m->line_phase = 0;
    m->line_start = m->now;
    latch_affine(m);
    gba_update_waitstates(m);
    gba_schedule(m);
}
