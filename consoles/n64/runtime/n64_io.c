/* VI and AI (N-4, XBA-172).
 *
 * VI: the registers, a line counter timed from H_SYNC (VI clocks per line) and V_SYNC (half-lines per frame), the
 * VI interrupt when the counter reaches V_INTR, the field bit for interlaced modes, and the picture: the framebuffer
 * at VI_ORIGIN (16-bit RGBA 5551 or 32-bit RGBA 8888, VI_WIDTH pixels per line) cropped to the visible area that
 * H_VIDEO / V_VIDEO and X_SCALE / Y_SCALE give, at framebuffer resolution. Not modelled: the anti-aliasing and
 * divot filters, the dither filter, gamma, the scaler's interpolation (nearest framebuffer pixel), the black borders
 * around the visible area.
 *
 * AI: the double-buffered DMA from RDRAM (two buffers queued, the AI interrupt when one starts playing), the DAC rate
 * (VI clock / (DACRATE + 1)), 16-bit big-endian stereo samples, resampled (sample and hold) to 48 kHz into a ring
 * that n64_audio_pull empties. */
#include "n64_internal.h"
#include "n64_rdp.h"

#include <string.h>

static uint32_t vi_clock(const n64_machine *m)
{
    return m->pal ? 49656530u : 48681812u;
}

/* ---- VI -------------------------------------------------------------------------------------------------------- */

static void vi_timing(n64_machine *m)
{
    uint32_t hsync = m->vi[7] & 0xFFF, vsync = m->vi[6] & 0x3FF;
    if (hsync == 0)
        hsync = m->pal ? 3177 : 3093;
    if (vsync < 16)
        vsync = m->pal ? 625 : 525;
    m->vi_line_ticks = (uint64_t)(hsync + 1) * N64_TICK_HZ / vi_clock(m);
    m->vi_lines = (vsync + 1) / 2;
    if (m->vi_line >= m->vi_lines)
        m->vi_line = 0;
}

void n64_vi_reset(n64_machine *m)
{
    memset(m->vi, 0, sizeof m->vi);
    m->vi_line = 0;
    m->vi_field = 0;
    vi_timing(m);
    n64_schedule(m, EV_VI, m->now + m->vi_line_ticks);
}

uint32_t n64_vi_read(n64_machine *m, uint32_t reg)
{
    if (reg == 4)
        return ((m->vi_line << 1) | m->vi_field) & 0x3FF;
    return m->vi[reg];
}

void n64_vi_write(n64_machine *m, uint32_t reg, uint32_t v)
{
    switch (reg) {
    case 0: m->vi[0] = v & 0x1FBFF; break;
    case 1: m->vi[1] = v & 0x00FFFFFF; break;
    case 2: m->vi[2] = v & 0xFFF; break;
    case 3: m->vi[3] = v & 0x3FF; break;
    case 4: n64_mi_clear(m, N64_MI_VI); break;
    case 6: m->vi[6] = v & 0x3FF; vi_timing(m); break;
    case 7: m->vi[7] = v & 0x1FFFFF; vi_timing(m); break;
    default: m->vi[reg] = v; break;
    }
}

void n64_vi_event(n64_machine *m)
{
    uint32_t intr_line = (m->vi[3] & 0x3FF) >> 1;
    m->vi_line++;
    if (m->vi_line >= m->vi_lines) {
        m->vi_line = 0;
        if (m->vi[0] & 0x40)
            m->vi_field ^= 1;
        else
            m->vi_field = 0;
        m->frames++;
        if (intr_line >= m->vi_lines)
            m->frame_done = true; /* V_INTR out of range: a field is still a frame */
    }
    if (m->vi_line == intr_line) {
        n64_mi_raise(m, N64_MI_VI);
        m->frame_done = true;
    }
    n64_schedule(m, EV_VI, m->ev[EV_VI] + m->vi_line_ticks);
    n64_rdp_run(m->rdp);
}

int n64_display(n64_machine *m, uint32_t *xrgb, int max_w, int max_h, int *w, int *h)
{
    uint32_t ctrl = m->vi[0], type = ctrl & 3;
    uint32_t origin = m->vi[1] & 0xFFFFFF, width = m->vi[2] & 0xFFF;
    int hs = (int)((m->vi[9] >> 16) & 0x3FF), he = (int)(m->vi[9] & 0x3FF);
    int vs = (int)((m->vi[10] >> 16) & 0x3FF), ve = (int)(m->vi[10] & 0x3FF);
    uint32_t xs = m->vi[12] & 0xFFF, ys = m->vi[13] & 0xFFF;
    *w = 320;
    *h = m->pal ? 288 : 240;
    n64_rdp_flush(m->rdp); /* with scanline-band threads the RDP may still hold batched primitives */
    if (type < 2 || width == 0 || he <= hs || ve <= vs || xs == 0 || ys == 0)
        return -1;
    int ow = (int)(((uint32_t)(he - hs) * xs + 512) >> 10);
    int oh = (int)(((uint32_t)((ve - vs) >> 1) * ys + 512) >> 10);
    if (ow > (int)width)
        ow = (int)width;
    if (ow > max_w)
        ow = max_w;
    if (oh > max_h)
        oh = max_h;
    if (ow <= 0 || oh <= 0)
        return -1;
    *w = ow;
    *h = oh;
    uint32_t bpp = type == 3 ? 4 : 2;
    for (int y = 0; y < oh; y++) {
        uint32_t row = origin + (uint32_t)y * width * bpp;
        uint32_t *out = xrgb + (size_t)y * (size_t)ow;
        for (int x = 0; x < ow; x++) {
            uint32_t a = row + (uint32_t)x * bpp;
            if (a + bpp > m->rdram_size) {
                out[x] = 0;
                continue;
            }
            if (bpp == 2) {
                uint32_t p = n64_be16(m->rdram + a);
                uint32_t r = (p >> 11) & 31, g = (p >> 6) & 31, b = (p >> 1) & 31;
                r = (r << 3) | (r >> 2);
                g = (g << 3) | (g >> 2);
                b = (b << 3) | (b >> 2);
                out[x] = r << 16 | g << 8 | b;
            } else {
                uint32_t p = n64_be32(m->rdram + a);
                out[x] = p >> 8;
            }
        }
    }
    return 0;
}

/* ---- AI -------------------------------------------------------------------------------------------------------- */

void n64_ai_reset(n64_machine *m)
{
    m->ai_dram = m->ai_len = m->ai_control = m->ai_dacrate = m->ai_bitrate = 0;
    m->ai_count = 0;
    m->ai_pos = 0;
    m->ai_tick = m->now;
    m->ai_src_phase = 0;
    m->ai_out_phase = 0;
    m->ai_cur[0] = m->ai_cur[1] = 0;
    m->audio_rd = m->audio_wr = 0;
    m->ev[EV_AI] = EV_NEVER;
}

static double src_period(const n64_machine *m)
{
    /* ticks per sample frame */
    return (double)N64_TICK_HZ * (double)(m->ai_dacrate + 1) / (double)vi_clock(m);
}

static void ai_start(n64_machine *m)
{
    m->ai_pos = 0;
    m->ai_src_phase = 0;
    uint64_t frames = m->ai_fifo[0].len / 4;
    n64_schedule(m, EV_AI, m->now + (uint64_t)((double)frames * src_period(m)));
    n64_mi_raise(m, N64_MI_AI);
}

static void push_out(n64_machine *m)
{
    if (m->audio_wr - m->audio_rd >= N64_AUDIO_RING)
        m->audio_rd++; /* full: drop the oldest */
    uint32_t i = m->audio_wr & (N64_AUDIO_RING - 1);
    m->audio[2 * i] = m->ai_cur[0];
    m->audio[2 * i + 1] = m->ai_cur[1];
    m->audio_wr++;
}

void n64_ai_advance(n64_machine *m)
{
    if (m->now <= m->ai_tick)
        return;
    double t = (double)(m->now - m->ai_tick);
    m->ai_tick = m->now;
    const double out_period = (double)N64_TICK_HZ / N64_AUDIO_RATE;
    bool playing = m->ai_count > 0 && m->ai_dacrate > 0;
    double sp = playing ? src_period(m) : 0;
    while (t > 0) {
        double to_out = out_period - m->ai_out_phase;
        double step = to_out < t ? to_out : t;
        if (playing) {
            double to_src = sp - m->ai_src_phase;
            if (to_src < step)
                step = to_src;
        }
        if (step < 0)
            step = 0;
        t -= step;
        m->ai_out_phase += step;
        if (playing) {
            m->ai_src_phase += step;
            if (m->ai_src_phase >= sp) {
                m->ai_src_phase -= sp;
                if (m->ai_pos + 4 <= m->ai_fifo[0].len) {
                    uint32_t a = (m->ai_fifo[0].addr + m->ai_pos) & 0x00FFFFFCu;
                    if (a + 4 <= m->rdram_size) {
                        m->ai_cur[0] = (int16_t)n64_be16(m->rdram + a);
                        m->ai_cur[1] = (int16_t)n64_be16(m->rdram + a + 2);
                    }
                    m->ai_pos += 4;
                }
            }
        }
        if (m->ai_out_phase >= out_period) {
            m->ai_out_phase -= out_period;
            push_out(m);
        }
        if (step == 0 && t > 0 && !playing) { /* guard against rounding stalls */
            m->ai_out_phase = out_period;
        }
    }
}

void n64_ai_event(n64_machine *m)
{
    n64_ai_advance(m);
    m->ev[EV_AI] = EV_NEVER;
    if (m->ai_count == 0)
        return;
    m->ai_fifo[0] = m->ai_fifo[1];
    m->ai_count--;
    if (m->ai_count > 0)
        ai_start(m);
}

uint32_t n64_ai_read(n64_machine *m, uint32_t reg)
{
    n64_ai_advance(m);
    switch (reg) {
    case 1: /* remaining bytes of the buffer playing */
        if (m->ai_count == 0)
            return 0;
        return (m->ai_fifo[0].len - (m->ai_pos < m->ai_fifo[0].len ? m->ai_pos : m->ai_fifo[0].len)) & 0x3FFF8u;
    case 3: {
        uint32_t s = 0x00100000u | 0x01000000u; /* as the hardware reports */
        if (m->ai_count >= 2)
            s |= 0x80000001u;
        if (m->ai_count > 0)
            s |= 0x40000000u;
        if (m->ai_control & 1)
            s |= 0x02000000u;
        return s;
    }
    default:
        return m->ai_len;
    }
}

void n64_ai_write(n64_machine *m, uint32_t reg, uint32_t v)
{
    n64_ai_advance(m);
    switch (reg) {
    case 0: m->ai_dram = v & 0x00FFFFF8u; break;
    case 1:
        m->ai_len = v & 0x3FFF8u;
        if (m->ai_len && m->ai_count < 2) {
            m->ai_fifo[m->ai_count].addr = m->ai_dram;
            m->ai_fifo[m->ai_count].len = m->ai_len;
            m->ai_count++;
            if (m->ai_count == 1)
                ai_start(m);
        }
        break;
    case 2: m->ai_control = v & 1; break;
    case 3: n64_mi_clear(m, N64_MI_AI); break;
    case 4: m->ai_dacrate = v & 0x3FFF; break;
    case 5: m->ai_bitrate = v & 0xF; break;
    default: break;
    }
}

size_t n64_audio_pull(n64_machine *m, int16_t *stereo, size_t frames)
{
    size_t n = 0;
    while (n < frames && m->audio_rd != m->audio_wr) {
        uint32_t i = m->audio_rd & (N64_AUDIO_RING - 1);
        stereo[2 * n] = m->audio[2 * i];
        stereo[2 * n + 1] = m->audio[2 * i + 1];
        m->audio_rd++;
        n++;
    }
    return n;
}

uint32_t n64_audio_source_rate(n64_machine *m)
{
    return m->ai_dacrate ? vi_clock(m) / (m->ai_dacrate + 1) : 0;
}
