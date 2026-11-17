/* gc_ax.c -- the AX microcode at a high level (XBA-263), the GameCube version Nintendo's AX library drives.
 *
 * Protocol: after DSP_INIT, the CPU sends 0xBABE0000 | n (a command list of n 16-bit words follows) and then the
 * list's address; the ucode runs it and answers DSP_YIELD (interrupt); the CPU then sends a task mail
 * (0xCDD10003 continue, 0xCDD10001 a new ucode follows, 0xCDD10002 reset, 0xCDD10000 resume) before the next list.
 * One list renders one 5 ms frame at 32 kHz (160 samples) into nine 32-bit buffers (main L / R / surround, aux A
 * and aux B L / R / S), the CPU's effects taking aux A / B out and back between commands, and the output command
 * writing main L / R as 16-bit right-then-left frames for the audio DMA.
 *
 * Voices are parameter blocks (PBs) in main memory, a linked list; each is processed 1 ms (32 samples) at a time
 * with its per-millisecond updates: samples read from ARAM through the accelerator (ADPCM, PCM8, PCM16, loops,
 * one-shot ends), sample-rate converted (linear, or none; the polyphase filter needs the DSP ROM's coefficients,
 * which we do not have, and falls back to linear), the volume envelope, the low-pass filter, then mixed into each
 * enabled buffer with its volume and ramp. Not modelled: the initial time delay (3D phase shift), Dolby Pro Logic II
 * surround, depop of dropped voices. Layout and semantics from the hardware's documentation (facts only). */
#include <string.h>

#include "gc_dsp_internal.h"

/* PB word offsets (the layout with the low-pass filter; the 2001 ucode has none, words 93..96 absent) */
enum {
    PB_NEXT = 0, PB_SRC_TYPE = 4, PB_COEF_SEL = 5, PB_MIXER_CTRL = 6, PB_RUNNING = 7, PB_IS_STREAM = 8,
    PB_MIXER = 9, PB_ITD = 27, PB_UPD_NUM = 34, PB_UPD_DATA = 39, PB_DPOP = 41, PB_VE_VOL = 50, PB_VE_DELTA = 51,
    PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_ADDR = 57, PB_END_ADDR = 59, PB_CUR_ADDR = 61, PB_COEFS = 63,
    PB_GAIN = 79, PB_PRED_SCALE = 80, PB_YN1 = 81, PB_YN2 = 82, PB_RATIO = 83, PB_FRAC = 85, PB_LAST = 86,
    PB_LOOP_PS = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92, PB_LPF_ON = 93, PB_LPF_YN1 = 94, PB_LPF_A0 = 95,
    PB_LPF_B0 = 96, PB_LOOP_COUNTER = 97, PB_WORDS = 122, PB_LPF_WORDS = 4,
};

enum { BUF_ML, BUF_MR, BUF_MS, BUF_AL, BUF_AR, BUF_AS, BUF_BL, BUF_BR, BUF_BS };

/* For each buffer: its volume pair in the PB's mixer, its depop word, its mix and ramp bits (our mixer control) */
static const struct {
    uint8_t mix, dpop;
    uint32_t on, ramp;
} k_buf[9] = {
    {9, 41, 0x000001, 0x000002},  {11, 44, 0x000004, 0x000008}, {23, 47, 0x000010, 0x000020},
    {13, 42, 0x000040, 0x000080}, {15, 45, 0x000100, 0x000200}, {25, 48, 0x000400, 0x000800},
    {17, 43, 0x001000, 0x002000}, {19, 46, 0x004000, 0x008000}, {21, 49, 0x010000, 0x020000},
};

static uint32_t hilo(const uint16_t *pb, int off)
{
    return (uint32_t)pb[off] << 16 | pb[off + 1];
}

static int16_t clamp16(int64_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* The PB's mixer control word as the ucode reads it, in our bits (k_buf). */
static uint32_t mixer_control(const gc_ax *ax, uint16_t mc)
{
    uint32_t r = 0;
    if (ax->old) {
        r = 0x1 | 0x4; /* main L / R always */
        if (mc & 0x0010) { /* Dolby Pro Logic II */
            if ((mc & 0x0006) == 0)
                r |= 0x1000 | 0x4000;
            if ((mc & 0x0007) == 1)
                r |= 0x40 | 0x100 | 0x400;
        } else {
            if (mc & 0x0001)
                r |= 0x40 | 0x100;
            if (mc & 0x0002)
                r |= 0x1000 | 0x4000;
            if (mc & 0x0004) {
                r |= 0x10;
                if (r & 0x40)
                    r |= 0x400;
                if (r & 0x1000)
                    r |= 0x10000;
            }
        }
        if (mc & 0x0008)
            r |= 0xAAAAAA; /* every ramp */
        return r;
    }
    if (mc & 0x0001) r |= 0x000001;
    if (mc & 0x0002) r |= 0x000004;
    if (mc & 0x0004) r |= 0x000010;
    if (mc & 0x0008) r |= 0x000002 | 0x000008 | 0x000020;
    if (mc & 0x0010) r |= 0x000040;
    if (mc & 0x0020) r |= 0x000100;
    if (mc & 0x0040) r |= 0x000080 | 0x000200;
    if (mc & 0x0080) r |= 0x000400;
    if (mc & 0x0100) r |= 0x000800;
    if (mc & 0x0200) r |= 0x001000;
    if (mc & 0x0400) r |= 0x004000;
    if (mc & 0x0800) r |= 0x002000 | 0x008000;
    if (mc & 0x1000) r |= 0x010000;
    if (mc & 0x2000) r |= 0x020000;
    return r;
}

/* ---- PBs in main memory ---- */

static void pb_read(gc_machine *m, const gc_ax *ax, uint32_t addr, uint16_t *pb)
{
    for (int i = 0; i < PB_WORDS; i++) {
        int src = i;
        if (ax->old) {
            if (i >= PB_LPF_ON && i < PB_LPF_ON + PB_LPF_WORDS) {
                pb[i] = 0;
                continue;
            }
            if (i >= PB_LPF_ON + PB_LPF_WORDS)
                src = i - PB_LPF_WORDS;
        }
        pb[i] = dsp_rd16(m, addr + 2u * (uint32_t)src);
    }
}

static void pb_write(gc_machine *m, const gc_ax *ax, uint32_t addr, const uint16_t *pb)
{
    for (int i = 0; i < PB_WORDS; i++) {
        int dst = i;
        if (ax->old) {
            if (i >= PB_LPF_ON && i < PB_LPF_ON + PB_LPF_WORDS)
                continue;
            if (i >= PB_LPF_ON + PB_LPF_WORDS)
                dst = i - PB_LPF_WORDS;
        }
        dsp_wr16(m, addr + 2u * (uint32_t)dst, pb[i]);
    }
}

/* ---- one voice, one millisecond ---- */

typedef struct voice_ctx {
    gc_accel acc;
    int16_t coefs[16];
    uint16_t *pb;
} voice_ctx;

static int16_t voice_sample(voice_ctx *v)
{
    int16_t s = gc_accel_read(&v->acc);
    if (v->acc.ended) { /* the end exception: loop, or the voice stops */
        v->acc.ended = 0;
        uint16_t *pb = v->pb;
        if (pb[PB_LOOPING]) {
            v->acc.pred_scale = pb[PB_LOOP_PS] & 0x7F;
            if (pb[PB_IS_STREAM] != 1) {
                v->acc.yn1 = (int16_t)pb[PB_LOOP_YN1];
                gc_accel_set_yn2(&v->acc, (int16_t)pb[PB_LOOP_YN2]);
            } else {
                gc_accel_set_yn2(&v->acc, v->acc.yn2);
                pb[PB_LOOP_COUNTER]++;
            }
        } else {
            pb[PB_RUNNING] = 0;
        }
    }
    return s;
}

/* count samples at the PB's ratio (16.16): linear between the two oldest of the last four input samples, as the
 * ucode does; none (type 2 and up): the input as is. */
static void voice_resample(voice_ctx *v, int16_t *out, int count)
{
    uint16_t *pb = v->pb;
    int32_t hist[4];
    for (int k = 0; k < 4; k++)
        hist[k] = (int16_t)pb[PB_LAST + k];
    unsigned type = pb[PB_SRC_TYPE];
    if (type <= 1) {
        uint32_t ratio = hilo(pb, PB_RATIO), pos = pb[PB_FRAC];
        for (int i = 0; i < count; i++) {
            pos += ratio;
            while (pos >= 0x10000) {
                hist[0] = hist[1];
                hist[1] = hist[2];
                hist[2] = hist[3];
                hist[3] = voice_sample(v);
                pos -= 0x10000;
            }
            uint32_t f = pos & 0xFFFF;
            out[i] = f ? (int16_t)(((int64_t)hist[0] * (0x10000 - f) + (int64_t)hist[1] * f) >> 16) : (int16_t)hist[0];
        }
        pb[PB_FRAC] = (uint16_t)pos;
    } else {
        for (int i = 0; i < count; i++)
            out[i] = voice_sample(v);
        for (int k = 0; k < 4; k++)
            hist[k] = out[count - 4 + k];
    }
    for (int k = 0; k < 4; k++)
        pb[PB_LAST + k] = (uint16_t)hist[k];
}

static void process_voice(gc_machine *m, gc_ax *ax, uint16_t *pb, int ms)
{
    if (pb[PB_RUNNING] != 1)
        return;
    voice_ctx v;
    memset(&v, 0, sizeof v);
    v.pb = pb;
    for (int k = 0; k < 16; k++)
        v.coefs[k] = (int16_t)pb[PB_COEFS + k];
    v.acc.mem = m->aram;
    v.acc.mem_mask = GC_ARAM_SIZE - 1;
    v.acc.start = hilo(pb, PB_LOOP_ADDR) & 0x3FFFFFFFu;
    v.acc.end = hilo(pb, PB_END_ADDR) & 0x3FFFFFFFu;
    v.acc.cur = hilo(pb, PB_CUR_ADDR) & 0xBFFFFFFFu;
    v.acc.format = pb[PB_FORMAT];
    v.acc.gain = (int16_t)pb[PB_GAIN];
    v.acc.yn1 = (int16_t)pb[PB_YN1];
    gc_accel_set_yn2(&v.acc, (int16_t)pb[PB_YN2]);
    v.acc.pred_scale = pb[PB_PRED_SCALE] & 0x7F;
    v.acc.coefs = v.coefs;

    int16_t s[32];
    voice_resample(&v, s, 32);
    pb[PB_CUR_ADDR] = (uint16_t)(v.acc.cur >> 16);
    pb[PB_CUR_ADDR + 1] = (uint16_t)v.acc.cur;
    pb[PB_YN1] = (uint16_t)v.acc.yn1;
    pb[PB_YN2] = (uint16_t)v.acc.yn2;
    pb[PB_PRED_SCALE] = v.acc.pred_scale;

    for (int i = 0; i < 32; i++) { /* the volume envelope (signed on the GameCube) */
        s[i] = clamp16(((int32_t)s[i] * (int16_t)pb[PB_VE_VOL]) >> 15);
        pb[PB_VE_VOL] = (uint16_t)(pb[PB_VE_VOL] + pb[PB_VE_DELTA]);
    }
    if (!ax->old && pb[PB_LPF_ON]) { /* one-pole low-pass */
        int32_t a0 = pb[PB_LPF_A0], b0 = (int16_t)pb[PB_LPF_B0];
        for (int i = 0; i < 32; i++) {
            int16_t y = clamp16(((int64_t)a0 * s[i] + (int64_t)b0 * (int16_t)pb[PB_LPF_YN1]) >> 15);
            s[i] = y;
            pb[PB_LPF_YN1] = (uint16_t)y;
        }
    }
    uint32_t mc = mixer_control(ax, pb[PB_MIXER_CTRL]);
    for (int b = 0; b < 9; b++) {
        if (!(mc & k_buf[b].on))
            continue;
        int32_t *out = ax->buf[b] + 32 * ms;
        uint16_t vol = pb[k_buf[b].mix], delta = (mc & k_buf[b].ramp) ? pb[k_buf[b].mix + 1] : 0;
        int16_t last = 0;
        for (int i = 0; i < 32; i++) {
            last = clamp16(((int64_t)s[i] * vol) >> 15);
            out[i] += last;
            vol = (uint16_t)(vol + delta);
        }
        pb[k_buf[b].mix] = vol;
        pb[k_buf[b].dpop] = (uint16_t)last;
    }
}

static void process_pbs(gc_machine *m, gc_ax *ax, uint32_t addr)
{
    uint16_t pb[PB_WORDS];
    for (int guard = 0; addr && guard < 4096; guard++) {
        pb_read(m, ax, addr, pb);
        uint16_t upd[64];
        uint32_t ua = hilo(pb, PB_UPD_DATA);
        for (int i = 0; i < 64; i++)
            upd[i] = dsp_rd16(m, ua + 2u * (uint32_t)i);
        unsigned start = 0;
        for (int ms = 0; ms < 5; ms++) {
            unsigned n = pb[PB_UPD_NUM + ms];
            if (start < 32 && n <= 32 - start) { /* (word offset in the ucode's layout, value) pairs */
                for (unsigned i = start; i < start + n; i++) {
                    unsigned off = upd[2 * i];
                    if (ax->old && off >= PB_LPF_ON)
                        off += PB_LPF_WORDS;
                    if (off < PB_WORDS)
                        pb[off] = upd[2 * i + 1];
                }
            }
            start += pb[PB_UPD_NUM + ms];
            process_voice(m, ax, pb, ms);
        }
        pb_write(m, ax, addr, pb);
        addr = hilo(pb, PB_NEXT);
    }
}

/* ---- buffer commands ---- */

static void put_buf(gc_machine *m, uint32_t addr, const int32_t *b)
{
    for (int i = 0; i < GC_AX_SAMPLES; i++)
        dsp_wr32(m, addr + 4u * (uint32_t)i, (uint32_t)b[i]);
}

static void add_buf(gc_machine *m, uint32_t addr, int32_t *b)
{
    for (int i = 0; i < GC_AX_SAMPLES; i++)
        b[i] = (int32_t)((uint32_t)b[i] + dsp_rd32(m, addr + 4u * (uint32_t)i)); /* guest words: wraps, no UB */
}

static void cmd_setup(gc_machine *m, gc_ax *ax, uint32_t addr)
{
    for (int b = 0; b < 9; b++) {
        int32_t v = (int32_t)dsp_rd32(m, addr + 6u * (uint32_t)b);
        int16_t d = (int16_t)dsp_rd16(m, addr + 6u * (uint32_t)b + 4);
        for (int i = 0; i < GC_AX_SAMPLES; i++)
            ax->buf[b][i] = v ? (int32_t)((uint32_t)v + (uint32_t)(i * d)) : 0; /* guest values: wraps, no UB */
    }
}

static void cmd_dl_vol_mix(gc_machine *m, gc_ax *ax, uint32_t addr, const uint16_t vol[3])
{
    for (int g = 0; g < 3; g++) /* the same 3 x 160 samples for main, aux A, aux B */
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < GC_AX_SAMPLES; i++) {
                int64_t s = (int32_t)dsp_rd32(m, addr + 4u * (uint32_t)(c * GC_AX_SAMPLES + i));
                ax->buf[3 * g + c][i] = (int32_t)((uint32_t)ax->buf[3 * g + c][i] + (uint32_t)((s * vol[g]) >> 15));
            }
}

static void cmd_mix_aux(gc_machine *m, gc_ax *ax, int aux, uint32_t up, uint32_t down)
{
    int base = aux ? BUF_BL : BUF_AL;
    if (up)
        for (int c = 0; c < 3; c++)
            put_buf(m, up + 4u * GC_AX_SAMPLES * (uint32_t)c, ax->buf[base + c]);
    for (int c = 0; c < 3; c++)
        add_buf(m, down + 4u * GC_AX_SAMPLES * (uint32_t)c, ax->buf[BUF_ML + c]);
}

static void cmd_compressor(gc_machine *m, gc_ax *ax, uint16_t threshold, uint16_t frames, uint32_t table)
{
    bool hit = false;
    for (int i = 0; i < GC_AX_SAMPLES && !hit; i++)
        hit = ax->buf[BUF_ML][i] > threshold || ax->buf[BUF_ML][i] < -(int32_t)threshold ||
              ax->buf[BUF_MR][i] > threshold || ax->buf[BUF_MR][i] < -(int32_t)threshold;
    uint32_t frame_bytes = GC_AX_SAMPLES * 2, off;
    if (hit) { /* one attack ramp from where the release was, then the release starts over */
        off = ax->compressor_pos * frame_bytes;
        ax->compressor_pos = frames;
    } else if (ax->compressor_pos) {
        ax->compressor_pos--;
        off = (11u + ax->compressor_pos) * frame_bytes; /* the release ramps follow the 11 attack ones */
    } else {
        return;
    }
    for (int i = 0; i < GC_AX_SAMPLES; i++) {
        uint16_t k = dsp_rd16(m, table + off + 2u * (uint32_t)i);
        ax->buf[BUF_ML][i] = (int32_t)(((int64_t)ax->buf[BUF_ML][i] * k) >> 15);
        ax->buf[BUF_MR][i] = (int32_t)(((int64_t)ax->buf[BUF_MR][i] * k) >> 15);
    }
}

static void cmd_output(gc_machine *m, gc_ax *ax, uint32_t lr, uint32_t surround)
{
    put_buf(m, surround, ax->buf[BUF_MS]);
    for (int i = 0; i < GC_AX_SAMPLES; i++) { /* right, then left */
        dsp_wr16(m, lr + 4u * (uint32_t)i, (uint16_t)clamp16(ax->buf[BUF_MR][i]));
        dsp_wr16(m, lr + 4u * (uint32_t)i + 2, (uint16_t)clamp16(ax->buf[BUF_ML][i]));
    }
}

static void copy_list(gc_machine *m, gc_ax *ax, uint32_t addr, uint16_t size)
{
    if (size > GC_AX_CMDLIST_MAX)
        size = GC_AX_CMDLIST_MAX;
    for (unsigned i = 0; i < size; i++)
        ax->cmd[i] = dsp_rd16(m, addr + 2u * i);
    for (unsigned i = size; i < GC_AX_CMDLIST_MAX; i++)
        ax->cmd[i] = 0x000F; /* past the end: CMD_END */
    ax->cmd_size = size;
}

static void run_list(gc_machine *m, gc_ax *ax)
{
    unsigned i = 0;
    uint32_t pb_addr = 0;
#define ARG() (i < GC_AX_CMDLIST_MAX ? ax->cmd[i++] : 0)
#define ARG32() (i += 2, i <= GC_AX_CMDLIST_MAX ? (uint32_t)ax->cmd[i - 2] << 16 | ax->cmd[i - 1] : 0)
    for (int guard = 0; guard < 4096; guard++) {
        if (i >= GC_AX_CMDLIST_MAX)
            return;
        uint16_t cmd = ax->cmd[i++];
        switch (cmd) {
        case 0x00: cmd_setup(m, ax, ARG32()); break;
        case 0x01: {
            uint32_t a = ARG32();
            uint16_t vol[3];
            vol[0] = ARG();
            vol[1] = ARG();
            vol[2] = ARG();
            cmd_dl_vol_mix(m, ax, a, vol);
            break;
        }
        case 0x02: pb_addr = ARG32(); break;
        case 0x03: process_pbs(m, ax, pb_addr); break;
        case 0x04:
        case 0x05: {
            uint32_t up = ARG32(), down = ARG32();
            cmd_mix_aux(m, ax, cmd - 0x04, up, down);
            break;
        }
        case 0x06: /* upload main L R S */
        {
            uint32_t a = ARG32();
            for (int c = 0; c < 3; c++)
                put_buf(m, a + 4u * GC_AX_SAMPLES * (uint32_t)c, ax->buf[BUF_ML + c]);
            break;
        }
        case 0x07: { /* main L = R = the samples there, S = 0 */
            uint32_t a = ARG32();
            for (int k = 0; k < GC_AX_SAMPLES; k++) {
                int32_t s = (int32_t)dsp_rd32(m, a + 4u * (uint32_t)k);
                ax->buf[BUF_ML][k] = ax->buf[BUF_MR][k] = s;
                ax->buf[BUF_MS][k] = 0;
            }
            break;
        }
        case 0x08: i += 10; break; /* unknown, never seen used */
        case 0x09: cmd_mix_aux(m, ax, 1, 0, ARG32()); break;
        case 0x0A:
        case 0x0B:
        case 0x0C: break;
        case 0x0D: { /* more: the list goes on elsewhere */
            uint32_t a = ARG32();
            uint16_t n = ARG();
            copy_list(m, ax, a, n);
            i = 0;
            break;
        }
        case 0x0E: {
            uint32_t surround = ARG32(), lr = ARG32();
            cmd_output(m, ax, lr, surround);
            break;
        }
        case 0x0F: return;
        case 0x10: { /* aux B L / R up, back down into main L / R and aux B L / R */
            uint32_t up = ARG32(), down = ARG32();
            put_buf(m, up, ax->buf[BUF_BL]);
            put_buf(m, up + 4u * GC_AX_SAMPLES, ax->buf[BUF_BR]);
            for (int c = 0; c < 2; c++)
                for (int k = 0; k < GC_AX_SAMPLES; k++) {
                    int32_t s = (int32_t)dsp_rd32(m, down + 4u * (uint32_t)(c * GC_AX_SAMPLES + k));
                    ax->buf[BUF_BL + c][k] = s;
                    ax->buf[BUF_ML + c][k] += s;
                }
            break;
        }
        case 0x11: { /* main L = -x, R = x, S = 0 */
            uint32_t a = ARG32();
            for (int k = 0; k < GC_AX_SAMPLES; k++) {
                int32_t s = (int32_t)dsp_rd32(m, a + 4u * (uint32_t)k);
                ax->buf[BUF_ML][k] = -s;
                ax->buf[BUF_MR][k] = s;
                ax->buf[BUF_MS][k] = 0;
            }
            break;
        }
        case 0x12: {
            uint16_t threshold = ARG(), frames = ARG();
            uint32_t table = ARG32();
            if (!ax->old)
                cmd_compressor(m, ax, threshold, frames, table);
            break;
        }
        case 0x13: { /* aux A L R S and aux B S up; main L, main R, aux B L, aux B R mixed from four buffers */
            uint32_t a_up = ARG32(), bs_up = ARG32(), dl[4];
            for (int k = 0; k < 4; k++)
                dl[k] = ARG32();
            for (int c = 0; c < 3; c++)
                put_buf(m, a_up + 4u * GC_AX_SAMPLES * (uint32_t)c, ax->buf[BUF_AL + c]);
            put_buf(m, bs_up, ax->buf[BUF_BS]);
            static const int dst[4] = {BUF_ML, BUF_MR, BUF_BL, BUF_BR};
            for (int k = 0; k < 4; k++)
                add_buf(m, dl[k], ax->buf[dst[k]]);
            break;
        }
        default:
            if (!ax->warned_cmd) {
                fprintf(stderr, "gc: ax: unknown command %04X in a command list, list ended\n", cmd);
                ax->warned_cmd = true;
            }
            return;
        }
    }
#undef ARG
#undef ARG32
}

/* ---- mail ---- */

void gc_ax_init(gc_machine *m, bool old)
{
    gc_ax *ax = &m->audio->ax;
    memset(ax, 0, sizeof *ax);
    ax->old = old;
}

void gc_ax_mail(gc_machine *m, uint32_t mail)
{
    gc_audio *a = m->audio;
    gc_ax *ax = &a->ax;
    if ((mail & 0xFFFF0000u) == 0xCDD10000u && ax->state != 1)
        ax->state = 2; /* a task mail (the ucode does not insist on the order) */
    switch (ax->state) {
    case 0:
        if ((mail & 0xFFFF0000u) == 0xBABE0000u) {
            ax->cmd_size = (uint16_t)mail;
            ax->state = 1;
        } else if (m->verbose) {
            fprintf(stderr, "gc: ax: mail %08X while waiting for a command list\n", mail);
        }
        break;
    case 1:
        copy_list(m, ax, mail, ax->cmd_size);
        run_list(m, ax);
        a->cmd_lists++;
        ax->state = 2;
        gc_dsp_push_mail(m, DSP_MAIL_YIELD, true, 2500); /* an empty list's time; the CPU expects a delay */
        break;
    default:
        switch (0xCDD10000u | (mail & 0xFFFF)) { /* the ucode looks at the low half only */
        case CPU_MAIL_RESUME:
            gc_dsp_push_mail(m, DSP_MAIL_RESUME, true, 0);
            ax->state = 0;
            break;
        case CPU_MAIL_NEW_UCODE:
            a->uploading = true;
            a->up_step = 0;
            ax->state = 0;
            break;
        case CPU_MAIL_RESET:
            gc_dsp_set_ucode(m, UC_ROM, 0);
            break;
        case CPU_MAIL_CONTINUE:
            ax->state = 0;
            break;
        default:
            if (m->verbose)
                fprintf(stderr, "gc: ax: unknown task mail %08X\n", mail);
            break;
        }
        break;
    }
}
