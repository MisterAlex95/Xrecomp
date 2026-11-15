/* XMV container demuxer (XBA-70): the C port of tools/xmv/xmv_demux.py.
 *
 * An XMV file is a header (magic "xobX" at 12, version, picture size,
 * duration, audio track descriptions) followed by a chain of packets; each
 * packet says how big the next one is. A packet holds a run of WMV2 video
 * frames and, for each audio track, a run of audio blocks. Layout in
 * docs/runtime/xmv.md. */
#ifndef XMV_DEMUX_H
#define XMV_DEMUX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XMV_MAX_TRACKS 8
#define XMV_MAX_FRAMES 255      /* frames in one packet (an 8-bit count) */

#define XMV_CODEC_PCM        0x0001
#define XMV_CODEC_XBOX_ADPCM 0x0069
#define XMV_CODEC_WMAV2      0x0161

/* Audio flags: one of the three stereo pairs of a 5.1 stream. */
#define XMV_AUDIO_51_FRONT  1
#define XMV_AUDIO_51_CENTER 2
#define XMV_AUDIO_51_REAR   4

typedef struct {
    uint16_t compression;      /* XMV_CODEC_* */
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t flags;            /* XMV_AUDIO_51_* */
} XmvTrack;

typedef struct {
    uint32_t version;          /* 2 or 4 */
    uint32_t width, height;
    uint32_t duration_ms;
    uint32_t max_packet_size;
    int ntracks;
    XmvTrack track[XMV_MAX_TRACKS];
} XmvInfo;

typedef struct {
    const uint8_t *data;       /* little-endian 32-bit words (WMV2_LE_WORDS) */
    uint32_t size;
    uint32_t pts_ms;           /* presentation time from the start of the movie */
    int key;                   /* an I frame */
} XmvFrame;

typedef struct {
    int has_header;            /* the packet carries a new WMV2 sequence header */
    uint32_t header;           /* in XMV's packing (wmv2_dec_set_xmv_header) */
    int nframes;
    XmvFrame frame[XMV_MAX_FRAMES];
    const uint8_t *audio[XMV_MAX_TRACKS];
    uint32_t audio_size[XMV_MAX_TRACKS];
} XmvPacket;

/* Byte source: read returns the bytes read; seek returns 0 on success. */
typedef struct {
    size_t (*read)(void *ctx, void *buf, size_t n);
    int (*seek)(void *ctx, uint64_t offset);
    void *ctx;
} XmvIo;

typedef struct XmvDemux XmvDemux;

/* Reads the header. NULL if it is not an XMV file (or on a read error). */
XmvDemux *xmv_demux_open(const XmvIo *io);
/* The same over a memory buffer the caller keeps alive. */
XmvDemux *xmv_demux_open_memory(const void *buf, size_t size);
/* The same over a file (stdio). */
XmvDemux *xmv_demux_open_file(const char *path);
void xmv_demux_close(XmvDemux *x);

const XmvInfo *xmv_demux_info(const XmvDemux *x);

/* The next packet: 1, 0 at the end of the file, -1 for damaged data. The
 * pointers in *pkt stay valid until the next call. */
int xmv_demux_next(XmvDemux *x, XmvPacket *pkt);

/* Back to the first packet. Returns 0 on success. */
int xmv_demux_rewind(XmvDemux *x);

#ifdef __cplusplus
}
#endif

#endif
