/* x360_xma_av_test -- the libavcodec XMA backend (XBA-299) on packets written here. We have no XMA encoder and no game
 * audio, so the frames carry no coefficients: this proves the decoder is reached and configured, follows frames across
 * packets and survives bad data. It does not prove how a real title's music sounds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x360_core.h"

extern const x360_xma_backend x360_xma_av_backend;

#define PACKET 2048
static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

typedef struct { uint8_t *p; unsigned bit; } bits;

static void put(bits *b, unsigned n, uint32_t v)
{
    while (n--) {
        if ((v >> n) & 1)
            b->p[b->bit >> 3] |= (uint8_t)(0x80 >> (b->bit & 7));
        b->bit++;
    }
}

/* one mono frame of 512 samples, no coefficients (31 bits; the layout libavcodec's wmaprodec.c reads for XMA) */
static void silent_frame(bits *b, int more)
{
    put(b, 15, 31); /* the frame's length in bits */
    put(b, 1, 1);   /* every channel has the same subframes */
    put(b, 1, 0);   /* one subframe of the maximum length */
    put(b, 8, 0);   /* dynamic range gain */
    put(b, 1, 0);   /* no trim */
    put(b, 1, 0);   /* no extended header */
    put(b, 1, 0);   /* reserved */
    put(b, 1, 0);   /* this channel sends no coefficients */
    put(b, 1, 0);   /* padding */
    put(b, 1, (uint32_t)more);
}

static void packet(uint8_t *p, int frames)
{
    memset(p, 0, PACKET);
    bits b = {p, 0};
    put(&b, 6, (uint32_t)frames);
    put(&b, 15, 0); /* no bits of an earlier frame */
    put(&b, 3, 0);
    put(&b, 8, 0);
    for (int f = 0; f < frames; f++)
        silent_frame(&b, f + 1 < frames);
}

int main(void)
{
    const x360_xma_backend *av = &x360_xma_av_backend;
    uint8_t p[PACKET];
    static int16_t pcm[64 * 512 * 2];

    /* frames written in a packet come out as samples, silent; what libavcodec holds back comes out at the drain */
    void *d = av->open(1, 48000);
    CHECK(d != NULL, "the decoder opens");
    int total = 0;
    for (int k = 0; k < 8; k++) {
        packet(p, 4);
        memset(pcm, 0x55, sizeof pcm);
        int n = av->decode(d, p, pcm, 64 * 512 * 2);
        CHECK(n >= 0 && n <= 4 * 512, "packet %d gave %d samples", k, n);
        for (int s = 0; s < n; s++)
            CHECK(pcm[s] == 0, "packet %d sample %d is %d", k, s, pcm[s]);
        total += n;
    }
    CHECK(total < 32 * 512, "libavcodec holds samples back until the end (%d)", total);
    int last = av->drain(d, pcm, 64 * 512 * 2);
    for (int s = 0; s < last; s++)
        CHECK(pcm[s] == 0, "drained sample %d is %d", s, pcm[s]);
    /* 32 frames went in; the first 64 samples are the codec's start-up, the end may lose its overlap */
    CHECK(total + last >= 31 * 512 - 64 && total + last <= 32 * 512, "32 frames gave %d + %d samples", total, last);
    av->close(d);

    /* a sound of one short packet is not lost in the decoder */
    d = av->open(1, 48000);
    packet(p, 2);
    total = av->decode(d, p, pcm, 64 * 512 * 2);
    total += av->drain(d, pcm, 64 * 512 * 2);
    CHECK(total >= 512 - 64 && total <= 2 * 512, "a two-frame sound gave %d samples", total);
    av->close(d);

    /* a stereo context opens at every sample rate the context can name */
    static const int rates[4] = {24000, 32000, 44100, 48000};
    for (int r = 0; r < 4; r++) {
        d = av->open(2, rates[r]);
        CHECK(d != NULL, "stereo at %d", rates[r]);
        av->close(d);
    }

    /* bad data: no crash, never more samples than asked for, even or exact to the stream's channels */
    srand(1);
    for (int ch = 1; ch <= 2; ch++) {
        d = av->open(ch, 48000);
        for (int k = 0; k < 3000; k++) {
            for (int b = 0; b < PACKET; b++)
                p[b] = (uint8_t)rand();
            if (k % 3 == 0)
                packet(p, 1 + k % 8), p[100 + k % 1800] ^= (uint8_t)(1 << (k % 8));
            int n = av->decode(d, p, pcm, 4096);
            CHECK(n >= 0 && n <= 4096 && n % ch == 0, "bad packet %d gave %d samples", k, n);
        }
        av->close(d);
    }
    printf(failures ? "x360_xma_av: %d failures\n" : "x360_xma_av: all checks passed (%d failures)\n", failures);
    return failures ? 1 : 0;
}
