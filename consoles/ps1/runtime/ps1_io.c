/* PS1 IO ports, interrupt controller, DMA, root counters and the scanline scheduler (P-4 / P-9, XBA-162, XBA-167).
 *
 * Scheduler: the CPU runs until the next event (hblank start, end of line, a timer reaching a value that raises an
 * IRQ, an SIO0 transfer finishing); events are absolute CPU cycle times. A scanline lasts cycles_per_line video cycles
 * (ps1_gpu_get_timing) = x 7/11 CPU cycles, carried exactly (remainder kept); its last (cycles_per_line - 2560) video
 * cycles are the hblank. VBlank covers lines 240..262 (NTSC) / 256..313 (PAL); its start raises IRQ 0 and ends
 * ps1_run_frame. The CD-ROM and SPU are ticked at every event and before any access to their registers.
 *
 * DMA: the words move at once when a channel starts. A burst (sync mode 0, OTC) stalls the CPU for its length; block
 * and linked-list transfers let the CPU run and complete (busy bit, DICR flag, IRQ) after their length. */
#include "ps1_internal.h"

#include <string.h>

#define HBLANK_START_V 2560u /* video cycles into a line where hblank starts */

static inline uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void ps1_irq_raise(ps1_machine *m, int irq)
{
    m->i_stat |= 1u << irq;
    ps1_cpu_update_irq(m);
}

void ps1_devices_sync(ps1_machine *m)
{
    uint64_t d = m->cycles - m->dev_time;
    if (!d)
        return;
    m->dev_time = m->cycles;
    while (d) {
        uint32_t n = d > 0x100000u ? 0x100000u : (uint32_t)d;
        if (m->cdrom)
            ps1_cdrom_tick(m->cdrom, n);
        if (m->spu)
            ps1_spu_tick(m->spu, n, m->cdrom);
        d -= n;
    }
}

/* ================================================================================================================
 * Timers
 * ============================================================================================================== */

static bool tmr_paused(ps1_machine *m, int i)
{
    const ps1_timer *t = &m->tmr[i];
    if (!(t->mode & 1))
        return false;
    int sync = (t->mode >> 1) & 3;
    if (i == 2)
        return sync == 0 || sync == 3;
    bool blank = i == 0 ? m->in_hblank : m->in_vblank;
    switch (sync) {
    case 0: return blank;
    case 1: return false;
    case 2: return !blank;
    default: return t->gate_wait;
    }
}

/* Clock source: 0 system clock, 1 dot clock, 2 hblank, 3 system clock / 8. */
static int tmr_source(const ps1_machine *m, int i)
{
    int s = (m->tmr[i].mode >> 8) & 3;
    (void)m;
    switch (i) {
    case 0: return (s & 1) ? 1 : 0;
    case 1: return (s & 1) ? 2 : 0;
    default: return (s & 2) ? 3 : 0;
    }
}

static void tmr_irq(ps1_machine *m, int i)
{
    ps1_timer *t = &m->tmr[i];
    if (!(t->mode & 0x40) && t->irq_done)
        return; /* one-shot */
    t->irq_done = true;
    if (t->mode & 0x80) { /* toggle bit 10: an IRQ on each 1 -> 0 edge */
        t->mode ^= 0x400;
        if (!(t->mode & 0x400))
            ps1_irq_raise(m, PS1_IRQ_TMR0 + i);
    } else { /* pulse: bit 10 goes low for a few cycles */
        t->mode |= 0x400;
        ps1_irq_raise(m, PS1_IRQ_TMR0 + i);
    }
}

/* Ticks until the counter next reaches its target or 0xFFFF (from value c). */
static uint32_t tmr_dist(const ps1_timer *t)
{
    uint32_t c = t->counter;
    if ((t->mode & 8) && c == t->target)
        return 1; /* wraps to 0 next */
    uint32_t dt = (uint32_t)(t->target - c) & 0xFFFFu;
    uint32_t df = 0xFFFFu - c;
    if (!dt)
        dt = 0x10000u;
    if (!df)
        df = 0x10000u;
    if ((t->mode & 8) && c < t->target)
        return dt; /* 0xFFFF is never reached below a reset target */
    return dt < df ? dt : df;
}

static void tmr_add(ps1_machine *m, int i, uint64_t ticks)
{
    ps1_timer *t = &m->tmr[i];
    while (ticks) {
        if ((t->mode & 8) && t->counter == t->target) {
            t->counter = 0;
            ticks--;
            if (t->target == 0) { /* stuck at 0 = target */
                t->mode |= 0x800;
                if (t->mode & 0x10)
                    tmr_irq(m, i);
                return;
            }
            continue;
        }
        uint32_t d = tmr_dist(t);
        if (ticks < d) {
            t->counter = (uint16_t)(t->counter + ticks);
            return;
        }
        t->counter = (uint16_t)(t->counter + d);
        ticks -= d;
        if (t->counter == t->target) {
            t->mode |= 0x800;
            if (t->mode & 0x10)
                tmr_irq(m, i);
        }
        if (t->counter == 0xFFFF) {
            t->mode |= 0x1000;
            if (t->mode & 0x20)
                tmr_irq(m, i);
        }
        /* Long runs with no IRQ left to raise: skip whole periods (the flags are already set by one pass). */
        uint64_t period = ((t->mode & 8) && t->counter <= t->target) ? (uint64_t)t->target + 1u : 0x10000u;
        if (ticks > 3 * period && (!(t->mode & 0x30) || (!(t->mode & 0x40) && t->irq_done)))
            ticks -= (ticks - 2 * period) / period * period;
    }
}

void ps1_timers_sync(ps1_machine *m)
{
    uint64_t d = m->cycles - m->tmr_time;
    if (!d)
        return;
    m->tmr_time = m->cycles;
    for (int i = 0; i < 3; i++) {
        ps1_timer *t = &m->tmr[i];
        if (tmr_paused(m, i))
            continue;
        uint64_t ticks;
        switch (tmr_source(m, i)) {
        case 0:
            ticks = d;
            break;
        case 3:
            ticks = (t->frac + d) >> 3;
            t->frac = (uint32_t)((t->frac + d) & 7);
            break;
        case 1: {
            uint64_t div = 7u * (uint64_t)(m->timing.dot_divider ? m->timing.dot_divider : 10);
            uint64_t acc = t->frac + d * 11u;
            ticks = acc / div;
            t->frac = (uint32_t)(acc % div);
            break;
        }
        default:
            ticks = 0; /* hblank: counted at the hblank events */
            break;
        }
        if (ticks)
            tmr_add(m, i, ticks);
    }
}

/* Cycle of the next timer IRQ, UINT64_MAX when none is due. */
static uint64_t tmr_next(ps1_machine *m)
{
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 3; i++) {
        ps1_timer *t = &m->tmr[i];
        if (!(t->mode & 0x30) || (!(t->mode & 0x40) && t->irq_done) || tmr_paused(m, i))
            continue;
        uint64_t d = tmr_dist(t), c;
        switch (tmr_source(m, i)) {
        case 0: c = d; break;
        case 3: c = d * 8 - t->frac; break;
        case 1: {
            uint64_t div = 7u * (uint64_t)(m->timing.dot_divider ? m->timing.dot_divider : 10);
            uint64_t need = d * div - t->frac;
            c = (need + 10) / 11;
            break;
        }
        default: continue;
        }
        if (c < 1)
            c = 1;
        if (m->tmr_time + c < best)
            best = m->tmr_time + c;
    }
    return best;
}

/* A blank (hblank for timer 0, vblank for timer 1) starts or ends. */
static void tmr_blank(ps1_machine *m, int i, bool start)
{
    ps1_timer *t = &m->tmr[i];
    if (!(t->mode & 1) || !start)
        return;
    int sync = (t->mode >> 1) & 3;
    if (sync == 1 || sync == 2)
        t->counter = 0;
    else if (sync == 3 && t->gate_wait)
        t->gate_wait = false;
}

static uint32_t tmr_read(ps1_machine *m, int i, uint32_t reg)
{
    ps1_timers_sync(m);
    ps1_timer *t = &m->tmr[i];
    switch (reg) {
    case 0: return t->counter;
    case 1: {
        uint32_t v = t->mode;
        t->mode &= ~0x1800u;
        return v;
    }
    case 2: return t->target;
    default: return 0;
    }
}

static void tmr_write(ps1_machine *m, int i, uint32_t reg, uint32_t v)
{
    ps1_timers_sync(m);
    ps1_timer *t = &m->tmr[i];
    switch (reg) {
    case 0:
        t->counter = (uint16_t)v;
        break;
    case 1:
        t->mode = (uint16_t)((v & 0x3FFu) | 0x400u);
        t->counter = 0;
        t->frac = 0;
        t->irq_done = false;
        t->gate_wait = (v & 1) && ((v >> 1) & 3) == 3 && i < 2;
        break;
    case 2:
        t->target = (uint16_t)v;
        break;
    default:
        break;
    }
    ps1_sched_recompute(m);
}

/* ================================================================================================================
 * DMA
 * ============================================================================================================== */

#define DMA_CHUNK 1024

static void dma_update_irq(ps1_machine *m, bool old31)
{
    uint32_t d = m->dicr;
    bool on = (d & 0x8000u) || ((d & 0x800000u) && ((d >> 16) & (d >> 24) & 0x7Fu));
    m->dicr = on ? d | 0x80000000u : d & ~0x80000000u;
    if (on && !old31)
        ps1_irq_raise(m, PS1_IRQ_DMA);
}

/* RAM -> device: n words from addr, stepping by step bytes. */
static void dma_to_device(ps1_machine *m, int ch, uint32_t addr, uint32_t n, int step)
{
    uint32_t buf[DMA_CHUNK];
    while (n) {
        uint32_t k = n > DMA_CHUNK ? DMA_CHUNK : n;
        for (uint32_t i = 0; i < k; i++) {
            buf[i] = rd32le(m->ram + (addr & 0x1FFFFCu));
            addr += (uint32_t)step;
        }
        switch (ch) {
        case 0: if (m->mdec) ps1_mdec_dma_write(m->mdec, buf, k); break;
        case 2: if (m->gpu) ps1_gpu_dma_write(m->gpu, buf, k); break;
        case 4: if (m->spu) ps1_spu_dma_write(m->spu, buf, k); break;
        default: break;
        }
        n -= k;
    }
}

static void dma_from_device(ps1_machine *m, int ch, uint32_t addr, uint32_t n, int step)
{
    uint32_t buf[DMA_CHUNK];
    while (n) {
        uint32_t k = n > DMA_CHUNK ? DMA_CHUNK : n;
        memset(buf, 0xFF, k * 4);
        switch (ch) {
        case 1: if (m->mdec) ps1_mdec_dma_read(m->mdec, buf, k); break;
        case 2: if (m->gpu) ps1_gpu_dma_read(m->gpu, buf, k); break;
        case 3: if (m->cdrom) ps1_cdrom_dma_read(m->cdrom, buf, k); break;
        case 4: if (m->spu) ps1_spu_dma_read(m->spu, buf, k); break;
        default: break;
        }
        for (uint32_t i = 0; i < k; i++) {
            wr32le(m->ram + (addr & 0x1FFFFCu), buf[i]);
            addr += (uint32_t)step;
        }
        n -= k;
    }
}

/* CPU cycles per word moved, by channel (approximate: bus timings of the devices). */
static uint32_t dma_word_cycles(int ch)
{
    static const uint8_t c[7] = {1, 1, 1, 24, 8, 1, 1};
    return c[ch];
}

static void dma_finish(ps1_machine *m, int ch)
{
    ps1_dma_chan *c = &m->dma[ch];
    m->dma_done_at[ch] = UINT64_MAX;
    c->chcr &= ~(0x01000000u | 0x10000000u);
    bool old31 = m->dicr & 0x80000000u;
    if (m->dicr & (1u << (16 + ch)))
        m->dicr |= 1u << (24 + ch);
    dma_update_irq(m, old31);
}

static void dma_run(ps1_machine *m, int ch)
{
    ps1_dma_chan *c = &m->dma[ch];
    uint32_t chcr = c->chcr;
    int sync = (int)((chcr >> 9) & 3);
    int step = (chcr & 2) ? -4 : 4;
    bool to_dev = chcr & 1;
    uint32_t addr = c->madr & 0xFFFFFCu;
    uint32_t words = 0;

    ps1_devices_sync(m);
    if (ch == 6) { /* OTC: an empty ordering table, backwards, last entry = end marker */
        uint32_t n = c->bcr & 0xFFFF;
        if (!n)
            n = 0x10000;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = i == n - 1 ? 0x00FFFFFFu : ((addr - 4) & 0x1FFFFCu);
            wr32le(m->ram + (addr & 0x1FFFFCu), v);
            addr -= 4;
        }
        words = n;
    } else if (sync == 2) { /* linked list (GPU) */
        uint32_t nodes = 0;
        for (;;) {
            uint32_t hdr = rd32le(m->ram + (addr & 0x1FFFFCu));
            uint32_t n = hdr >> 24;
            if (n && to_dev)
                dma_to_device(m, ch, addr + 4, n, 4);
            words += n + 1;
            if (hdr & 0x800000u) {
                addr = hdr & 0xFFFFFFu;
                break;
            }
            addr = hdr & 0xFFFFFCu;
            if (++nodes > 0x40000u) {
                /* A list that loops never ends: the channel stays busy (no completion, no IRQ) until the CPU
                 * stops it; on hardware it keeps transferring meanwhile. */
                c->madr = addr;
                m->cycles += words;
                return;
            }
        }
        c->madr = 0x00FFFFFFu;
    } else {
        uint32_t n;
        if (sync == 0) {
            n = c->bcr & 0xFFFF;
            if (!n)
                n = 0x10000;
        } else {
            uint32_t bs = c->bcr & 0xFFFF, ba = c->bcr >> 16;
            if (!bs)
                bs = 0x10000;
            n = bs * ba;
            c->bcr &= 0xFFFF;
        }
        if (to_dev)
            dma_to_device(m, ch, addr, n, step);
        else
            dma_from_device(m, ch, addr, n, step);
        if (sync == 1)
            c->madr = (addr + (uint32_t)step * n) & 0xFFFFFFu;
        words = n;
    }
    if (sync == 0 || ch == 6) {
        /* Burst: the CPU is stalled until it is done. */
        m->cycles += (uint64_t)words * dma_word_cycles(ch);
        dma_finish(m, ch);
    } else {
        /* Blocks / linked list: the CPU runs between the chunks; the channel reports busy until the end. */
        m->dma_done_at[ch] = m->cycles + (uint64_t)words * dma_word_cycles(ch);
        ps1_sched_recompute(m);
    }
}

static void dma_try(ps1_machine *m, int ch)
{
    uint32_t chcr = m->dma[ch].chcr;
    if (!(chcr & 0x01000000u) || !((m->dpcr >> (ch * 4 + 3)) & 1) || m->dma_done_at[ch] != UINT64_MAX)
        return;
    if (((chcr >> 9) & 3) == 0 && !(chcr & 0x10000000u))
        return;
    dma_run(m, ch);
}

static uint32_t dma_read(ps1_machine *m, uint32_t off)
{
    if (off >= 0x70) {
        switch (off) {
        case 0x70: return m->dpcr;
        case 0x74: return m->dicr;
        case 0x78: return 0x7FFAC68Bu;
        case 0x7C: return 0x00FFFFF7u;
        default: return 0;
        }
    }
    ps1_dma_chan *c = &m->dma[off >> 4];
    switch (off & 0xF) {
    case 0: return c->madr;
    case 4: return c->bcr;
    case 8: return c->chcr;
    default: return 0;
    }
}

static void dma_write(ps1_machine *m, uint32_t off, uint32_t v)
{
    if (off >= 0x70) {
        if (off == 0x70) {
            m->dpcr = v;
            for (int ch = 0; ch < 7; ch++)
                dma_try(m, ch);
        } else if (off == 0x74) {
            bool old31 = m->dicr & 0x80000000u;
            m->dicr = (m->dicr & 0x7F000000u & ~(v & 0x7F000000u)) | (v & 0x00FF803Fu);
            dma_update_irq(m, old31);
        }
        return;
    }
    int ch = (int)(off >> 4);
    ps1_dma_chan *c = &m->dma[ch];
    switch (off & 0xF) {
    case 0: c->madr = v & 0xFFFFFFu; break;
    case 4: c->bcr = v; break;
    case 8:
        if (ch == 6)
            c->chcr = (v & 0x51000000u) | 2u;
        else
            c->chcr = v & 0x71770703u;
        if (!(c->chcr & 0x01000000u) && m->dma_done_at[ch] != UINT64_MAX) {
            m->dma_done_at[ch] = UINT64_MAX; /* stopped by the CPU: no completion */
            ps1_sched_recompute(m);
        }
        dma_try(m, ch);
        break;
    default: break;
    }
}

/* ================================================================================================================
 * Scheduler
 * ============================================================================================================== */

static int vblank_line(const ps1_machine *m)
{
    return m->timing.pal ? 256 : 240;
}

static void line_times(ps1_machine *m)
{
    uint32_t cpl = (uint32_t)(m->timing.cycles_per_line ? m->timing.cycles_per_line : 3413);
    uint64_t v = (uint64_t)cpl * 7u + m->line_rem;
    m->next_line_at = m->line_at + v / 11u;
    m->line_rem = (uint32_t)(v % 11u);
    m->hblank_at = m->line_at + (HBLANK_START_V * 7u) / 11u;
}

static void get_timing(ps1_machine *m)
{
    ps1_gpu_timing t = {0, 0, 10, 263, 3413};
    if (m->gpu)
        ps1_gpu_get_timing(m->gpu, &t);
    if (t.lines < 200 || t.lines > 400)
        t.lines = t.pal ? 314 : 263;
    if (t.cycles_per_line < 2600 || t.cycles_per_line > 4000)
        t.cycles_per_line = t.pal ? 3406 : 3413;
    if (t.dot_divider <= 0)
        t.dot_divider = 10;
    m->timing = t;
}

void ps1_sched_init(ps1_machine *m)
{
    get_timing(m);
    m->line = 0;
    m->line_at = m->cycles;
    m->line_rem = 0;
    m->in_hblank = false;
    m->in_vblank = false;
    for (int ch = 0; ch < 7; ch++)
        m->dma_done_at[ch] = UINT64_MAX;
    m->tmr_time = m->cycles;
    m->dev_time = m->cycles;
    line_times(m);
    if (m->gpu)
        ps1_gpu_scanline(m->gpu, 0);
    ps1_sched_recompute(m);
}

void ps1_sched_recompute(ps1_machine *m)
{
    uint64_t n = m->next_line_at;
    if (!m->in_hblank && m->hblank_at < n)
        n = m->hblank_at;
    uint64_t t = tmr_next(m);
    if (t < n)
        n = t;
    if ((m->sio.busy || m->sio.ack_pending) && m->sio.done_at < n)
        n = m->sio.done_at;
    for (int ch = 0; ch < 7; ch++)
        if (m->dma_done_at[ch] < n)
            n = m->dma_done_at[ch];
    m->next_event = n;
}

void ps1_sched_events(ps1_machine *m)
{
    ps1_devices_sync(m);
    ps1_timers_sync(m);
    while ((m->sio.busy || m->sio.ack_pending) && m->cycles >= m->sio.done_at)
        ps1_sio_event(m);
    for (int ch = 0; ch < 7; ch++)
        if (m->cycles >= m->dma_done_at[ch])
            dma_finish(m, ch);
    if (!m->in_hblank && m->cycles >= m->hblank_at) {
        m->in_hblank = true;
        tmr_blank(m, 0, true);
        if (tmr_source(m, 1) == 2 && !tmr_paused(m, 1))
            tmr_add(m, 1, 1);
    }
    if (m->cycles >= m->next_line_at) {
        m->in_hblank = false;
        tmr_blank(m, 0, false);
        m->line_at = m->next_line_at;
        m->line++;
        if (m->line == vblank_line(m)) {
            m->in_vblank = true;
            tmr_blank(m, 1, true);
            ps1_irq_raise(m, PS1_IRQ_VBLANK);
            m->frame_done = true;
            m->frames++;
        }
        if (m->line >= m->timing.lines) {
            m->line = 0;
            m->in_vblank = false;
            tmr_blank(m, 1, false);
            get_timing(m);
        }
        line_times(m);
        if (m->gpu)
            ps1_gpu_scanline(m->gpu, m->line);
    }
    ps1_sched_recompute(m);
}

/* ================================================================================================================
 * IO dispatch
 * ============================================================================================================== */

static void tty_out(ps1_machine *m, uint32_t c)
{
    if (m->tty)
        m->tty(m->tty_ctx, (char)c);
}

uint32_t ps1_io_read(ps1_machine *m, uint32_t phys, int width)
{
    uint32_t off = phys - 0x1F801000u;
    uint32_t sh = (off & 3) * 8, v;

    if (off >= 0xC00 && off < 0x1000) { /* SPU: 16-bit registers */
        if (!m->spu)
            return 0;
        ps1_devices_sync(m);
        if (width == 4)
            return ps1_spu_read16(m->spu, off - 0xC00) | (uint32_t)ps1_spu_read16(m->spu, off - 0xC00 + 2) << 16;
        v = ps1_spu_read16(m->spu, (off - 0xC00) & ~1u);
        return width == 1 ? (v >> ((off & 1) * 8)) & 0xFF : v;
    }
    if (off >= 0x800 && off < 0x804) { /* CD-ROM: 8-bit registers */
        if (!m->cdrom)
            return width == 1 ? 0xFF : 0xFFFFFFFFu;
        ps1_devices_sync(m);
        v = ps1_cdrom_read(m->cdrom, off & 3);
        if (width == 2)
            v |= v << 8;
        else if (width == 4)
            v *= 0x01010101u;
        return v;
    }
    if (off >= 0x40 && off < 0x50)
        return ps1_sio_read(m, off - 0x40, width);
    if (off >= 0x1000) { /* expansion 2 */
        if (phys == 0x1F802021u)
            return 0x0C; /* DUART status A: transmitter ready / empty */
        return width == 1 ? 0xFF : width == 2 ? 0xFFFF : 0xFFFFFFFFu;
    }

    uint32_t a = off & ~3u;
    if (a < 0x24)
        v = m->mem_ctrl[a / 4];
    else if (a >= 0x50 && a < 0x60) {
        if (a == 0x54)
            v = 0x00000005u; /* SIO1 stat: TX ready, TX finished */
        else if (a == 0x58)
            v = (uint32_t)m->sio1_mode | (uint32_t)m->sio1_ctrl << 16;
        else if (a == 0x5C)
            v = (uint32_t)m->sio1_baud << 16;
        else
            v = 0;
    } else if (a == 0x60)
        v = m->ram_size;
    else if (a == 0x70)
        v = m->i_stat;
    else if (a == 0x74)
        v = m->i_mask;
    else if (a >= 0x80 && a < 0x100)
        v = dma_read(m, a - 0x80);
    else if (a >= 0x100 && a < 0x130)
        v = ((a & 0xF) < 0xC) ? tmr_read(m, (int)((a - 0x100) >> 4), (a & 0xF) >> 2) : 0;
    else if (a == 0x810)
        v = m->gpu ? ps1_gpu_read(m->gpu) : 0;
    else if (a == 0x814)
        v = m->gpu ? ps1_gpu_stat(m->gpu) : 0x1C000000u;
    else if (a == 0x820 || a == 0x824)
        v = m->mdec ? ps1_mdec_read(m->mdec, (a - 0x820) >> 2) : 0;
    else
        v = 0;
    v >>= sh;
    if (width == 1)
        v &= 0xFF;
    else if (width == 2)
        v &= 0xFFFF;
    return v;
}

void ps1_io_write(ps1_machine *m, uint32_t phys, uint32_t v, int width)
{
    uint32_t off = phys - 0x1F801000u;

    if (off >= 0xC00 && off < 0x1000) {
        if (!m->spu)
            return;
        ps1_devices_sync(m);
        if (width == 4) {
            ps1_spu_write16(m->spu, off - 0xC00, (uint16_t)v);
            ps1_spu_write16(m->spu, off - 0xC00 + 2, (uint16_t)(v >> 16));
        } else {
            ps1_spu_write16(m->spu, (off - 0xC00) & ~1u, (uint16_t)v);
        }
        return;
    }
    if (off >= 0x800 && off < 0x804) {
        if (m->cdrom) {
            ps1_devices_sync(m);
            ps1_cdrom_write(m->cdrom, off & 3, (uint8_t)v);
        }
        return;
    }
    if (off >= 0x40 && off < 0x50) {
        ps1_sio_write(m, off - 0x40, v, width);
        return;
    }
    if (off >= 0x1000) {
        if (phys == 0x1F802041u)
            m->post = (uint8_t)v;
        else if (phys == 0x1F802023u || phys == 0x1F802080u)
            tty_out(m, v & 0xFF);
        return;
    }

    /* 32-bit registers: a narrower store lands shifted into the aligned word. */
    uint32_t a = off & ~3u;
    if (width != 4)
        v <<= (off & 3) * 8;
    if (a < 0x24)
        m->mem_ctrl[a / 4] = v;
    else if (a >= 0x50 && a < 0x60) {
        if (a == 0x58) {
            if (width == 4 || (off & 3) == 0)
                m->sio1_mode = (uint16_t)v;
            if (width == 4 || (off & 3) == 2)
                m->sio1_ctrl = (uint16_t)(v >> 16);
        } else if (a == 0x5C)
            m->sio1_baud = (uint16_t)(v >> 16);
    } else if (a == 0x60)
        m->ram_size = v;
    else if (a == 0x70) {
        m->i_stat &= v;
        ps1_cpu_update_irq(m);
    } else if (a == 0x74) {
        m->i_mask = v & 0x7FFu;
        ps1_cpu_update_irq(m);
    } else if (a >= 0x80 && a < 0x100)
        dma_write(m, a - 0x80, v);
    else if (a >= 0x100 && a < 0x130) {
        if ((a & 0xF) < 0xC)
            tmr_write(m, (int)((a - 0x100) >> 4), (a & 0xF) >> 2, v);
    } else if (a == 0x810) {
        if (m->gpu)
            ps1_gpu_gp0(m->gpu, v);
    } else if (a == 0x814) {
        if (m->gpu) {
            ps1_gpu_gp1(m->gpu, v);
            if ((v >> 24) == 0x08 || (v >> 24) == 0x00) { /* display mode: dot clock / standard may change */
                ps1_timers_sync(m);
                get_timing(m);
                ps1_sched_recompute(m);
            }
        }
    } else if (a == 0x820 || a == 0x824) {
        if (m->mdec)
            ps1_mdec_write(m->mdec, (a - 0x820) >> 2, v);
    }
}
