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

static uint64_t block_cycles(const gc_audio *a)
{
    return 8ull * GC_CPU_HZ / dma_rate(a);
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
    for (int i = 0; i < 8; i++) {
        uint32_t p = a->aid_cur + 4u * (uint32_t)i;
        int16_t r = (int16_t)dsp_rd16(m, p), l = (int16_t)dsp_rd16(m, p + 2);
        fifo_push(a, l, r);
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

static void fetch_dma(void *ctx, int16_t lr[2])
{
    gc_audio *a = ctx;
    if (!a->fn) {
        lr[0] = lr[1] = 0;
        return;
    }
    lr[0] = a->fifo[a->fh][0];
    lr[1] = a->fifo[a->fh][1];
    a->fh = (a->fh + 1) % GC_AID_FIFO;
    a->fn--;
}

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

static void produce(gc_machine *m, uint64_t upto)
{
    gc_audio *a = m->audio;
    while (a->out_done < upto) {
        int16_t d[2], s[2];
        gc_resampler_next(&a->rs_dma, d, fetch_dma, a);
        gc_resampler_next(&a->rs_stream, s, fetch_stream, m);
        int32_t l = d[0] + s[0], r = d[1] + s[1];
        if (a->rn == GC_OUT_RING) {
            a->rh = (a->rh + 1) % GC_OUT_RING;
            a->rn--;
            a->overruns++;
        }
        unsigned i = (a->rh + a->rn) % GC_OUT_RING;
        a->ring[i][0] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
        a->ring[i][1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
        a->rn++;
        a->out_done++;
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
    size_t n = 0;
    while (n < max_frames && a->rn) {
        lr[2 * n] = a->ring[a->rh][0];
        lr[2 * n + 1] = a->ring[a->rh][1];
        a->rh = (a->rh + 1) % GC_OUT_RING;
        a->rn--;
        n++;
    }
    return n;
}

size_t gc_audio_queued(const gc_machine *m)
{
    return m->audio->rn;
}
