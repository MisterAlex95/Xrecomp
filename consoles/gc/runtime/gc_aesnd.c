/* gc_aesnd.c -- libogc's AESND microcode at a high level (XBA-266), so homebrew built with libaesnd has sound.
 *
 * libaesnd (CPU side, zlib licence) streams each voice's samples into ARAM and has the ucode mix one voice at a time
 * into a 2 ms buffer (96 stereo frames at the DSP's rate, ~48 kHz), mails prefixed 0xFACE: 0x0080 then the address of
 * the voice's parameter block (no answer), 0x0010 mix the first voice over silence, 0x0020 mix the next one (each
 * answered DSP_SYNC with an interrupt once the block is written back with its "finished" flag), 0x0100 write the
 * buffer to the block's out_buf (DSP_SYNC), 0xDEAD terminate (DSP_DONE; the releases before 2022 never answer it on
 * hardware: we answer anyway, as a hang helps nobody). A voice is read through the DSP accelerator from ARAM (PCM8
 * or PCM16, mono or stereo, signed or unsigned since 2020), stepped by a 16.16 frequency against the output rate with
 * the last sample held in between, scaled by its 0..255 volumes, after a delay in output frames. The releases are
 * told apart by hash: the input channel order swapped in 2012, the flag bits moved in 2020 (and in EDuke32's fork).
 * Written from the ucode's documented behaviour (Dolphin's notes and source, libogc's sources: facts only). */
#include <string.h>

#include "gc_dsp_internal.h"

/* parameter block, byte offsets */
enum {
    PB_OUT_BUF = 0, PB_START = 4, PB_END = 8, PB_CURR = 12, PB_YN1 = 16, PB_YN2 = 18, PB_PDS = 20, PB_FREQ = 22,
    PB_COUNTER = 26, PB_LEFT = 28, PB_RIGHT = 30, PB_VOL_L = 32, PB_VOL_R = 34, PB_DELAY = 36, PB_FLAGS = 40,
};

#define VOICE_FINISHED 0x00100000u
#define VOICE_RUNNING 0x40000000u

static int16_t sat16(int32_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

void gc_aesnd_init(gc_machine *m, uint32_t crc)
{
    gc_aesnd *s = &m->audio->aesnd;
    memset(s, 0, sizeof *s);
    /* 2012, EDuke32, 2020, 2020 padded, 2022 padded, 2023: left first; the 2020 flag layout from EDuke32 on */
    s->swap_lr = crc == 0x078066ABu || crc == 0x5AD4D933u || crc == 0x84C680A9u || crc == 0xA02A6131u ||
                 crc == 0x2E5E4100u || crc == 0x002E5E41u;
    s->new_flags = crc == 0x5AD4D933u || crc == 0x84C680A9u || crc == 0xA02A6131u || crc == 0x2E5E4100u ||
                   crc == 0x002E5E41u;
    s->fixed_terminate = crc == 0x2E5E4100u || crc == 0x002E5E41u;
}

static void mix_voice(gc_machine *m, gc_aesnd *s)
{
    uint32_t pb = s->pb_addr;
    uint32_t flags = dsp_rd32(m, pb + PB_FLAGS);
    s->out_buf = dsp_rd32(m, pb + PB_OUT_BUF);
    bool paused = flags & (s->new_flags ? 0x8u : 0x4u);
    if (!paused && (flags & VOICE_RUNNING) && dsp_rd32(m, pb + PB_START)) {
        unsigned fmt = flags & (s->new_flags ? 7u : 3u);
        bool wide = fmt & 2, stereo = fmt & 1, unsign = fmt & 4;
        gc_accel acc;
        memset(&acc, 0, sizeof acc);
        acc.mem = m->aram;
        acc.mem_mask = GC_ARAM_SIZE - 1;
        acc.format = wide ? 0x000A : 0x0019; /* PCM16 at gain / 2048, PCM8 at gain x 1 */
        acc.gain = wide ? 0x0800 : 0x0100;
        acc.start = dsp_rd32(m, pb + PB_START);
        acc.end = dsp_rd32(m, pb + PB_END);
        acc.cur = dsp_rd32(m, pb + PB_CURR);
        acc.yn1 = (int16_t)dsp_rd16(m, pb + PB_YN1);
        gc_accel_set_yn2(&acc, (int16_t)dsp_rd16(m, pb + PB_YN2));
        acc.pred_scale = dsp_rd16(m, pb + PB_PDS);
        acc.coefs = NULL; /* the ucode zeroes them: no prediction */
        uint32_t freq = dsp_rd32(m, pb + PB_FREQ), delay = dsp_rd32(m, pb + PB_DELAY);
        uint16_t counter = dsp_rd16(m, pb + PB_COUNTER);
        uint16_t vl = dsp_rd16(m, pb + PB_VOL_L), vr = dsp_rd16(m, pb + PB_VOL_R);
        int16_t left = (int16_t)dsp_rd16(m, pb + PB_LEFT), right = (int16_t)dsp_rd16(m, pb + PB_RIGHT);
        unsigned at = 0, todo = GC_AESND_FRAMES;
        if (delay) { /* frames of the buffer skipped while the delay runs out (the ucode counts one less) */
            while (todo) {
                todo--;
                if (--delay == 0)
                    break;
                at++;
            }
            dsp_wr32(m, pb + PB_DELAY, delay);
        }
        while (todo--) {
            s->out[2 * at] = sat16(s->out[2 * at] + right);
            s->out[2 * at + 1] = sat16(s->out[2 * at + 1] + left);
            at++;
            uint32_t c = counter + freq;
            counter = (uint16_t)c;
            unsigned steps = c >> 16;
            if (!steps)
                continue;
            int16_t first = 0, second = 0;
            while (steps--) {
                first = gc_accel_read(&acc);
                if (acc.ended) { /* the end exception only resets the decoder: reads go on from the start */
                    acc.ended = 0;
                    gc_accel_set_yn2(&acc, acc.yn2);
                }
                second = first;
                if (stereo) {
                    second = gc_accel_read(&acc);
                    if (acc.ended) {
                        acc.ended = 0;
                        gc_accel_set_yn2(&acc, acc.yn2);
                    }
                }
            }
            if (unsign) {
                first = (int16_t)(first ^ 0x8000);
                second = (int16_t)(second ^ 0x8000);
            }
            int16_t r = first, l = second; /* right came first before 2012 */
            if (s->swap_lr) {
                r = second;
                l = first;
            }
            left = sat16(((int32_t)l * vl) >> 8);
            right = sat16(((int32_t)r * vr) >> 8);
        }
        dsp_wr16(m, pb + PB_COUNTER, counter);
        dsp_wr16(m, pb + PB_LEFT, (uint16_t)left);
        dsp_wr16(m, pb + PB_RIGHT, (uint16_t)right);
        dsp_wr16(m, pb + PB_PDS, acc.pred_scale);
        dsp_wr16(m, pb + PB_YN2, (uint16_t)acc.yn2);
        dsp_wr16(m, pb + PB_YN1, (uint16_t)acc.yn1);
        dsp_wr32(m, pb + PB_CURR, acc.cur);
    }
    dsp_wr32(m, pb + PB_FLAGS, flags | VOICE_FINISHED);
    m->audio->cmd_lists++;
    gc_dsp_push_mail(m, DSP_MAIL_SYNC, true, 0);
}

void gc_aesnd_mail(gc_machine *m, uint32_t mail)
{
    gc_audio *a = m->audio;
    gc_aesnd *s = &a->aesnd;
    if (s->next_is_pb) {
        s->pb_addr = mail;
        s->next_is_pb = false;
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
    switch (mail) {
    case 0xFACE0010u:
        memset(s->out, 0, sizeof s->out);
        mix_voice(m, s);
        break;
    case 0xFACE0020u:
        mix_voice(m, s);
        break;
    case 0xFACE0080u:
        s->next_is_pb = true;
        break;
    case 0xFACE0100u:
        for (int i = 0; i < GC_AESND_FRAMES * 2; i++)
            dsp_wr16(m, s->out_buf + 2u * (uint32_t)i, (uint16_t)s->out[i]);
        gc_dsp_push_mail(m, DSP_MAIL_SYNC, true, 0);
        break;
    case 0xFACEDEADu:
        if (!s->fixed_terminate && m->verbose)
            fprintf(stderr, "gc: aesnd: terminate answered (this release never answers it on hardware)\n");
        gc_dsp_push_mail(m, DSP_MAIL_DONE, true, 0);
        break;
    default:
        if (m->verbose)
            fprintf(stderr, "gc: aesnd: unknown mail %08X\n", mail);
        break;
    }
}
