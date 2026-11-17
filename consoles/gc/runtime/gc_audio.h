/* gc_audio.h -- the GameCube's sound (XBA-263): what the machine plays and the pure pieces its tests check.
 *
 * The core (gc_core.h) runs the DSP interface (mailboxes, control register, ARAM DMA, the audio DMA), the audio
 * interface (sample rates, the streaming sample counter and its interrupt) and the drive's streamed audio (DTK)
 * itself; the DSP's microcode is emulated at a high level (HLE: AX, the Zelda / JAudio family, libogc's ASND and
 * AESND, the ROM / init / card ucodes), picked by a hash of the image the game uploads (gc_dsp.c). What the
 * speakers would get comes out here: 48 kHz signed 16-bit stereo, left then right, the audio DMA's samples (32 or 48 kHz) and the disc stream (32 or 48 kHz,
 * at the AI's streaming volume) resampled and mixed. See consoles/README.md « GameCube sound ». */
#ifndef GC_AUDIO_H
#define GC_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "gc_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_AUDIO_RATE 48000

/* Takes up to max frames (L, R interleaved) of what the machine played since the last pull. The core keeps about
 * 0.68 s; older frames are dropped (counted in overruns) when nobody pulls. Returns the frames written. */
size_t gc_audio_pull(gc_machine *m, int16_t *lr, size_t max_frames);
/* Frames waiting to be pulled. */
size_t gc_audio_queued(const gc_machine *m);

/* What the sound hardware did, for logs and tests. */
typedef struct gc_audio_info {
    uint32_t ucode_crc;       /* the running microcode's hash (0: the ROM) */
    const char *ucode;        /* its kind: "rom", "init", "ax", "ax-old", "asnd", "zelda", "aesnd", "card"... */
    unsigned ucodes_booted;   /* microcodes started since power-on */
    unsigned unknown_ucodes;  /* of which unknown (played as AX, logged) */
    uint64_t mails_to_dsp, mails_from_dsp;
    uint64_t cmd_lists;       /* AX command lists, ASND / AESND mix commands, Zelda frames */
    uint64_t dma_blocks;      /* 32-byte audio DMA blocks played */
    uint64_t stream_frames;   /* disc stream frames played */
    uint64_t frames_out;      /* 48 kHz frames produced */
    uint64_t overruns;        /* frames dropped because nobody pulled */
    uint64_t aram_dmas;
    uint32_t ai_control;      /* AICR */
} gc_audio_info;
void gc_audio_get_info(const gc_machine *m, gc_audio_info *out);

/* ---- Pure pieces (no machine), tested in consoles/gc/tests/gc_audio_unit.c ---------------------------------- */

/* The ucode hash: each byte xor'ed in, then the word rotated left by 3 (the hash Dolphin's ucode list is keyed by,
 * so its published values identify the same images). */
uint32_t gc_ucode_hash(const uint8_t *p, size_t n);

/* The drive's streamed audio (DTK): one 32-byte block, 28 stereo frames. Bytes 0 / 1 are the left / right
 * headers (filter << 4 | shift), bytes 4..31 the samples (low nibble left, high nibble right); hist holds the
 * decoder's history (left yn1, yn2, right yn1, yn2) across blocks. out gets 28 frames L, R. */
void gc_dtk_decode_block(const uint8_t blk[32], int16_t out[56], int32_t hist[4]);

/* A fixed-ratio resampler, stereo, 4-point cubic (Hermite) interpolation, exact over any length: after n output
 * frames it has read floor(n * in_rate / out_rate) input frames (plus its 2-frame lead), so it never drifts. */
typedef struct gc_resampler {
    uint32_t in_rate, out_rate;
    uint32_t phase;          /* position between hist[1] and hist[2], in 1 / out_rate */
    int32_t hist[4][2];      /* the last four input frames, oldest first */
} gc_resampler;
typedef void (*gc_resampler_fetch)(void *ctx, int16_t lr[2]);
void gc_resampler_init(gc_resampler *r, uint32_t in_rate, uint32_t out_rate);
/* Changes the input rate, keeping the history (a game switching the AI's rate mid-stream). */
void gc_resampler_set_rate(gc_resampler *r, uint32_t in_rate);
/* One output frame; fetch is called for each input frame it needs (0, 1 or more). */
void gc_resampler_next(gc_resampler *r, int16_t out[2], gc_resampler_fetch fetch, void *ctx);

/* The DSP accelerator's sample reader (what AX voices read ARAM through): ADPCM (8-byte frames: a header byte
 * pred << 4 | scale, then 14 4-bit samples; addresses in nibbles), PCM8 (bytes) and PCM16 (big-endian words),
 * with the loop / end exception. Addresses as the AX parameter blocks hold them. */
typedef struct gc_accel {
    const uint8_t *mem;      /* ARAM */
    uint32_t mem_mask;       /* its size - 1 */
    uint32_t start, end, cur;
    uint16_t format;         /* 0 ADPCM, 0x0A PCM16, 0x19 PCM8 (low 6 bits: size, decode, gain scale) */
    int16_t gain, yn1, yn2;
    uint16_t pred_scale;
    const int16_t *coefs;    /* 16: 8 pairs */
    int stopped;             /* the end was reached: reads give 0 until yn2 is set again */
    int ended;               /* set by gc_accel_read when it hit the end (the caller loops or stops the voice) */
} gc_accel;
int16_t gc_accel_read(gc_accel *a);
void gc_accel_set_yn2(gc_accel *a, int16_t v); /* also restarts reads after the end, as on hardware */

#ifdef __cplusplus
}
#endif

#endif
