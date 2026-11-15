/* XMV movie player core (XBA-70): demuxer + WMV2 video + Xbox ADPCM audio.
 *
 * Pull model, no threads, no clock: the caller asks for the next video frame
 * (with its presentation time) when its own clock says so, and drains the
 * audio decoded along the way. The runtime's movie hook (xmv_hle.c) and the
 * tests drive it. Portable C, no dependencies. */
#ifndef XMV_PLAYER_H
#define XMV_PLAYER_H

#include <stddef.h>
#include <stdint.h>

#include "xmv_demux.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XmvPlayer XmvPlayer;

XmvPlayer *xmv_player_open_file(const char *path);
XmvPlayer *xmv_player_open_memory(const void *buf, size_t size);
void xmv_player_close(XmvPlayer *p);

const XmvInfo *xmv_player_info(const XmvPlayer *p);

/* Decodes the next video frame. Returns 1 (a frame; *pts_ms its time), 0 at
 * the end of the movie. A frame the decoder cannot decode (an X8 intra
 * frame, damaged data) still returns 1 and shows what the decoder holds;
 * xmv_player_stats counts them. */
int xmv_player_next_frame(XmvPlayer *p, uint32_t *pts_ms);

/* The current picture (YUV 4:2:0, the movie's width x height). */
const uint8_t *xmv_player_plane(const XmvPlayer *p, int plane, int *stride);

/* The audio track played: the first Xbox ADPCM or PCM one (for a 5.1 movie,
 * the front pair). -1 if none can be played. */
int xmv_player_audio_track(const XmvPlayer *p);
int xmv_player_audio_rate(const XmvPlayer *p);
int xmv_player_audio_channels(const XmvPlayer *p);
/* Decoded sample frames waiting, and reading them (interleaved int16). */
size_t xmv_player_audio_available(const XmvPlayer *p);
size_t xmv_player_read_audio(XmvPlayer *p, int16_t *out, size_t max_frames);

typedef struct {
    uint32_t frames;          /* video frames returned */
    uint32_t decoded;         /* of them decoded */
    uint32_t unsupported;     /* X8 intra frames shown as the previous picture */
    uint32_t errors;          /* damaged frames */
    uint32_t skipped;         /* "nothing changed" P frames */
} XmvPlayerStats;
void xmv_player_stats(const XmvPlayer *p, XmvPlayerStats *s);

/* Back to the start. */
int xmv_player_rewind(XmvPlayer *p);

/* Colour conversion of a YUV 4:2:0 picture (BT.601, studio range, as the
 * Xbox's XMV player converts): to 32-bit pixels B, G, R, A=255 in memory
 * order (D3DFMT_A8R8G8B8 / X8R8G8B8 little-endian), or to YUY2 (Y0 U Y1 V). */
void xmv_yuv420_to_bgra(const uint8_t *const plane[3], const int stride[3], int w, int h,
                        uint8_t *dst, int dst_pitch);
void xmv_yuv420_to_yuy2(const uint8_t *const plane[3], const int stride[3], int w, int h,
                        uint8_t *dst, int dst_pitch);

#ifdef __cplusplus
}
#endif

#endif
