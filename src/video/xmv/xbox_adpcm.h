/* Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM, 0x0069) to 16-bit PCM (XBA-70).
 *
 * The IMA ADPCM variant the Xbox's APU plays: blocks of 36 bytes per channel,
 * each channel's block a 4-byte header (the first sample, little-endian
 * int16, then the step index and a reserved byte) and 32 bytes of 4-bit
 * codes, low nibble first, interleaved between channels 4 bytes at a time.
 * A block decodes to 65 sample frames: the header's sample and 64 more. The
 * step arithmetic is the IMA reference's shift-and-add, as the APU's own
 * decoder (src/apu/apu_state.h adpcm_decode_block) does. */
#ifndef XMV_XBOX_ADPCM_H
#define XMV_XBOX_ADPCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_ADPCM_BLOCK_BYTES   36   /* per channel */
#define XBOX_ADPCM_BLOCK_SAMPLES 65   /* sample frames per block */

/* Decodes one block of `channels` channels (36 * channels bytes) into
 * 65 * channels interleaved samples. Returns the frames written (65), or 0
 * for a bad channel count. */
int xbox_adpcm_decode_block(int16_t *out, const uint8_t *in, int channels);

/* Decodes every whole block in `size` bytes; returns the frames written.
 * out holds (size / (36 * channels)) * 65 * channels samples. */
size_t xbox_adpcm_decode(int16_t *out, const uint8_t *in, size_t size, int channels);

#ifdef __cplusplus
}
#endif

#endif
