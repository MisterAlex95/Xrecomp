/* Xbox ADPCM decoder (XBA-70). See xbox_adpcm.h. */
#include "xbox_adpcm.h"

/* The IMA ADPCM step sizes and index adjustments (IMA Digital Audio Focus
 * and Technical Working Groups, "Recommended Practices for Enhancing Digital
 * Audio Compatibility in Multimedia Systems", 1992). */
static const int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t index_adjust[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static inline int16_t expand(int *pred, int *index, int nib)
{
    int step = step_table[*index], diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    if (nib & 8) diff = -diff;
    *pred += diff;
    if (*pred > 32767) *pred = 32767;
    else if (*pred < -32768) *pred = -32768;
    *index += index_adjust[nib & 7];
    if (*index < 0) *index = 0;
    else if (*index > 88) *index = 88;
    return (int16_t)*pred;
}

int xbox_adpcm_decode_block(int16_t *out, const uint8_t *in, int channels)
{
    int pred[8], index[8], ch, chunk, k;
    if (channels < 1 || channels > 8)
        return 0;
    for (ch = 0; ch < channels; ch++) {
        const uint8_t *h = in + 4 * ch;
        pred[ch] = (int16_t)(h[0] | (h[1] << 8));
        /* The step index byte is signed and clamped to 0..88, the reserved
         * byte ignored, as the APU's decoder does. */
        index[ch] = (int8_t)h[2];
        index[ch] = index[ch] < 0 ? 0 : index[ch] > 88 ? 88 : index[ch];
        out[ch] = (int16_t)pred[ch];
    }
    in += 4 * channels;
    /* 8 chunks of 4 bytes (8 samples) per channel, channels interleaved. */
    for (chunk = 0; chunk < 8; chunk++) {
        for (ch = 0; ch < channels; ch++) {
            const uint8_t *p = in + (chunk * channels + ch) * 4;
            int16_t *o = out + (1 + chunk * 8) * channels + ch;
            for (k = 0; k < 4; k++) {
                o[(2 * k) * channels] = expand(&pred[ch], &index[ch], p[k] & 15);
                o[(2 * k + 1) * channels] = expand(&pred[ch], &index[ch], p[k] >> 4);
            }
        }
    }
    return XBOX_ADPCM_BLOCK_SAMPLES;
}

size_t xbox_adpcm_decode(int16_t *out, const uint8_t *in, size_t size, int channels)
{
    size_t blk, n, frames = 0;
    if (channels < 1 || channels > 8)
        return 0;
    blk = (size_t)XBOX_ADPCM_BLOCK_BYTES * (size_t)channels;
    for (n = 0; n + blk <= size; n += blk) {
        xbox_adpcm_decode_block(out + frames * (size_t)channels, in + n, channels);
        frames += XBOX_ADPCM_BLOCK_SAMPLES;
    }
    return frames;
}
