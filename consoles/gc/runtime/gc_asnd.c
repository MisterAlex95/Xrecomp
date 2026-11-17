/* gc_asnd.c -- libogc's ASND microcode at a high level (XBA-263), so homebrew built with libasnd has sound.
 *
 * libasnd (CPU side, BSD-licensed, Hermes) mixes up to 16 voices into a 4096-byte buffer (1024 stereo frames at
 * 48 kHz) per audio DMA buffer: 0x0123 then the address of its 64-byte voice structure; then per buffer 0x0111
 * (clear the output, mix the first voice), 0x0222 for each next voice, 0x0666 (write the output to the voice's
 * out_buf); each answered DSP_SYNC with an interrupt. A voice: 8 or 16-bit, mono or stereo (signed, unsigned,
 * little or big-endian since 2011), any rate stepped against 48000 (no interpolation: the last sample holds),
 * volume 0..256 per side, a delay in output frames, a second buffer queued behind the first (a ring when looping).
 * The release is told apart by the image's hash: the flag bits moved in 2011, the channel order of the input was
 * swapped until 2024. Written from the ucode's documented behaviour (facts only). */
#include <string.h>

#include "gc_dsp_internal.h"

/* voice structure words (offset / 4) */
enum {
    V_OUT_BUF, V_DELAY, V_FLAGS, V_START, V_END, V_FREQ, V_LR /* left << 16 | right */, V_COUNTER,
    V_VOL /* l << 16 | r */, V_START2, V_END2, V_VOL2, V_BACKUP, V_TICK, V_CB, V_PAD,
};

#define ASND_RATE 48000u

static bool hash_in(uint32_t crc, const uint32_t *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (list[i] == crc)
            return true;
    return false;
}

void gc_asnd_init(gc_machine *m, uint32_t crc)
{
    gc_asnd *s = &m->audio->asnd;
    static const uint32_t newer[] = {0xA81582E2u, 0xDBBEEB61u, 0xBAD876EFu, 0xFA9C576Fu, 0x614DD145u, 0x5DBF8BF1u,
                                     0x373A950Eu};
    static const uint32_t swapped[] = {0x8D69A19Bu, 0xCC2FD441u, 0xA81582E2u, 0xDBBEEB61u, 0xBAD876EFu};
    memset(s, 0, sizeof *s);
    s->new_flags = hash_in(crc, newer, sizeof newer / sizeof newer[0]);
    s->swap_lr = hash_in(crc, swapped, sizeof swapped / sizeof swapped[0]);
    s->has_terminate = crc != 0x8D69A19Bu;
}

static void voice_in(gc_machine *m, gc_asnd *s)
{
    for (int i = 0; i < 16; i++)
        s->v[i] = dsp_rd32(m, s->voice_addr + 4u * (uint32_t)i);
}

static void voice_out(gc_machine *m, gc_asnd *s)
{
    for (int i = 0; i < 16; i++)
        dsp_wr32(m, s->voice_addr + 4u * (uint32_t)i, s->v[i]);
}

static void samples_in(gc_machine *m, gc_asnd *s, uint32_t addr)
{
    for (int i = 0; i < 16; i++)
        s->in[i] = dsp_rd16(m, addr + 2u * (uint32_t)i);
}

/* The second buffer becomes the current one (and stays queued when the voice loops). */
static void change_buffer(gc_asnd *s)
{
    uint32_t *v = s->v;
    v[V_VOL] = v[V_VOL2];
    v[V_END] = v[V_END2];
    v[V_START] = v[V_START2];
    v[V_BACKUP] = v[V_START2];
    if (!(v[V_FLAGS] & (s->new_flags ? 1u << 8 : 1u << 2))) {
        v[V_START2] = 0;
        v[V_END2] = 0;
    }
}

/* The sample at the current address: first and second (the order they sit in memory) */
static void read_sample(const gc_asnd *s, unsigned fmt, uint32_t addr, int16_t *first, int16_t *second)
{
    unsigned i = (addr >> 1) & 15;
    uint16_t w = s->in[i];
    int16_t a, b;
    switch (fmt) {
    case 0: /* mono 8-bit signed */
    case 4: /* unsigned */
        a = (int16_t)(((addr & 1) ? w & 0xFF : w >> 8) << 8);
        if (fmt == 4)
            a = (int16_t)(a ^ 0x8000);
        b = a;
        break;
    case 1: a = b = (int16_t)w; break;
    case 2: /* stereo 8-bit signed */
    case 6: /* unsigned */
        if (fmt == 6)
            w ^= 0x8080;
        a = (int16_t)(w & 0xFF00);
        b = (int16_t)(w << 8);
        break;
    case 3: a = (int16_t)w; b = (int16_t)s->in[i + 1]; break;
    case 5: a = b = (int16_t)(uint16_t)(w << 8 | w >> 8); break;
    case 7: {
        uint16_t w2 = s->in[i + 1];
        a = (int16_t)(uint16_t)(w << 8 | w >> 8);
        b = (int16_t)(uint16_t)(w2 << 8 | w2 >> 8);
        break;
    }
    default: a = b = 0; break;
    }
    *first = a;
    *second = b;
}

static int16_t sat16(int32_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static void mix_voice(gc_machine *m, gc_asnd *s)
{
    uint32_t *v = s->v;
    uint32_t flags = v[V_FLAGS];
    unsigned fmt = flags & (s->new_flags ? 7u : 3u);
    uint32_t step = flags >> 16;
    if (flags & (s->new_flags ? 1u << 9 : 1u << 5)) /* paused */
        goto done;
    if (!v[V_START]) {
        change_buffer(s);
        if (!v[V_START])
            goto done;
    }
    int16_t left = (int16_t)(v[V_LR] >> 16), right = (int16_t)v[V_LR];
    unsigned off = 0, remaining = 1024;
    if (v[V_DELAY]) { /* the delay eats the buffer (and, as the ucode does, what is left of it once it ends) */
        left = right = 0;
        unsigned rd = remaining;
        for (;;) {
            off += 2;
            if (--rd == 0)
                break;
            if (--v[V_DELAY] == 0)
                break;
        }
        remaining = 0;
    }
    samples_in(m, s, v[V_START] & ~31u);
    uint16_t vl = (uint16_t)(v[V_VOL] >> 16), vr = (uint16_t)v[V_VOL];
    while (remaining > 0) {
        remaining--;
        s->out[off] = (uint16_t)sat16((int16_t)s->out[off] + right); /* right first, as the audio DMA wants */
        off++;
        s->out[off] = (uint16_t)sat16((int16_t)s->out[off] + left);
        off++;
        v[V_COUNTER] += v[V_FREQ];
        if (v[V_COUNTER] < ASND_RATE)
            continue;
        if (v[V_FREQ] <= ASND_RATE) {
            v[V_COUNTER] -= ASND_RATE;
            v[V_START] += step;
        } else {
            while (v[V_COUNTER] >= ASND_RATE) {
                v[V_COUNTER] -= ASND_RATE;
                v[V_START] += step;
                if (!(v[V_START] & 31))
                    samples_in(m, s, v[V_START]);
            }
        }
        if (v[V_START] >= v[V_END]) {
            change_buffer(s);
            vl = (uint16_t)(v[V_VOL] >> 16);
            vr = (uint16_t)v[V_VOL];
            if (!v[V_START]) {
                left = right = 0;
                continue;
            }
            samples_in(m, s, v[V_START] & ~31u);
        }
        if (((v[V_START] >> 1) & 15) == 0)
            samples_in(m, s, v[V_START]);
        int16_t a, b;
        read_sample(s, fmt, v[V_START], &a, &b);
        if (s->swap_lr) {
            int16_t t = a;
            a = b;
            b = t;
        }
        left = (int16_t)(((int32_t)a * vl) >> 8);
        right = (int16_t)(((int32_t)b * vr) >> 8);
    }
    if (!v[V_START])
        change_buffer(s);
    v[V_LR] = (uint32_t)(uint16_t)left << 16 | (uint16_t)right;
done:
    voice_out(m, s);
}

void gc_asnd_mail(gc_machine *m, uint32_t mail)
{
    gc_audio *a = m->audio;
    gc_asnd *s = &a->asnd;
    if (s->next_is_voice) {
        s->voice_addr = mail;
        s->next_is_voice = false;
        return;
    }
    if ((mail & 0xFFFF0000u) == 0xCDD10000u) {
        if (mail == CPU_MAIL_NEW_UCODE) {
            a->uploading = true;
            a->up_step = 0;
        } else if (mail == CPU_MAIL_RESET) {
            gc_dsp_set_ucode(m, UC_ROM, 0);
        }
        return;
    }
    switch (mail & 0xFFFF) {
    case 0x0111: /* first voice: the output starts from silence */
        voice_in(m, s);
        memset(s->out, 0, sizeof s->out);
        mix_voice(m, s);
        a->cmd_lists++;
        gc_dsp_push_mail(m, DSP_MAIL_SYNC, true, 0);
        break;
    case 0x0112: /* first voice over what out_buf holds */
        voice_in(m, s);
        for (int i = 0; i < 2048; i++)
            s->out[i] = dsp_rd16(m, s->v[V_OUT_BUF] + 2u * (uint32_t)i);
        mix_voice(m, s);
        gc_dsp_push_mail(m, DSP_MAIL_SYNC, true, 0);
        break;
    case 0x0123:
        s->next_is_voice = true;
        break;
    case 0x0222:
        voice_in(m, s);
        mix_voice(m, s);
        gc_dsp_push_mail(m, DSP_MAIL_SYNC, true, 0);
        break;
    case 0x0666:
        for (int i = 0; i < 2048; i++)
            dsp_wr16(m, s->v[V_OUT_BUF] + 2u * (uint32_t)i, s->out[i]);
        gc_dsp_push_mail(m, DSP_MAIL_SYNC, true, 0);
        break;
    case 0x0777: /* IROM dump word: no ROM here */
        gc_dsp_push_mail(m, 0, false, 0);
        break;
    case 0x0888: /* test */
        gc_dsp_push_mail(m, 0x22640000u, false, 0);
        break;
    case 0x0999:
        gc_dsp_push_mail(m, s->has_terminate ? DSP_MAIL_DONE : DSP_MAIL_SYNC, true, 0);
        break;
    default:
        gc_dsp_push_mail(m, DSP_MAIL_SYNC, true, 0);
        break;
    }
}
