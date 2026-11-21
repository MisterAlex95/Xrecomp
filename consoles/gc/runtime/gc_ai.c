/* gc_ai.c -- the audio interface at 0xCC006C00 and the audio DMA (XBA-263): the DMA reads 32-byte blocks of 16-bit
 * big-endian stereo (right, then left) from main memory at 32 or 48 kHz (AICR's DMA rate bit) and raises the DSP
 * interface's audio DMA interrupt each time it latches a buffer; the drive's streamed audio (gc_di.c) plays at 32 or
 * 48 kHz while AICR's play bit is set, at AIVR's volume, and counts the streaming sample counter (AISCNT) whose
 * match with AIIT raises the AI interrupt. Both are resampled to 48 kHz and mixed into the ring gc_audio_pull empties.
 *
 * AICR bits: 0 play (PSTAT), 1 stream rate (1 = 48 kHz), 2 interrupt mask, 3 interrupt (write 1 to clear),
 * 4 interrupt valid, 5 counter reset (write 1), 6 DMA rate (1 = 32 kHz). Rates are the nominal 32 / 48 kHz against
 * the 486 MHz CPU clock (a DMA block every 121500 / 81000 cycles, a 48 kHz frame every 10125). */
#include <string.h>

#include "gc_dsp_internal.h"

#define AICR_PSTAT 0x01u
#define AICR_AISFR 0x02u
#define AICR_MASK 0x04u
#define AICR_INT 0x08u
#define AICR_VLD 0x10u
#define AICR_SCRESET 0x20u
#define AICR_AIDFR 0x40u

#define OUT_CYCLES (GC_CPU_HZ / GC_AUDIO_RATE)  /* 10125 */
#define OUT_LATENCY (2ull * 8u * GC_CPU_HZ / 32000u) /* two 32 kHz blocks: the DMA's frames are there in time */

static uint32_t dma_rate(const gc_audio *a)
{
    return (a->aicr & AICR_AIDFR) ? 32000u : 48000u;
}

static uint32_t stream_rate(const gc_audio *a)
{
    return (a->aicr & AICR_AISFR) ? 48000u : 32000u;
}

/* 8ull * GC_CPU_HZ / dma_rate(a), the two rates' quotients as constants (no division at every block) */
static uint64_t block_cycles(const gc_audio *a)
{
    return (a->aicr & AICR_AIDFR) ? 8ull * GC_CPU_HZ / 32000u : 8ull * GC_CPU_HZ / 48000u;
}

/* ---- the streaming sample counter and its interrupt ---- */

static uint32_t scnt(gc_machine *m)
{
    gc_audio *a = m->audio;
    if (!(a->aicr & AICR_PSTAT))
        return a->scnt_base;
    return a->scnt_base + (uint32_t)((m->h.now - a->scnt_t0) * stream_rate(a) / GC_CPU_HZ);
}

static void scnt_rebase(gc_machine *m)
{
    gc_audio *a = m->audio;
    a->scnt_base = scnt(m);
    a->scnt_t0 = m->h.now;
}

static void ai_pi_update(gc_machine *m)
{
    gc_audio *a = m->audio;
    if ((a->aicr & AICR_INT) && (a->aicr & AICR_MASK))
        m->pi_intsr |= GC_INT_AI;
    else
        m->pi_intsr &= ~GC_INT_AI;
    gc_check_interrupts(m);
}

/* When the counter next goes past AIIT (it equals AIIT one sample after it was below). */
static void aiit_schedule(gc_machine *m)
{
    gc_audio *a = m->audio;
    a->aiit_at = UINT64_MAX;
    if (a->aicr & AICR_PSTAT) {
        uint32_t d = a->aiit - a->scnt_base;
        uint64_t elapsed = m->h.now - a->scnt_t0;
        uint64_t have = elapsed * stream_rate(a) / GC_CPU_HZ; /* samples counted since the base */
        if (d && d > have) {
            uint64_t r = stream_rate(a);
            a->aiit_at = a->scnt_t0 + ((uint64_t)d * GC_CPU_HZ + r - 1) / r;
        }
    }
    gc_schedule(m);
}

/* ---- registers ---- */

uint32_t gc_ai_read32(gc_machine *m, unsigned idx)
{
    gc_audio *a = m->audio;
    switch (idx) {
    case 0: return a->aicr;
    case 1: return a->aivr;
    case 2: return scnt(m);
    case 3: return a->aiit;
    default: return 0;
    }
}

void gc_ai_write32(gc_machine *m, unsigned idx, uint32_t v)
{
    gc_audio *a = m->audio;
    if (m->verbose >= 2)
        fprintf(stderr, "gc: AI[%u] = %08X\n", idx * 4, v);
    switch (idx) {
    case 0: {
        scnt_rebase(m);
        uint32_t old = a->aicr;
        uint32_t keep = AICR_MASK | AICR_VLD | AICR_AISFR | AICR_AIDFR | AICR_PSTAT;
        a->aicr = (old & AICR_INT) | (v & keep);
        if (v & AICR_INT)
            a->aicr &= ~AICR_INT;
        if (v & AICR_SCRESET)
            a->scnt_base = 0;
        if ((old ^ a->aicr) & AICR_AIDFR) { /* the DMA's rate: the next block at the new period */
            gc_resampler_set_rate(&a->rs_dma, dma_rate(a));
            a->tick_at = m->h.now + block_cycles(a);
        }
        if ((old ^ a->aicr) & AICR_AISFR)
            gc_resampler_set_rate(&a->rs_stream, stream_rate(a));
        ai_pi_update(m);
        aiit_schedule(m);
        break;
    }
    case 1:
        a->aivr = v;
        break;
    case 2:
        a->scnt_base = v;
        a->scnt_t0 = m->h.now;
        aiit_schedule(m);
        break;
    case 3:
        scnt_rebase(m);
        a->aiit = v;
        aiit_schedule(m);
        break;
    default:
        break;
    }
}

/* ---- the audio DMA ---- */

static uint32_t dma_start(const gc_audio *a)
{
    return ((uint32_t)a->reg[0x18] << 16 | a->reg[0x19]) & 0x03FFFFE0u;
}

void gc_ai_dma_control(gc_machine *m, uint16_t old, uint16_t v)
{
    gc_audio *a = m->audio;
    (void)old;
    if (!(v & 0x8000)) {
        a->aid_on = false;
        return;
    }
    if (!a->aid_on) { /* latched now; later values are taken when this buffer ends */
        a->aid_on = true;
        a->aid_cur = dma_start(a);
        a->aid_left = v & 0x7FFF;
        uint64_t t = m->h.now + 200;
        if (t < a->aidint_at)
            a->aidint_at = t;
        gc_schedule(m);
    }
}

uint16_t gc_ai_dma_blocks_left(gc_machine *m)
{
    gc_audio *a = m->audio;
    return (uint16_t)(a->aid_left > 0 ? a->aid_left - 1 : 0);
}

static void fifo_push(gc_audio *a, int16_t l, int16_t r)
{
    if (a->fn == GC_AID_FIFO) {
        a->fh = (a->fh + 1) % GC_AID_FIFO;
        a->fn--;
    }
    unsigned i = (a->fh + a->fn) % GC_AID_FIFO;
    a->fifo[i][0] = l;
    a->fifo[i][1] = r;
    a->fn++;
}

static void dma_block(gc_machine *m)
{
    gc_audio *a = m->audio;
    if (!a->aid_on) {
        for (int i = 0; i < 8; i++)
            fifo_push(a, 0, 0);
        return;
    }
    if (a->aid_cur <= GC_RAM_SIZE - 32u) { /* the whole block in MEM1 (aid_cur stays below dsp_pa's mask) */
        const uint8_t *b = m->h.ram + a->aid_cur;
        for (int i = 0; i < 8; i++, b += 4)
            fifo_push(a, (int16_t)(b[2] << 8 | b[3]), (int16_t)(b[0] << 8 | b[1]));
    } else {
        for (int i = 0; i < 8; i++) {
            uint32_t p = a->aid_cur + 4u * (uint32_t)i;
            int16_t r = (int16_t)dsp_rd16(m, p), l = (int16_t)dsp_rd16(m, p + 2);
            fifo_push(a, l, r);
        }
    }
    a->dma_blocks++;
    if (a->aid_left) {
        a->aid_left--;
        a->aid_cur += 32;
    }
    if (!a->aid_left) { /* the buffer ends: the next one (the registers as they are now) is latched */
        a->aid_cur = dma_start(a);
        a->aid_left = a->reg[0x1B] & 0x7FFF;
        if (a->aid_left)
            gc_dsp_irq(m, DSPCR_AIDINT);
    }
}

/* ---- mixing to 48 kHz ---- */

static void fetch_stream(void *ctx, int16_t lr[2])
{
    gc_machine *m = ctx;
    gc_audio *a = m->audio;
    lr[0] = lr[1] = 0;
    if (!(a->aicr & AICR_PSTAT) || !m->di || !gc_di_stream_frame(m, lr))
        return;
    a->stream_frames++;
    int vl = (int)(a->aivr & 0xFF), vr = (int)((a->aivr >> 8) & 0xFF);
    lr[0] = (int16_t)(lr[0] * vl / 255);
    lr[1] = (int16_t)(lr[1] * vr / 255);
}

static int16_t clamp16(int64_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static bool hist_silent(const gc_resampler *r)
{
    return !(r->hist[0][0] | r->hist[0][1] | r->hist[1][0] | r->hist[1][1] | r->hist[2][0] | r->hist[2][1] |
             r->hist[3][0] | r->hist[3][1]);
}

/* gc_resampler_next(&a->rs_dma, out, fetch, a) with the fetch from the DMA's frames (fifo, *fh, *fn) inlined: the
 * same arithmetic, frame for frame (gc_audio_util.c). t's division is by a constant at the 48 kHz output, and t = 0
 * (the DMA at 48 kHz: every frame) gives hist[1], as the polynomial does: v = c1 * 0 / 65536 = 0. */
static inline void rs_dma_next(gc_resampler *r, const int16_t (*fifo)[2], unsigned *fh, unsigned *fn,
                               int16_t out[2])
{
    r->phase += r->in_rate;
    while (r->phase >= r->out_rate) {
        r->phase -= r->out_rate;
        int32_t l = 0, rr = 0; /* an empty FIFO gives silence */
        if (*fn) {
            l = fifo[*fh][0];
            rr = fifo[*fh][1];
            *fh = (*fh + 1) % GC_AID_FIFO;
            (*fn)--;
        }
        r->hist[0][0] = r->hist[1][0], r->hist[0][1] = r->hist[1][1];
        r->hist[1][0] = r->hist[2][0], r->hist[1][1] = r->hist[2][1];
        r->hist[2][0] = r->hist[3][0], r->hist[2][1] = r->hist[3][1];
        r->hist[3][0] = l, r->hist[3][1] = rr;
    }
    if (hist_silent(r)) {
        out[0] = out[1] = 0;
        return;
    }
    int64_t t = r->out_rate == GC_AUDIO_RATE ? (int64_t)r->phase * 65536 / GC_AUDIO_RATE
                                             : (int64_t)r->phase * 65536 / r->out_rate;
    if (t == 0) {
        out[0] = clamp16(r->hist[1][0]);
        out[1] = clamp16(r->hist[1][1]);
        return;
    }
    for (int c = 0; c < 2; c++) {
        int64_t p0 = r->hist[0][c], p1 = r->hist[1][c], p2 = r->hist[2][c], p3 = r->hist[3][c];
        int64_t c1 = p2 - p0, c2 = 2 * p0 - 5 * p1 + 4 * p2 - p3, c3 = (p3 - p0) + 3 * (p1 - p2);
        int64_t v = c3 * t / 65536 + c2;
        v = v * t / 65536 + c1;
        v = v * t / 65536;
        out[c] = clamp16(p1 + v / 2);
    }
}

/* The 48 kHz frames up to upto: the DMA's resampler, FIFO and the ring in locals for the loop (the stream's fetch
 * touches none of them). quiet: the stream gives silence and its phase has moved already (produce). */
static inline __attribute__((always_inline)) void produce_span(gc_machine *m, gc_audio *a, uint64_t upto,
                                                               bool quiet)
{
    gc_resampler *st = &a->rs_stream;
    gc_resampler rd = a->rs_dma;
    unsigned fh = a->fh, fn = a->fn, rh = a->rh, rn = a->rn;
    uint64_t done = a->out_done, overruns = a->overruns;
    while (done < upto) {
        int16_t d[2], s[2] = {0, 0};
        rs_dma_next(&rd, (const int16_t (*)[2])a->fifo, &fh, &fn, d);
        if (!quiet)
            gc_resampler_next(st, s, fetch_stream, m);
        int32_t l = d[0] + s[0], r = d[1] + s[1];
        if (rn == GC_OUT_RING) {
            rh = (rh + 1) % GC_OUT_RING;
            rn--;
            overruns++;
        }
        unsigned i = (rh + rn) % GC_OUT_RING;
        a->ring[i][0] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
        a->ring[i][1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
        rn++;
        done++;
    }
    a->rs_dma = rd;
    a->fh = fh, a->fn = fn, a->rh = rh, a->rn = rn;
    a->out_done = done, a->overruns = overruns;
}

/* produce_span(quiet) when the DMA's side is silent too: its history silence and every FIFO frame the span takes
 * silence (the AI DMA off pushes zero frames). Then every output frame is silence and only the DMA's phase and FIFO
 * move: (phase + frames * in_rate) / out_rate frames taken (an empty FIFO gives silence), the rest of the sum the new
 * phase, as frame by frame. true when done, false (nothing done) otherwise: a game's sound (or a GX demo's quiet). */
static bool produce_silence(gc_audio *a, uint64_t upto)
{
    gc_resampler *rd = &a->rs_dma;
    uint64_t n = upto - a->out_done;
    if (!hist_silent(rd) || rd->phase >= rd->out_rate || (uint64_t)rd->out_rate + rd->in_rate > UINT32_MAX)
        return false;
    uint64_t sum = (uint64_t)rd->phase + n * rd->in_rate, taken = sum / rd->out_rate;
    unsigned k = taken < a->fn ? (unsigned)taken : a->fn;
    for (unsigned j = 0, i = a->fh; j < k; j++, i = (i + 1) % GC_AID_FIFO)
        if (a->fifo[i][0] | a->fifo[i][1])
            return false;
    rd->phase = (uint32_t)(sum % rd->out_rate);
    a->fh = (a->fh + k) % GC_AID_FIFO;
    a->fn -= k;
    unsigned rh = a->rh, rn = a->rn;
    for (uint64_t f = 0; f < n; f++) {
        if (rn == GC_OUT_RING) {
            rh = (rh + 1) % GC_OUT_RING;
            rn--;
            a->overruns++;
        }
        unsigned i = (rh + rn) % GC_OUT_RING;
        a->ring[i][0] = a->ring[i][1] = 0;
        rn++;
    }
    a->rh = rh, a->rn = rn;
    a->out_done = upto;
    return true;
}

static void produce(gc_machine *m, uint64_t upto)
{
    gc_audio *a = m->audio;
    if (a->out_done >= upto)
        return;
    /* The drive's stream off (or no drive) and its history silent: every frame it would fetch is silence, so its
     * output is and only its phase moves, by in_rate a frame less out_rate while it is out_rate or more -- for the
     * whole span at once: (phase + frames * in_rate) % out_rate (XBA-279). A live stream or a history still
     * decaying goes frame by frame, as before (so does a phase or rate from a save state that the frame by frame
     * sum would wrap). */
    gc_resampler *st = &a->rs_stream;
    if (!((a->aicr & AICR_PSTAT) && m->di) && hist_silent(st) && st->phase < st->out_rate &&
        (uint64_t)st->out_rate + st->in_rate <= UINT32_MAX && upto - a->out_done <= UINT32_MAX) {
        st->phase = (uint32_t)(((uint64_t)st->phase + (upto - a->out_done) * st->in_rate) % st->out_rate);
        if (!produce_silence(a, upto))
            produce_span(m, a, upto, true);
    } else {
        produce_span(m, a, upto, false);
    }
}

void gc_ai_init(gc_machine *m)
{
    gc_audio *a = m->audio;
    a->aicr = AICR_AISFR | AICR_AIDFR; /* streams at 48 kHz, the DMA at 32 kHz */
    a->aiit_at = UINT64_MAX;
    gc_resampler_init(&a->rs_dma, dma_rate(a), GC_AUDIO_RATE);
    gc_resampler_init(&a->rs_stream, stream_rate(a), GC_AUDIO_RATE);
    a->tick_at = m->h.now + block_cycles(a);
}

uint64_t gc_ai_next_event(gc_machine *m)
{
    gc_audio *a = m->audio;
    return a->aiit_at < a->tick_at ? a->aiit_at : a->tick_at;
}

void gc_ai_events(gc_machine *m)
{
    gc_audio *a = m->audio;
    if (m->h.now >= a->aiit_at) {
        a->aiit_at = UINT64_MAX;
        a->aicr |= AICR_INT;
        ai_pi_update(m);
    }
    while (m->h.now >= a->tick_at) {
        dma_block(m);
        a->tick_at += block_cycles(a);
        uint64_t t = a->tick_at > OUT_LATENCY ? a->tick_at - OUT_LATENCY : 0;
        produce(m, t / OUT_CYCLES);
    }
}

size_t gc_audio_pull(gc_machine *m, int16_t *lr, size_t max_frames)
{
    gc_audio *a = m->audio;
    size_t n = a->rn < max_frames ? a->rn : max_frames;
    size_t first = GC_OUT_RING - a->rh < n ? GC_OUT_RING - a->rh : n; /* up to the ring's end, then from its start */
    memcpy(lr, a->ring[a->rh], first * sizeof a->ring[0]);
    memcpy(lr + 2 * first, a->ring[0], (n - first) * sizeof a->ring[0]);
    a->rh = (unsigned)((a->rh + n) % GC_OUT_RING);
    a->rn -= (unsigned)n;
    return n;
}

size_t gc_audio_queued(const gc_machine *m)
{
    return m->audio->rn;
}
