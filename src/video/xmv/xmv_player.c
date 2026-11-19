/* XMV movie player core (XBA-70). See xmv_player.h. */
#include "xmv_player.h"
#include "wmv2_dec.h"
#include "xbox_adpcm.h"

#include <stdlib.h>
#include <string.h>

struct XmvPlayer {
    XmvDemux *dmx;
    Wmv2Dec *dec;
    XmvPacket pkt;
    int pkt_valid, pkt_frame;
    int track;                 /* audio track played, -1 none */
    int channels, rate;
    int x8_allowed;            /* the last sequence header allows X8 intra frames */
    int16_t *pcm;              /* FIFO of decoded frames */
    size_t pcm_cap, pcm_head, pcm_len;   /* in frames */
    XmvPlayerStats st;
};

static int pick_track(const XmvInfo *in)
{
    int i;
    for (i = 0; i < in->ntracks; i++) {
        const XmvTrack *t = &in->track[i];
        int playable = (t->compression == XMV_CODEC_XBOX_ADPCM && t->bits_per_sample == 4) ||
                       (t->compression == XMV_CODEC_PCM && t->bits_per_sample == 16);
        if (!playable || t->channels > 8)
            continue;
        /* 5.1 comes as three stereo tracks: the front pair stands for it. */
        if ((t->flags & (XMV_AUDIO_51_CENTER | XMV_AUDIO_51_REAR)) && !(t->flags & XMV_AUDIO_51_FRONT))
            continue;
        return i;
    }
    return -1;
}

static XmvPlayer *open_common(XmvDemux *dmx)
{
    XmvPlayer *p;
    const XmvInfo *in;
    if (!dmx)
        return NULL;
    p = (XmvPlayer *)calloc(1, sizeof *p);
    if (!p) {
        xmv_demux_close(dmx);
        return NULL;
    }
    p->dmx = dmx;
    in = xmv_demux_info(dmx);
    p->dec = wmv2_dec_create((int)in->width, (int)in->height);
    if (!p->dec) {
        xmv_player_close(p);
        return NULL;
    }
    p->track = pick_track(in);
    if (p->track >= 0) {
        p->channels = in->track[p->track].channels;
        p->rate = (int)in->track[p->track].sample_rate;
    }
    return p;
}

XmvPlayer *xmv_player_open_file(const char *path) { return open_common(xmv_demux_open_file(path)); }
XmvPlayer *xmv_player_open_io(const XmvIo *io) { return open_common(xmv_demux_open(io)); }
XmvPlayer *xmv_player_open_memory(const void *buf, size_t size)
{
    return open_common(xmv_demux_open_memory(buf, size));
}

void xmv_player_close(XmvPlayer *p)
{
    if (!p)
        return;
    wmv2_dec_destroy(p->dec);
    xmv_demux_close(p->dmx);
    free(p->pcm);
    free(p);
}

const XmvInfo *xmv_player_info(const XmvPlayer *p) { return xmv_demux_info(p->dmx); }
int xmv_player_audio_track(const XmvPlayer *p) { return p->track; }
int xmv_player_audio_rate(const XmvPlayer *p) { return p->rate; }
int xmv_player_audio_channels(const XmvPlayer *p) { return p->channels; }
size_t xmv_player_audio_available(const XmvPlayer *p) { return p->pcm_len; }
void xmv_player_stats(const XmvPlayer *p, XmvPlayerStats *s) { *s = p->st; }

/* Room for n more frames in the FIFO (compacted to the front). */
static int pcm_reserve(XmvPlayer *p, size_t n)
{
    size_t ch = (size_t)p->channels;
    if (p->pcm_head) {
        memmove(p->pcm, p->pcm + p->pcm_head * ch, p->pcm_len * ch * sizeof(int16_t));
        p->pcm_head = 0;
    }
    if (p->pcm_len + n > p->pcm_cap) {
        size_t cap = (p->pcm_len + n) * 2;
        int16_t *np = (int16_t *)realloc(p->pcm, cap * ch * sizeof(int16_t));
        if (!np)
            return -1;
        p->pcm = np;
        p->pcm_cap = cap;
    }
    return 0;
}

static void queue_audio(XmvPlayer *p)
{
    const XmvTrack *t;
    const uint8_t *a;
    size_t size, frames;
    if (p->track < 0)
        return;
    t = &xmv_demux_info(p->dmx)->track[p->track];
    a = p->pkt.audio[p->track];
    size = p->pkt.audio_size[p->track];
    if (!a || !size)
        return;
    if (t->compression == XMV_CODEC_XBOX_ADPCM) {
        frames = size / (XBOX_ADPCM_BLOCK_BYTES * (size_t)p->channels) * XBOX_ADPCM_BLOCK_SAMPLES;
        if (!frames || pcm_reserve(p, frames) < 0)
            return;
        p->pcm_len += xbox_adpcm_decode(p->pcm + p->pcm_len * (size_t)p->channels, a, size, p->channels);
    } else {
        size_t i, n;
        frames = size / (2 * (size_t)p->channels);
        if (!frames || pcm_reserve(p, frames) < 0)
            return;
        n = frames * (size_t)p->channels;
        for (i = 0; i < n; i++)
            p->pcm[p->pcm_len * (size_t)p->channels + i] = (int16_t)(a[2 * i] | (a[2 * i + 1] << 8));
        p->pcm_len += frames;
    }
}

size_t xmv_player_read_audio(XmvPlayer *p, int16_t *out, size_t max_frames)
{
    size_t n = max_frames < p->pcm_len ? max_frames : p->pcm_len;
    if (!n)
        return 0;
    memcpy(out, p->pcm + p->pcm_head * (size_t)p->channels, n * (size_t)p->channels * sizeof(int16_t));
    p->pcm_head += n;
    p->pcm_len -= n;
    if (!p->pcm_len)
        p->pcm_head = 0;
    return n;
}

/* The next frame of the packet chain: 1, 0 at the end, -1 if the chain
 * broke. audio: decode the packets' audio on the way. */
static int next_coded_frame(XmvPlayer *p, const XmvFrame **out, int audio)
{
    int r;
    while (!p->pkt_valid || p->pkt_frame >= p->pkt.nframes) {
        r = xmv_demux_next(p->dmx, &p->pkt);
        if (r <= 0) {
            p->pkt_valid = 0;
            return r;
        }
        p->pkt_valid = 1;
        p->pkt_frame = 0;
        if (p->pkt.has_header) {
            wmv2_dec_set_xmv_header(p->dec, p->pkt.header);
            p->x8_allowed = (p->pkt.header >> 3) & 1;
        }
        if (audio)
            queue_audio(p);
    }
    *out = &p->pkt.frame[p->pkt_frame++];
    return 1;
}

int xmv_player_next_frame(XmvPlayer *p, uint32_t *pts_ms)
{
    const XmvFrame *f;
    int r;
    if (next_coded_frame(p, &f, 1) <= 0)
        return 0;
    r = wmv2_dec_decode(p->dec, f->data, f->size, WMV2_LE_WORDS);
    p->st.frames++;
    if (r == WMV2_OK) p->st.decoded++;
    else if (r == WMV2_SKIPPED) p->st.skipped++;
    else if (r == WMV2_UNSUPPORTED) p->st.unsupported++;
    else p->st.errors++;
    if (pts_ms)
        *pts_ms = f->pts_ms;
    return 1;
}

const uint8_t *xmv_player_plane(const XmvPlayer *p, int plane, int *stride)
{
    return wmv2_dec_plane(p->dec, plane, stride);
}

int xmv_player_rewind(XmvPlayer *p)
{
    const XmvInfo *in = xmv_demux_info(p->dmx);
    Wmv2Dec *nd = wmv2_dec_create((int)in->width, (int)in->height);
    if (!nd)
        return -1;
    wmv2_dec_destroy(p->dec);
    p->dec = nd;
    p->pkt_valid = 0;
    p->x8_allowed = 0;
    p->pcm_head = p->pcm_len = 0;
    memset(&p->st, 0, sizeof p->st);
    return xmv_demux_rewind(p->dmx);
}

/* An X8 intra frame, from its picture header alone: an I frame (first bit
 * 0), 7 bits, the quantiser (5), then j_type when the sequence header allows
 * X8. The frame is little-endian 32-bit words of the MSB-first bitstream:
 * those 14 bits are the top of its first word. */
static int frame_is_x8(const XmvFrame *f, int x8_allowed)
{
    uint32_t w;
    if (!x8_allowed || f->size < 4)
        return 0;
    w = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) | ((uint32_t)f->data[2] << 16) |
        ((uint32_t)f->data[3] << 24);
    return !(w >> 31) && ((w >> 19) & 31) != 0 && ((w >> 18) & 1);
}

int xmv_player_probe(XmvPlayer *p, uint32_t max_frames, uint32_t decode_frames, XmvProbe *out)
{
    const XmvFrame *f;
    int r = 1;
    memset(out, 0, sizeof *out);
    while (out->frames < max_frames && (r = next_coded_frame(p, &f, 0)) == 1) {
        int x8 = frame_is_x8(f, p->x8_allowed);
        out->x8_allowed |= p->x8_allowed;
        out->x8 += (uint32_t)x8;
        if (out->frames < decode_frames) {
            int d = wmv2_dec_decode(p->dec, f->data, f->size, WMV2_LE_WORDS);
            out->decoded++;
            if (d == WMV2_ERROR)
                out->damaged++;
        }
        out->frames++;
    }
    out->broken = r < 0;
    out->complete = r == 0;
    return xmv_player_rewind(p);
}

/* ── Colour conversion ─────────────────────────────────────────────────── */

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

void xmv_yuv420_to_bgra(const uint8_t *const pl[3], const int st[3], int w, int h,
                        uint8_t *dst, int pitch)
{
    int x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *py = pl[0] + (size_t)y * st[0];
        const uint8_t *pu = pl[1] + (size_t)(y >> 1) * st[1];
        const uint8_t *pv = pl[2] + (size_t)(y >> 1) * st[2];
        uint8_t *o = dst + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            /* 16.16 fixed point: 1.164 (Y - 16), 1.596 V, 0.391 U, 0.813 V, 2.018 U */
            int c = (py[x] - 16) * 76309, d = pu[x >> 1] - 128, e = pv[x >> 1] - 128;
            o[4 * x + 0] = clamp8((c + 132201 * d + 32768) >> 16);
            o[4 * x + 1] = clamp8((c - 25675 * d - 53279 * e + 32768) >> 16);
            o[4 * x + 2] = clamp8((c + 104597 * e + 32768) >> 16);
            o[4 * x + 3] = 255;
        }
    }
}

void xmv_yuv420_to_yuy2(const uint8_t *const pl[3], const int st[3], int w, int h,
                        uint8_t *dst, int pitch)
{
    int x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *py = pl[0] + (size_t)y * st[0];
        const uint8_t *pu = pl[1] + (size_t)(y >> 1) * st[1];
        const uint8_t *pv = pl[2] + (size_t)(y >> 1) * st[2];
        uint8_t *o = dst + (size_t)y * pitch;
        for (x = 0; x + 1 < w; x += 2) {
            o[2 * x + 0] = py[x];
            o[2 * x + 1] = pu[x >> 1];
            o[2 * x + 2] = py[x + 1];
            o[2 * x + 3] = pv[x >> 1];
        }
        if (x < w) {
            o[2 * x + 0] = py[x];
            o[2 * x + 1] = pu[x >> 1];
            o[2 * x + 2] = py[x];
            o[2 * x + 3] = pv[x >> 1];
        }
    }
}
