/* x360_xma_av.c -- XMA2 packets decoded by libavcodec (XBA-299), the default backend of x360_xma.c where the build found
 * it. FFmpeg is LGPL-2.1-or-later and is only linked dynamically (NOTICE « FFmpeg »); none of its code is here.
 *
 * One decoder per XMA context, a single stream of one or two channels. A packet is handed over whole: libavcodec follows
 * the frames across packets itself. Its floats become 16-bit samples, interleaved when the stream is stereo. A packet it
 * refuses gives one silent frame per frame of its header, so the title's buffers keep flowing. libavcodec keeps the last
 * 4096 samples of a stream back until it is told the stream ended: av_drain gets them. */
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <string.h>

#include "x360_core.h"

#define XMA_PACKET_BYTES 2048
#define XMA_FRAME_SAMPLES 512

typedef struct av_dec {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    int channels;
} av_dec;

static void av_close(void *h)
{
    av_dec *d = h;
    if (!d)
        return;
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    av_free(d);
}

static void *av_open(int channels, int rate)
{
    av_log_set_level(AV_LOG_FATAL); /* a damaged packet is answered with silence, not a line in the log */
    av_dec *d = av_mallocz(sizeof *d);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    if (!d)
        return NULL;
    d->channels = channels;
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!d->pkt || !d->frame || !d->ctx)
        goto fail;
    d->ctx->sample_rate = rate;
    d->ctx->block_align = XMA_PACKET_BYTES;
    d->ctx->thread_count = 1;
    av_channel_layout_default(&d->ctx->ch_layout, channels);
    /* an XMA2WAVEFORMATEX as libavcodec reads it: 16-bit stream count, then the 32-bit channel mask, little-endian */
    d->ctx->extradata = av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!d->ctx->extradata)
        goto fail;
    d->ctx->extradata_size = 34;
    d->ctx->extradata[0] = 1;
    d->ctx->extradata[2] = channels > 1 ? 3 : 4; /* front left + right, or front centre */
    if (avcodec_open2(d->ctx, codec, NULL) < 0)
        goto fail;
    return d;
fail:
    /* no decoder: av_decode gives silence */
    avcodec_free_context(&d->ctx);
    return d;
}

static int silence(const uint8_t *packet, int channels, int16_t *out, int max_samples)
{
    int n = (packet[0] >> 2) * XMA_FRAME_SAMPLES * channels;
    n = n > max_samples ? max_samples : n;
    memset(out, 0, (size_t)n * sizeof *out);
    return n;
}

static int16_t to_s16(float f)
{
    f *= 32768.0f;
    return f >= 32767.0f ? 32767 : f <= -32768.0f ? -32768 : f != f ? 0 : (int16_t)f;
}

/* every frame the decoder has ready, as 16-bit samples */
static int av_frames(av_dec *d, int16_t *out, int max_samples)
{
    int n = 0;
    while (avcodec_receive_frame(d->ctx, d->frame) >= 0) {
        int ch = d->frame->ch_layout.nb_channels < d->channels ? d->frame->ch_layout.nb_channels : d->channels;
        for (int s = 0; s < d->frame->nb_samples && n + d->channels <= max_samples; s++, n += d->channels)
            for (int c = 0; c < d->channels; c++)
                out[n + c] = to_s16(((const float *)d->frame->extended_data[c < ch ? c : 0])[s]);
        av_frame_unref(d->frame);
    }
    return n;
}

static int av_drain(void *h, int16_t *out, int max_samples)
{
    av_dec *d = h;
    if (!d || !d->ctx || avcodec_send_packet(d->ctx, NULL) < 0)
        return 0;
    return av_frames(d, out, max_samples);
}

static int av_decode(void *h, const uint8_t *packet, int16_t *out, int max_samples)
{
    av_dec *d = h;
    if (!d)
        return 0;
    if (!d->ctx)
        return silence(packet, d->channels, out, max_samples);
    uint8_t buf[XMA_PACKET_BYTES + AV_INPUT_BUFFER_PADDING_SIZE] = {0};
    memcpy(buf, packet, XMA_PACKET_BYTES);
    d->pkt->data = buf;
    d->pkt->size = XMA_PACKET_BYTES;
    if (avcodec_send_packet(d->ctx, d->pkt) < 0) {
        avcodec_flush_buffers(d->ctx);
        return silence(packet, d->channels, out, max_samples);
    }
    return av_frames(d, out, max_samples);
}

const x360_xma_backend x360_xma_av_backend = {"libavcodec", av_open, av_decode, av_close, av_drain};
