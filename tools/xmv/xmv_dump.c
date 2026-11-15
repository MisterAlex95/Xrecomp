/* xmv_dump: decode an XMV movie with the runtime's decoder (src/video/xmv,
 * XBA-70) to a Y4M video and a WAV sound file, and say how much of it the
 * decoder handled. For checking the decoder on movies from your own discs;
 * nothing is kept. Built with tests/xmv_decode:
 *
 *     cmake -S tests/xmv_decode -B build/xmv && cmake --build build/xmv
 *     build/xmv/xmv_dump movie.xmv [out.y4m] [out.wav]
 *
 * Compare with FFmpeg: ffmpeg -flags +bitexact -i movie.xmv -f yuv4mpegpipe ref.y4m */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xmv_player.h"

static void le16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f); }
static void le32(FILE *f, unsigned long v) { le16(f, (unsigned)(v & 0xFFFF)); le16(f, (unsigned)(v >> 16)); }

int main(int argc, char **argv)
{
    XmvPlayer *p;
    const XmvInfo *in;
    XmvPlayerStats st;
    FILE *vy = NULL, *wa = NULL;
    unsigned long pcm_bytes = 0;
    uint32_t pts = 0;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s movie.xmv [out.y4m] [out.wav]\n", argv[0]);
        return 2;
    }
    p = xmv_player_open_file(argv[1]);
    if (!p) {
        fprintf(stderr, "%s: not an XMV file\n", argv[1]);
        return 1;
    }
    in = xmv_player_info(p);
    printf("%s: XMV version %u, %ux%u, %u ms, %d audio track(s)\n", argv[1], in->version, in->width,
           in->height, in->duration_ms, in->ntracks);
    for (i = 0; i < in->ntracks; i++)
        printf("  track %d: codec 0x%04X, %u ch, %u Hz, %u bits, flags 0x%X%s\n", i, in->track[i].compression,
               in->track[i].channels, in->track[i].sample_rate, in->track[i].bits_per_sample, in->track[i].flags,
               i == xmv_player_audio_track(p) ? " (played)" : "");
    if (argc > 2 && !(vy = fopen(argv[2], "wb"))) {
        perror(argv[2]);
        return 1;
    }
    if (argc > 3 && xmv_player_audio_track(p) >= 0) {
        if (!(wa = fopen(argv[3], "wb"))) {
            perror(argv[3]);
            return 1;
        }
        fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wa);
        le32(wa, 16);
        le16(wa, 1);
        le16(wa, (unsigned)xmv_player_audio_channels(p));
        le32(wa, (unsigned long)xmv_player_audio_rate(p));
        le32(wa, (unsigned long)xmv_player_audio_rate(p) * 2ul * (unsigned long)xmv_player_audio_channels(p));
        le16(wa, (unsigned)(2 * xmv_player_audio_channels(p)));
        le16(wa, 16);
        fwrite("data\0\0\0\0", 1, 8, wa);
    }
    if (vy)
        fprintf(vy, "YUV4MPEG2 W%u H%u F30:1 Ip A1:1 C420jpeg\n", in->width, in->height);
    while (xmv_player_next_frame(p, &pts) == 1) {
        if (vy) {
            int pl, y, stride;
            fputs("FRAME\n", vy);
            for (pl = 0; pl < 3; pl++) {
                const uint8_t *s = xmv_player_plane(p, pl, &stride);
                unsigned pw = pl ? (in->width + 1) / 2 : in->width, ph = pl ? (in->height + 1) / 2 : in->height;
                for (y = 0; y < (int)ph; y++)
                    fwrite(s + (size_t)y * (size_t)stride, 1, pw, vy);
            }
        }
        for (;;) {
            int16_t buf[4096];
            size_t n = xmv_player_read_audio(p, buf, 4096 / 8);
            if (!n)
                break;
            if (wa) {
                size_t k, cnt = n * (size_t)xmv_player_audio_channels(p);
                for (k = 0; k < cnt; k++)
                    le16(wa, (unsigned)(uint16_t)buf[k]);
                pcm_bytes += (unsigned long)cnt * 2ul;
            }
        }
    }
    if (wa) {
        fseek(wa, 4, SEEK_SET);
        le32(wa, 36ul + pcm_bytes);
        fseek(wa, 40, SEEK_SET);
        le32(wa, pcm_bytes);
        fclose(wa);
    }
    if (vy)
        fclose(vy);
    xmv_player_stats(p, &st);
    printf("frames %u (last at %u ms): %u decoded, %u unchanged P, %u X8 intra not supported, %u damaged\n",
           st.frames, pts, st.decoded, st.skipped, st.unsupported, st.errors);
    xmv_player_close(p);
    return st.errors || st.unsupported ? 3 : 0;
}
