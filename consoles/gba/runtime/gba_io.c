/* GBA IO registers (XBA-153): LCD timing (DISPSTAT / VCOUNT), the four timers with prescalers and cascade, the
 * four DMA channels, interrupts (IE / IF / IME), HALTCNT, KEYINPUT / KEYCNT, WAITCNT. GBATEK « GBA I/O Map ».
 *
 * m->io keeps the 0x400-byte register image as the CPU wrote it (the PPU reads it directly); registers with
 * hardware state (timer counters, IF, KEYINPUT, DISPSTAT flags, VCOUNT) are kept in sync or computed on read.
 * Sound: registers 0x60-0xA7 belong to the APU (gba_audio.c); timer 0 / 1 overflows pop the Direct Sound FIFOs
 * and DMA1 / DMA2 in special timing refill them. DMA3 transfers to the EEPROM tell the save code the address
 * width (gba_save.c). */
#include "gba_internal.h"

static const int timer_shift[4] = {0, 6, 8, 10};
/* Timer and interrupt timing, fitted to the mGBA suite's hardware results (timer count-up, timer IRQ; XBA-201): a
 * timer counts from the cycle after the write that starts it; a CPU read sees the counter as it was when the access
 * began (one cycle before the access ends, where m->now stands); an interrupt request reaches the CPU
 * GBA_IRQ_LATENCY cycles after its flag rises (gba_update_irq, irq_at). */
#define TIMER_START_DELAY 1
#define TIMER_READ_EARLY 1

static inline int32_t sext28(uint32_t v)
{
    return (int32_t)(v << 4) >> 4;
}

/* ---- interrupts ----------------------------------------------------------------------------------------- */

#define GBA_IRQ_LATENCY 4

static void update_irq_at(gba_machine *m, uint64_t when)
{
    bool pending = (m->ime & 1) && (m->ie & m->if_ & 0x3FFF);
    if (!pending) {
        m->irq_line = false;
        m->irq_at = UINT64_MAX;
    } else if (!m->irq_line) {
        if (m->irq_at == UINT64_MAX)
            m->irq_at = when + GBA_IRQ_LATENCY;
        if (m->irq_at <= m->now) {
            m->irq_line = true;
            m->irq_at = UINT64_MAX;
            m->cpu.halted = false;
        } else if (m->irq_at < m->next_event) {
            m->next_event = m->irq_at;
        }
    }
    wr16(&m->io[0x200], m->ie);
    wr16(&m->io[0x202], m->if_);
    wr16(&m->io[0x208], m->ime);
    /* Halt ends on IE & IF, whatever IME and CPSR.I say; with IME set, when the request reaches the CPU (irq_at, so the
     * IRQ is taken first thing after the halt, as on hardware). */
    if ((m->ie & m->if_ & 0x3FFF) && (!(m->ime & 1) || m->irq_line))
        m->cpu.halted = false;
}

void gba_update_irq(gba_machine *m)
{
    update_irq_at(m, m->now);
}

void gba_raise_irq(gba_machine *m, uint16_t bits)
{
    m->if_ |= bits;
    update_irq_at(m, m->now);
}

void gba_raise_irq_at(gba_machine *m, uint16_t bits, uint64_t when)
{
    m->if_ |= bits;
    update_irq_at(m, when);
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
    if (m->irq_at < next)
        next = m->irq_at;
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
    if (i == 3 && gba_eeprom_area(m, dst))
        gba_eeprom_dma_hint(m, count);

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

/* Sound FIFO `f` (0 = A, 1 = B) plays its next sample; at 16 bytes or less, the DMA channel pointed at it refills
 * it (4 words). */
static void fifo_tick(gba_machine *m, int f, uint64_t when)
{
    if (gba_apu_fifo_tick(m, f, when) > 16)
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

/* The counter at cycle `now` (>= the last processed overflow). */
static uint16_t timer_value(const gba_machine *m, int i, uint64_t now)
{
    const gba_timer *t = &m->timer[i];
    if (!timer_counting(m, i) || now < t->base_time)
        return t->counter;
    uint64_t v = t->counter + ((now - t->base_time) >> t->shift);
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
        gba_raise_irq_at(m, (uint16_t)(IRQ_TIMER0 << i), when);
    if (i < 2) {
        uint16_t sh = rd16(&m->io[0x82]); /* SOUNDCNT_H: FIFO A / B timer select */
        if (((sh >> 10) & 1) == i)
            fifo_tick(m, 0, when);
        if (((sh >> 14) & 1) == i)
            fifo_tick(m, 1, when);
    }
    if (i < 3) {
        gba_timer *n = &m->timer[i + 1];
        if ((n->ctl & 0x80) && (n->ctl & 0x04)) {
            if (++n->counter == 0)
                timer_overflow(m, i + 1, when);
        }
    }
}

/* Overflows due by now happen before a timer register write that lands at the same cycle (the write sees the
 * timers as they are at its end). */
static void timers_catch_up(gba_machine *m)
{
    bool any;
    do {
        any = false;
        for (int i = 0; i < 4; i++)
            if (m->timer[i].overflow_at <= m->now) {
                timer_overflow(m, i, m->timer[i].overflow_at);
                any = true;
            }
    } while (any);
}

static void timer_write_ctl(gba_machine *m, int i, uint16_t v)
{
    timers_catch_up(m);
    gba_timer *t = &m->timer[i];
    bool was = timer_counting(m, i);
    uint16_t old = t->ctl;
    uint64_t frac = 0;
    if (was) {
        frac = (m->now - t->base_time) & ((1u << t->shift) - 1);
        t->counter = timer_value(m, i, m->now);
    }
    t->ctl = v & 0xC7;
    if (!(old & 0x80) && (v & 0x80))
        t->counter = t->reload;
    int shift = timer_shift[v & 3];
    if (was && shift == t->shift)
        t->base_time = m->now - frac;
    else /* the prescaler is a free-running divider: a prescaled timer ticks on multiples of its period */
        t->base_time = (m->now + TIMER_START_DELAY) & ~(((uint64_t)1 << shift) - 1);
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
        if (m->irq_at <= m->now) { /* the request reaches the CPU (and ends a halt) */
            m->irq_line = true;
            m->irq_at = UINT64_MAX;
            m->cpu.halted = false;
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
    gba_apu_sync(m, m->now); /* the frame's audio is in the output ring */
    m->fb = NULL;
}

/* ---- register access ------------------------------------------------------------------------------------ */

/* Registers that do not drive the data bus when read (write-only or unmapped halfwords): the CPU sees the open bus
 * (the last prefetched opcode, gba_prefetch_latch). The other unreadable halfwords (the upper half of a 32-bit
 * register, a few holes) read as 0. GBATEK « GBA I/O Map », checked by the mGBA suite's I/O read tests (XBA-201). */
bool gba_io_open_bus(uint32_t addr)
{
    uint32_t a = addr & 0x3FE;
    return (a >= 0x10 && a < 0x48) || a == 0x4C || a == 0x4E || (a >= 0x54 && a < 0x60) || a == 0x8C || a == 0x8E ||
           (a >= 0xA0 && a < 0xB8) || (a >= 0xBC && a < 0xC4) || (a >= 0xC8 && a < 0xD0) || (a >= 0xD4 && a < 0xDC) ||
           (a >= 0xE0 && a < 0x100);
}

uint16_t gba_io_read16(gba_machine *m, uint32_t addr)
{
    uint32_t a = addr & 0x3FE;
    switch (a) {
    case 0x100: case 0x104: case 0x108: case 0x10C: {
        /* Before an overflow processed this cycle, timer_value gives the reloaded value (base_time > now). */
        return timer_value(m, (int)(a - 0x100) >> 2, m->now - TIMER_READ_EARLY);
    }
    case 0x102: case 0x106: case 0x10A: case 0x10E:
        return m->timer[(a - 0x102) >> 2].ctl;
    case 0xBA: case 0xC6: case 0xD2:
        return m->dma[(a - 0xBA) / 12].cnt_h & 0xF7E0;
    case 0xDE:
        return m->dma[3].cnt_h & 0xFFE0;
    case 0x008: case 0x00A: /* BG0CNT, BG1CNT: no wrap-around bit */
        return rd16(&m->io[a]) & 0xDFFF;
    case 0x048: case 0x04A: /* WININ, WINOUT */
        return rd16(&m->io[a]) & 0x3F3F;
    case 0x050: /* BLDCNT */
        return rd16(&m->io[a]) & 0x3FFF;
    case 0x052: /* BLDALPHA */
        return rd16(&m->io[a]) & 0x1F1F;
    case 0x136: case 0x142: case 0x15A: case 0x206: case 0x20A: case 0x302:
        return 0;
    /* Serial port with no link cable (GBATEK « GBA Communication Ports », the mGBA suite's SIO register tests):
     * what reads back depends on the mode (SIOCNT bits 12-13, RCNT bits 14-15). */
    case 0x120: case 0x122: /* SIODATA32 in 32-bit normal mode; SIOMULTI0-1 (nothing received) otherwise */
        return (rd16(&m->io[0x128]) & 0x3000) == 0x1000 && !(m->io[0x135] & 0x80) ? rd16(&m->io[a]) : 0;
    case 0x124: case 0x126:
        return 0;
    case 0x128: { /* SIOCNT: bit 15 and the error / ID bits read as 0 (UART: bit 5 is CTS) */
        uint16_t v = rd16(&m->io[a]);
        return (uint16_t)(v & ((v & 0x3000) == 0x3000 ? ~0x8050u : ~0x8070u));
    }
    case 0x12A: /* SIODATA8 / SIOMLT_SEND (UART: the empty receive FIFO) */
        return (rd16(&m->io[0x128]) & 0x3000) == 0x3000 ? 0 : rd16(&m->io[a]);
    case 0x134: { /* RCNT: bits 9-13 unused; SC / SD (normal mode) and SC / SD (JOY mode) read low */
        uint16_t v = rd16(&m->io[a]) & 0xC1FF;
        if (!(v & 0x8000) && !(rd16(&m->io[0x128]) & 0x2000))
            v &= (uint16_t)~0x000A;
        else if ((v & 0xC000) == 0xC000)
            v &= (uint16_t)~0x0003;
        return v;
    }
    case 0x140: /* JOYCNT: the IRQ enable bit (the flags are write-1-to-clear) */
        return rd16(&m->io[a]) & 0x0040;
    case 0x150: case 0x152: case 0x154: case 0x156: /* JOY_RECV / JOY_TRANS */
        return 0;
    default:
        break;
    }
    if (a >= 0x60 && a < 0xA0)
        return (uint16_t)(gba_apu_read8(m, a) | gba_apu_read8(m, a + 1) << 8);
    if ((a >= 0x10 && a < 0x48) || a == 0x4C || (a >= 0x54 && a < 0x60) || (a >= 0xA0 && a < 0xBA) ||
        (a >= 0xBC && a < 0xC6) || (a >= 0xC8 && a < 0xD2) || (a >= 0xD4 && a < 0xDE) || (a >= 0xE0 && a < 0x100) ||
        (a >= 0x110 && a < 0x120) || (a >= 0x20C && a < 0x300) || a >= 0x304)
        return 0; /* write-only or unused (the CPU's reads of the open-bus ones never get here: gba_io_open_bus) */
    return rd16(&m->io[a]);
}

void gba_io_write16(gba_machine *m, uint32_t addr, uint16_t v)
{
    uint32_t a = addr & 0x3FE;
    if (a >= 0x60 && a < 0xA8) { /* sound: byte by byte, low byte first (a FIFO gets them in that order) */
        gba_apu_write8(m, a, (uint8_t)v);
        gba_apu_write8(m, a + 1, (uint8_t)(v >> 8));
        return;
    }
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
    case 0x0BA: case 0x0C6: case 0x0D2: case 0x0DE:
        wr16(&m->io[a], v);
        dma_write_ctl(m, (int)(a - 0xBA) / 12, v);
        return;
    case 0x100: case 0x104: case 0x108: case 0x10C:
        timers_catch_up(m);
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
    if (a >= 0x60 && a < 0xA8) {
        gba_apu_write8(m, a, v);
        return;
    }
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
    m->irq_at = UINT64_MAX;
    m->keycnt = 0;
    gba_apu_reset(m);
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
