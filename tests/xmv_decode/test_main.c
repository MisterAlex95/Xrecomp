/* The portable XMV decoder (src/video/xmv, XBA-70) against references.
 *
 * Video: each fixture (tests/xmv_decode/fixtures, synthetic, made by
 * `python -m tools.xmv.xmv_mux make-test-set`) is decoded frame by frame and
 * every picture must be bit-identical to FFmpeg's decode of the same file
 * (its CRC-32 in fixtures/expected.h). Audio: the decoded PCM must be what
 * the ADPCM encoder reconstructed, and every block must decode exactly as
 * the APU's own Xbox ADPCM decoder (src/apu/apu_state.h) decodes it. Plus the
 * demuxer's view of the files, the colour conversion and the writing of a
 * picture into the D3D surface formats a title hands the movie hook. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xmv_player.h"
#include "xmv_demux.h"
#include "wmv2_dec.h"
#include "xbox_adpcm.h"
#include "xmv_surface.h"
#include "d3d/d3d8_swizzle.h"
#include "fixtures/expected.h"

#ifndef XMV_FIXTURE_DIR
#define XMV_FIXTURE_DIR "fixtures"
#endif

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t crc_table[256];
static void crc_init(void)
{
    uint32_t i, k, c;
    for (i = 0; i < 256; i++) {
        for (c = i, k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint8_t *load(const char *name, size_t *size)
{
    char path[512];
    FILE *f;
    uint8_t *buf;
    long n;
    snprintf(path, sizeof path, "%s/%s.xmv", XMV_FIXTURE_DIR, name);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static uint32_t picture_crc(const XmvPlayer *p, int w, int h)
{
    uint32_t crc = 0;
    int pl, y, stride;
    for (pl = 0; pl < 3; pl++) {
        const uint8_t *s = xmv_player_plane(p, pl, &stride);
        int pw = pl ? (w + 1) / 2 : w, ph = pl ? (h + 1) / 2 : h;
        for (y = 0; y < ph; y++)
            crc = crc_update(crc, s + (size_t)y * stride, (size_t)pw);
    }
    return crc;
}

/* The APU's decoder, for the cross-check (header-only in src/apu). */
#include "apu_state.h"

static void check_fixture(const XmvFixture *fx)
{
    size_t size;
    uint8_t *buf = load(fx->name, &size);
    XmvPlayer *p;
    XmvPlayerStats st;
    uint32_t pts, last_pts = 0, crc_pcm = 0;
    size_t pcm_frames = 0;
    int i, n = 0;

    CHECK(buf != NULL, "%s: fixture not found in %s", fx->name, XMV_FIXTURE_DIR);
    if (!buf)
        return;
    p = xmv_player_open_memory(buf, size);
    CHECK(p != NULL, "%s: opens", fx->name);
    if (!p) {
        free(buf);
        return;
    }
    CHECK(xmv_player_info(p)->width == (uint32_t)fx->width && xmv_player_info(p)->height == (uint32_t)fx->height,
          "%s: %ux%u", fx->name, xmv_player_info(p)->width, xmv_player_info(p)->height);
    CHECK(xmv_player_audio_channels(p) == fx->channels, "%s: %d audio channels", fx->name,
          xmv_player_audio_channels(p));

    while (xmv_player_next_frame(p, &pts) == 1) {
        uint32_t crc = picture_crc(p, fx->width, fx->height);
        if (n < fx->nframes)
            CHECK(crc == fx->crc[n], "%s frame %d: crc %08X, FFmpeg %08X", fx->name, n, crc, fx->crc[n]);
        CHECK(n == 0 || pts > last_pts, "%s frame %d: pts %u after %u", fx->name, n, pts, last_pts);
        last_pts = pts;
        n++;
        /* Drain the audio as a player would. */
        for (;;) {
            int16_t tmp[512];
            size_t got = xmv_player_read_audio(p, tmp, 512 / (fx->channels ? fx->channels : 1));
            if (!got)
                break;
            crc_pcm = crc_update(crc_pcm, (const uint8_t *)tmp, got * (size_t)fx->channels * 2);
            pcm_frames += got;
        }
    }
    CHECK(n == fx->nframes, "%s: %d frames, expected %d", fx->name, n, fx->nframes);
    xmv_player_stats(p, &st);
    CHECK(st.decoded == (uint32_t)fx->nframes && st.errors == 0 && st.unsupported == 0,
          "%s: decoded %u, errors %u, unsupported %u", fx->name, st.decoded, st.errors, st.unsupported);
    if (fx->channels) {
        CHECK(pcm_frames == fx->pcm_frames, "%s: %zu PCM frames, expected %u", fx->name, pcm_frames,
              fx->pcm_frames);
        CHECK(crc_pcm == fx->pcm_crc, "%s: PCM crc %08X, expected %08X", fx->name, crc_pcm, fx->pcm_crc);
    }

    /* Rewind: the same pictures again. */
    CHECK(xmv_player_rewind(p) == 0, "%s: rewinds", fx->name);
    for (i = 0; i < fx->nframes && xmv_player_next_frame(p, &pts) == 1; i++)
        if (picture_crc(p, fx->width, fx->height) != fx->crc[i])
            break;
    CHECK(i == fx->nframes, "%s: after a rewind, frame %d differs", fx->name, i);

    /* Every audio block decodes as the APU decodes it. */
    if (fx->channels) {
        XmvDemux *x = xmv_demux_open_memory(buf, size);
        XmvPacket pkt;
        int blocks = 0, same = 0;
        while (x && xmv_demux_next(x, &pkt) == 1) {
            size_t blk = 36u * (size_t)fx->channels, o;
            for (o = 0; o + blk <= pkt.audio_size[0]; o += blk) {
                int16_t a[65 * 8], b[65 * 8];
                xbox_adpcm_decode_block(a, pkt.audio[0] + o, fx->channels);
                adpcm_decode_block(b, pkt.audio[0] + o, blk, fx->channels);
                blocks++;
                same += !memcmp(a, b, 65 * (size_t)fx->channels * sizeof(int16_t));
            }
        }
        CHECK(blocks > 0 && same == blocks, "%s: %d of %d ADPCM blocks as the APU decodes them", fx->name,
              same, blocks);
        xmv_demux_close(x);
    }
    printf("%-10s %3dx%-3d %2d frames  %s\n", fx->name, fx->width, fx->height, n,
           fx->channels ? "+ audio" : "");
    xmv_player_close(p);
    free(buf);
}

/* ADPCM edge cases against the APU's decoder: random blocks, including
 * out-of-range and negative step indices and saturating codes. */
static void check_adpcm_random(void)
{
    uint32_t seed = 12345;
    int t, ch, same = 0;
    for (t = 0; t < 2000; t++) {
        uint8_t blk[36 * 2];
        int16_t a[65 * 2], b[65 * 2];
        int i;
        ch = 1 + t % 2;           /* the APU decodes mono and stereo */
        for (i = 0; i < 36 * ch; i++) {
            seed = seed * 1103515245u + 12345u;
            blk[i] = (uint8_t)(seed >> 16);
        }
        xbox_adpcm_decode_block(a, blk, ch);
        adpcm_decode_block(b, blk, 36u * (size_t)ch, ch);
        same += !memcmp(a, b, 65 * (size_t)ch * sizeof(int16_t));
    }
    CHECK(same == 2000, "random ADPCM blocks: %d of 2000 as the APU decodes them", same);
    CHECK(xbox_adpcm_decode_block(NULL, NULL, 0) == 0, "0 channels is refused");
}

static void check_demux_errors(void)
{
    uint8_t junk[64];
    memset(junk, 0, sizeof junk);
    CHECK(xmv_demux_open_memory(junk, sizeof junk) == NULL, "a file without the magic is refused");
    memcpy(junk + 12, "xobX", 4);
    junk[16] = 9;                 /* version 9 */
    CHECK(xmv_demux_open_memory(junk, sizeof junk) == NULL, "an unknown version is refused");
    CHECK(xmv_demux_open_memory(junk, 20) == NULL, "a truncated header is refused");
}

static void check_truncated(void)
{
    size_t size;
    uint8_t *buf = load("basic", &size);
    XmvPlayer *p;
    uint32_t pts;
    int n = 0;
    if (!buf)
        return;
    /* Cut in the middle of the second packet: the first packet's frames play,
     * then the movie ends (no crash, no hang). */
    p = xmv_player_open_memory(buf, 9000);
    CHECK(p != NULL, "a truncated file opens");
    while (p && xmv_player_next_frame(p, &pts) == 1 && n < 100)
        n++;
    CHECK(n == 4, "a truncated file plays its whole packets: %d frames", n);
    xmv_player_close(p);
    free(buf);
}

/* The decoder alone, configured from a WMV file's 4-byte extradata (FFmpeg's
 * encoder wrote f0 c3 b4 80 for "basic") instead of the XMV header word: the
 * same pictures. */
static void check_wmv_extradata(void)
{
    static const uint8_t ext[4] = { 0xf0, 0xc3, 0xb4, 0x80 };
    size_t size;
    uint8_t *buf = load("basic", &size);
    XmvDemux *x;
    XmvPacket pkt;
    Wmv2Dec *d;
    int n = 0, ok = 1, i;
    if (!buf)
        return;
    x = xmv_demux_open_memory(buf, size);
    d = wmv2_dec_create(96, 64);
    wmv2_dec_set_wmv_extradata(d, ext);
    while (x && d && xmv_demux_next(x, &pkt) == 1) {
        for (i = 0; i < pkt.nframes; i++, n++) {
            uint32_t crc = 0;
            int pl, y, stride;
            ok &= wmv2_dec_decode(d, pkt.frame[i].data, pkt.frame[i].size, WMV2_LE_WORDS) == WMV2_OK;
            ok &= wmv2_dec_last_was_key(d) == pkt.frame[i].key;
            for (pl = 0; pl < 3; pl++) {
                const uint8_t *p = wmv2_dec_plane(d, pl, &stride);
                for (y = 0; y < (pl ? 32 : 64); y++)
                    crc = crc_update(crc, p + (size_t)y * stride, pl ? 48 : 96);
            }
            ok &= crc == xmv_fixtures[0].crc[n];
        }
    }
    CHECK(ok && n == 12 && wmv2_dec_width(d) == 96 && wmv2_dec_height(d) == 64,
          "WMV extradata configures the decoder as the XMV header does (%d frames)", n);
    wmv2_dec_destroy(d);
    xmv_demux_close(x);
    free(buf);
}

static void check_damaged(void)
{
    /* Random bytes as a frame: an error or a picture, never a crash. */
    Wmv2Dec *d = wmv2_dec_create(96, 64);
    uint8_t frame[256];
    uint32_t seed = 99;
    int t, i;
    for (t = 0; t < 500; t++) {
        for (i = 0; i < (int)sizeof frame; i++) {
            seed = seed * 1103515245u + 12345u;
            frame[i] = (uint8_t)(seed >> 16);
        }
        wmv2_dec_set_xmv_header(d, seed & 0x1FF);
        (void)wmv2_dec_decode(d, frame, 4 + (seed % 252), (t & 1) ? WMV2_LE_WORDS : 0);
    }
    CHECK(wmv2_dec_plane(d, 0, NULL) != NULL, "the decoder survives damaged frames");
    wmv2_dec_destroy(d);
    CHECK(wmv2_dec_create(1, 64) == NULL && wmv2_dec_create(64, 5000) == NULL, "bad sizes are refused");
}

static void check_colour(void)
{
    /* Studio-range black, white and a saturated red-ish pair. */
    uint8_t y[4] = { 16, 235, 81, 81 }, u[1] = { 128 }, v[1] = { 128 };
    uint8_t y2[4] = { 81, 81, 81, 81 }, u2[1] = { 90 }, v2[1] = { 240 };
    const uint8_t *pl[3] = { y, u, v };
    const uint8_t *pl2[3] = { y2, u2, v2 };
    int st[3] = { 2, 1, 1 };
    uint8_t out[16], yuy2[8];
    xmv_yuv420_to_bgra(pl, st, 2, 2, out, 8);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 255, "Y 16 is black");
    CHECK(out[4] == 255 && out[5] == 255 && out[6] == 255, "Y 235 is white");
    xmv_yuv420_to_bgra(pl2, st, 2, 2, out, 8);
    CHECK(out[2] >= 250 && out[1] < 40 && out[0] < 40, "Cr 240 Cb 90 is red: %d %d %d", out[2], out[1], out[0]);
    xmv_yuv420_to_yuy2(pl, st, 2, 1, yuy2, 4);
    CHECK(yuy2[0] == 16 && yuy2[1] == 128 && yuy2[2] == 235 && yuy2[3] == 128, "YUY2 is Y0 U Y1 V");
}

static void check_surface(void)
{
    /* A 6x4 picture: luma ramp, neutral chroma (grey levels). */
    uint8_t y[24], u[6], v[6];
    const uint8_t *pl[3] = { y, u, v };
    int st[3] = { 6, 3, 3 }, i, x;
    uint32_t f, w, h, pitch;
    uint8_t surf[64 * 8 * 4], lin[8 * 8 * 4];
    for (i = 0; i < 24; i++) y[i] = (uint8_t)(16 + i * 9);
    memset(u, 128, sizeof u);
    memset(v, 128, sizeof v);

    /* LIN_X8R8G8B8 640x480, pitch 2560 (Size dword), and swizzled 512x256. */
    CHECK(xmv_surface_layout(0x1E << 8, 639u | (479u << 12) | (39u << 24), &f, &w, &h, &pitch) == 2560u * 480u
          && f == 0x1E && w == 640 && h == 480 && pitch == 2560, "LIN_X8R8G8B8 layout: %u %ux%u %u", f, w, h, pitch);
    CHECK(xmv_surface_layout((0x06u << 8) | (9u << 20) | (8u << 24), 0, &f, &w, &h, &pitch) == 512u * 256u * 4u
          && w == 512 && h == 256, "A8R8G8B8 layout from the log2 sizes: %ux%u", w, h);
    CHECK(xmv_surface_layout(0x0Cu << 8, 0, &f, &w, &h, &pitch) == 0, "DXT1 is not a movie surface");

    /* Linear 32-bit with a wider pitch: rows land at the pitch, the rest untouched. */
    memset(surf, 0xAB, sizeof surf);
    CHECK(xmv_surface_write(pl, st, 6, 4, 0x1E, 64, 8, 64 * 4, surf, sizeof surf) == 0, "LIN_X8R8G8B8 written");
    CHECK(surf[0] == 0 && surf[3] == 255 && surf[64 * 4 + 0] > surf[0], "first pixels: %d %d", surf[0], surf[64 * 4]);
    CHECK(surf[6 * 4] == 0xAB && surf[4 * 64 * 4] == 0xAB, "outside the picture is left alone");
    /* YUY2 */
    CHECK(xmv_surface_write(pl, st, 6, 4, 0x24, 8, 4, 16, surf, sizeof surf) == 0, "YUY2 written");
    CHECK(surf[0] == y[0] && surf[1] == 128 && surf[2] == y[1] && surf[3] == 128, "YUY2 bytes");
    /* UYVY swaps each byte pair. */
    CHECK(xmv_surface_write(pl, st, 6, 4, 0x25, 8, 4, 16, surf, sizeof surf) == 0 && surf[0] == 128 && surf[1] == y[0],
          "UYVY bytes");
    /* Swizzled A8R8G8B8 8x8: unswizzled again it is the linear picture. */
    CHECK(xmv_surface_write(pl, st, 6, 4, 0x06, 8, 8, 0, surf, 8 * 8 * 4) == 0, "A8R8G8B8 written");
    xbox_unswizzle_rect(lin, surf, 8, 8, 4);
    {
        uint8_t ref[8 * 8 * 4];
        int ok = 1;
        memset(ref, 0, sizeof ref);
        xmv_surface_write(pl, st, 6, 4, 0x12, 8, 8, 32, ref, sizeof ref);
        for (i = 0; i < 4 && ok; i++)
            for (x = 0; x < 6 * 4; x++)
                ok &= lin[i * 32 + x] == ref[i * 32 + x];
        CHECK(ok, "swizzled = linear after unswizzling");
    }
    CHECK(xmv_surface_write(pl, st, 6, 4, 0x06, 6, 8, 0, surf, sizeof surf) < 0, "a swizzled non-power-of-two is refused");
    /* R5G6B5: white-ish and black ends of the ramp. */
    CHECK(xmv_surface_write(pl, st, 6, 4, 0x11, 6, 4, 12, surf, sizeof surf) == 0, "LIN_R5G6B5 written");
    CHECK(surf[0] == 0 && surf[1] == 0, "Y 16 is 0x0000 in 565");
    CHECK(xmv_surface_write(pl, st, 6, 4, 0x0C, 8, 8, 32, surf, sizeof surf) < 0, "DXT1 is refused");
    CHECK(xmv_surface_write(pl, st, 6, 4, 0x1E, 64, 8, 64 * 4, surf, 100) < 0, "a too-small buffer is refused");
}

int main(void)
{
    size_t i;
    crc_init();
    for (i = 0; i < sizeof xmv_fixtures / sizeof xmv_fixtures[0]; i++)
        check_fixture(&xmv_fixtures[i]);
    check_adpcm_random();
    check_demux_errors();
    check_truncated();
    check_damaged();
    check_wmv_extradata();
    check_colour();
    check_surface();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("xmv_decode: all passed\n");
    return 0;
}
