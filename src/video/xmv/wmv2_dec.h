/* WMV2 (Windows Media Video 8) video decoder, the codec of the Xbox's XMV
 * movies. Portable C, no dependencies (XBA-70).
 *
 * Decodes I and P frames into 8-bit YUV 4:2:0: MS-MPEG4 intra coding with DC
 * and AC prediction, the WMV2 IDCT, half-pel and "mspel" motion compensation,
 * the adaptive block transform (8x4 / 4x8) and the H.263 loop filter. I
 * frames coded with the alternative "X8" intra coder (j_type) are not
 * supported: wmv2_dec_decode returns WMV2_UNSUPPORTED for them and keeps the
 * previous picture (docs/runtime/xmv.md).
 *
 * The bitstream is read most significant bit first. In an XMV file the frame
 * is stored as little-endian 32-bit words (WMV2_LE_WORDS); in a WMV/ASF file
 * as plain bytes. */
#ifndef XMV_WMV2_DEC_H
#define XMV_WMV2_DEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Wmv2Dec Wmv2Dec;

enum {
    WMV2_OK          = 0,   /* a new picture */
    WMV2_SKIPPED     = 1,   /* a P frame that skips everything: the picture is unchanged */
    WMV2_UNSUPPORTED = 2,   /* an X8 intra frame: the picture is unchanged */
    WMV2_ERROR       = -1,  /* damaged data: what was decoded before the error is kept */
};

/* Flags for wmv2_dec_decode. */
#define WMV2_LE_WORDS 1u    /* the frame is little-endian 32-bit words (XMV) */

/* A decoder for width x height pictures (2..4096 each). NULL on failure. */
Wmv2Dec *wmv2_dec_create(int width, int height);
void wmv2_dec_destroy(Wmv2Dec *d);

/* The sequence header in XMV's packing (the 32-bit word an XMV packet carries
 * when its extradata flag is set): bit 0 mspel, 1 loop filter, 2 adaptive
 * block transform, 3 X8 intra allowed, 4 top-left MV flag, 5 per-MB RL
 * table, 6..8 slice count. */
void wmv2_dec_set_xmv_header(Wmv2Dec *d, uint32_t xmv_word);
/* The same from a WMV file's 4-byte extradata (big-endian bit fields:
 * 5 fps, 11 bit rate, then the flags above, then 3 bits of slice count). */
void wmv2_dec_set_wmv_extradata(Wmv2Dec *d, const uint8_t ext[4]);

/* Decodes one frame. size is in bytes. Returns a WMV2_* code. */
int wmv2_dec_decode(Wmv2Dec *d, const uint8_t *data, size_t size, unsigned flags);

/* The current picture: plane 0 is Y (width x height), 1 and 2 are U and V
 * ((width+1)/2 x (height+1)/2). *stride gets the row pitch in bytes. */
const uint8_t *wmv2_dec_plane(const Wmv2Dec *d, int plane, int *stride);
int wmv2_dec_width(const Wmv2Dec *d);
int wmv2_dec_height(const Wmv2Dec *d);
/* 1 if the last decoded frame was an I frame. */
int wmv2_dec_last_was_key(const Wmv2Dec *d);

#ifdef __cplusplus
}
#endif

#endif
