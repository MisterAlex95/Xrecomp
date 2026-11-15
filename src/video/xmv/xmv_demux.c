/* XMV demuxer (XBA-70). See xmv_demux.h and docs/runtime/xmv.md. */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* fseeko */
#endif
#include "xmv_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct XmvDemux {
    XmvIo io;
    XmvInfo info;
    uint64_t first_offset;     /* the first packet */
    uint32_t first_size;
    uint64_t next_offset;
    uint32_t next_size;
    uint32_t pts;              /* running video time, ms */
    uint8_t *buf;
    size_t buf_size;
    /* Owned sources. */
    FILE *fp;
    const uint8_t *mem;
    size_t mem_size, mem_pos;
};

static uint32_t rl32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rl16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int read_full(XmvDemux *x, void *buf, size_t n)
{
    return x->io.read(x->io.ctx, buf, n) == n ? 0 : -1;
}

static XmvDemux *open_io(XmvDemux *x)
{
    uint8_t h[36 + 12 * XMV_MAX_TRACKS];
    uint32_t this_size;
    int i;
    if (read_full(x, h, 36) < 0 || memcmp(h + 12, "xobX", 4) != 0)
        goto fail;
    x->info.version = rl32(h + 16);
    if (x->info.version == 0 || x->info.version > 4)
        goto fail;
    this_size = rl32(h + 4);
    x->info.max_packet_size = rl32(h + 8);
    x->info.width = rl32(h + 20);
    x->info.height = rl32(h + 24);
    x->info.duration_ms = rl32(h + 28);
    x->info.ntracks = rl16(h + 32);
    if (x->info.ntracks > XMV_MAX_TRACKS || x->info.width == 0 || x->info.height == 0 ||
        x->info.width > 4096 || x->info.height > 4096)
        goto fail;
    if (x->info.ntracks && read_full(x, h + 36, 12 * (size_t)x->info.ntracks) < 0)
        goto fail;
    for (i = 0; i < x->info.ntracks; i++) {
        const uint8_t *t = h + 36 + 12 * i;
        XmvTrack *tr = &x->info.track[i];
        tr->compression = rl16(t);
        tr->channels = rl16(t + 2);
        tr->sample_rate = rl32(t + 4);
        tr->bits_per_sample = rl16(t + 8);
        tr->flags = rl16(t + 10);
        if (tr->channels == 0 || tr->sample_rate == 0)
            goto fail;
    }
    /* The header's "this packet" size counts the header and the first packet. */
    x->first_offset = 36 + 12 * (uint64_t)x->info.ntracks;
    if (this_size < x->first_offset)
        goto fail;
    x->first_size = this_size - (uint32_t)x->first_offset;
    x->next_offset = x->first_offset;
    x->next_size = x->first_size;
    return x;
fail:
    xmv_demux_close(x);
    return NULL;
}

XmvDemux *xmv_demux_open(const XmvIo *io)
{
    XmvDemux *x = (XmvDemux *)calloc(1, sizeof *x);
    if (!x)
        return NULL;
    x->io = *io;
    return open_io(x);
}

static size_t mem_read(void *ctx, void *buf, size_t n)
{
    XmvDemux *x = (XmvDemux *)ctx;
    size_t left = x->mem_pos < x->mem_size ? x->mem_size - x->mem_pos : 0;
    if (n > left)
        n = left;
    memcpy(buf, x->mem + x->mem_pos, n);
    x->mem_pos += n;
    return n;
}
static int mem_seek(void *ctx, uint64_t off)
{
    XmvDemux *x = (XmvDemux *)ctx;
    if (off > x->mem_size)
        return -1;
    x->mem_pos = (size_t)off;
    return 0;
}

XmvDemux *xmv_demux_open_memory(const void *buf, size_t size)
{
    XmvDemux *x = (XmvDemux *)calloc(1, sizeof *x);
    if (!x)
        return NULL;
    x->mem = (const uint8_t *)buf;
    x->mem_size = size;
    x->io.read = mem_read;
    x->io.seek = mem_seek;
    x->io.ctx = x;
    return open_io(x);
}

static size_t file_read(void *ctx, void *buf, size_t n) { return fread(buf, 1, n, (FILE *)ctx); }
static int file_seek(void *ctx, uint64_t off)
{
#if defined(_WIN32)
    return _fseeki64((FILE *)ctx, (long long)off, SEEK_SET) ? -1 : 0;
#else
    return fseeko((FILE *)ctx, (off_t)off, SEEK_SET) ? -1 : 0;
#endif
}

XmvDemux *xmv_demux_open_file(const char *path)
{
    XmvDemux *x;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    x = (XmvDemux *)calloc(1, sizeof *x);
    if (!x) {
        fclose(fp);
        return NULL;
    }
    x->fp = fp;
    x->io.read = file_read;
    x->io.seek = file_seek;
    x->io.ctx = fp;
    return open_io(x);
}

void xmv_demux_close(XmvDemux *x)
{
    if (!x)
        return;
    if (x->fp)
        fclose(x->fp);
    free(x->buf);
    free(x);
}

const XmvInfo *xmv_demux_info(const XmvDemux *x) { return &x->info; }

int xmv_demux_rewind(XmvDemux *x)
{
    x->next_offset = x->first_offset;
    x->next_size = x->first_size;
    x->pts = 0;
    return 0;
}

int xmv_demux_next(XmvDemux *x, XmvPacket *pkt)
{
    uint32_t size = x->next_size, vword, vsize, frames, off, end, hdr;
    int nt = x->info.ntracks, i;
    uint8_t *p;

    memset(pkt, 0, sizeof *pkt);
    if (size == 0)
        return 0;
    if (size < 12u + 4u * (uint32_t)nt || size > (64u << 20))
        return x->next_size = 0, 0;            /* the chain's end (or nonsense) */
    if (size > x->buf_size) {
        uint8_t *nb = (uint8_t *)realloc(x->buf, size);
        if (!nb)
            return -1;
        x->buf = nb;
        x->buf_size = size;
    }
    if (x->io.seek(x->io.ctx, x->next_offset) < 0 || read_full(x, x->buf, size) < 0) {
        x->next_size = 0;
        return 0;                              /* a truncated file ends there */
    }
    p = x->buf;
    x->next_offset += size;
    x->next_size = rl32(p);

    vword = rl32(p + 4);
    vsize = vword & 0x007FFFFF;
    frames = (vword >> 23) & 0xFF;
    pkt->has_header = (vword >> 31) & 1;
    /* The video size field counts 4 bytes per audio track more than the video
     * data has (the audio sizes are right). */
    vsize = vsize >= 4u * (uint32_t)nt ? vsize - 4u * (uint32_t)nt : 0;

    hdr = 12 + 4 * (uint32_t)nt;
    off = hdr + vsize;
    for (i = 0; i < nt; i++) {
        uint32_t a = rl32(p + 12 + 4 * i) & 0x007FFFFF;
        if (a == 0 && i > 0)
            a = pkt->audio_size[i - 1];        /* duplicate tracks: the size repeats */
        if (off > size || a > size - off)
            return -1;
        pkt->audio[i] = p + off;
        pkt->audio_size[i] = a;
        off += a;
    }
    if (hdr + vsize > size)
        return -1;

    off = hdr;
    end = hdr + vsize;
    if (frames == 0)
        return 1;                              /* audio only */
    if (vsize > 0 && pkt->has_header) {
        if (vsize < 4)
            return -1;
        pkt->header = rl32(p + off);
        off += 4;
    }
    for (i = 0; i < (int)frames; i++) {
        uint32_t fh, fsize;
        if (end - off < 4)
            break;
        fh = rl32(p + off);
        fsize = (fh & 0x1FFFF) * 4 + 4;
        if (fsize + 4 > end - off)
            break;
        x->pts += fh >> 17;
        pkt->frame[pkt->nframes].data = p + off + 4;
        pkt->frame[pkt->nframes].size = fsize;
        pkt->frame[pkt->nframes].pts_ms = x->pts;
        /* The first bitstream bit (the frame type, 0 = I) is the top bit of
         * the first little-endian word. */
        pkt->frame[pkt->nframes].key = !(p[off + 4 + 3] & 0x80);
        pkt->nframes++;
        off += fsize + 4;
    }
    return 1;
}
