/* NV2A coverage (XBA-120): what the executor and its back ends do not draw
 * is counted by name, and what they do draw is checked against the CPU path.
 *
 * Methods go in as a title's pushbuffer would deliver them, into the real
 * executor (nv2a_pb_exec.c, linked) over a fake guest memory, with no back
 * end (the CPU rasteriser) or a fake one that records what it is handed:
 *
 *   1. the format table (nv2a_coverage.c) says "decoded" for exactly the
 *      formats nv2a_backend_decode_texture reads, every code 0x00-0x41;
 *   2. method names from xemu's list, arrays and light blocks included;
 *      mip level offsets (each level after the last, in its own layout);
 *   3. a batch of points, a texture format nothing decodes, a volume
 *      texture, logic op, BRDF, a stage mode on a stage it is not valid
 *      on, DOT_ZW's depth on the CPU and a texgen mode the channel does
 *      not have (sphere map on R; XBA-306 models the others) are each
 *      counted once per batch under their names, and reach the status
 *      file's "unsupported"; the stage modes and YUV formats XBA-311
 *      models are not;
 *   4. the [GPU] report runs and the status file's "unmodelled" names the
 *      method nothing models (SET_LOGIC_OP).
 *   5. whole-image decode (the GL back end's uploads) reads, for every
 *      decoded uncompressed format and random images of several sizes,
 *      the same texels as decoding each texel alone at the address the
 *      per-texel sampler computes (swizzle_offset, or the row and pitch);
 *      also prints the time of 512x512 SZ_R5G6B5 and SZ_A8R8G8B8 decodes.
 *
 * Guest memory is a buffer addressed by the offsets the title would use. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nv2a_backend.h"
#include "nv2a_coverage.h"
#include "recomp_status.h"
#include "d3d/d3d8_swizzle.h"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <unistd.h>
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

#define MEM_BYTES 0x400000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
void xbox_FramebufferWindowSetFormat(uint32_t f) { (void)f; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowFrameTris(uint32_t tris) { (void)tris; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
int xbox_FramebufferWindowPresentHost(const void *px, uint32_t w, uint32_t h, uint32_t pitch,
                                      uint32_t format)
{ (void)px; (void)w; (void)h; (void)pitch; (void)format; return 0; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
void xbox_HostSleepNs(uint64_t ns) { (void)ns; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

/* ---- a fake back end ----------------------------------------------------- */
static struct {
    int draws;
    uint32_t count;
} s_be;

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{ (void)s; (void)rs; (void)flags; (void)argb; (void)zstencil; }
static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    (void)s;
    s_be.draws++;
    s_be.count = b->count;
}
static void be_flip(void) {}
static const Nv2aBackend s_backend_px = { be_clear, be_draw, be_flip, NV2A_BACKEND_CAP_PIXEL };

/* ---- driving the executor ---------------------------------------------- */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static char *last_line(const char *path)
{
    static char line[4096], buf[4096];
    FILE *f = fopen(path, "r");
    line[0] = 0;
    if (!f)
        return line;
    while (fgets(buf, sizeof buf, f))
        strcpy(line, buf);
    fclose(f);
    return line;
}

/* 1. Every code: decode a small texture of it; the table must agree. */
static void check_format_table(void)
{
    uint32_t fmt, argb[16 * 16 * 2];
    int mism = 0, known = 0, decoded = 0;

    memset(s_mem + 0x40000, 0x5A, 0x4000);
    for (fmt = 0; fmt < 0x50; fmt++) {
        const Nv2aTexFormat *f = nv2a_tex_format(fmt);
        Nv2aTexture t;
        int ok;
        memset(&t, 0, sizeof t);
        t.offset = 0x40000;
        t.width = t.height = 8;
        t.pitch = 8 * 4;                  /* linear: wide enough for any texel */
        t.color = fmt;
        t.levels = 1;
        t.palette = 0x44000;
        ok = nv2a_backend_decode_texture(&t, argb);
        if (f->name)
            known++;
        decoded += ok;
        if (ok != f->decoded) {
            printf("format 0x%02X %s: table says %s, the decoder %s\n", fmt,
                   f->name ? f->name : "(none)", f->decoded ? "decoded" : "not decoded",
                   ok ? "reads it" : "does not");
            mism++;
        }
        CHECK(f->gap != NULL && strstr(f->gap, "texture format") == f->gap);
    }
    printf("1. format table: %d NV2A formats, %d decoded, %d disagreements\n", known, decoded, mism);
    CHECK(mism == 0);
    CHECK(known == 59);
    CHECK(!strcmp(nv2a_tex_format(0x26)->gap, "texture format 0x26 LU_IMAGE_A8CR8CB8Y8"));
    CHECK(nv2a_tex_format(0x1A)->bytes == 2 && nv2a_tex_format(0x1A)->swizzled);
    CHECK(nv2a_tex_format(0x77)->name == NULL);
}

/* 1b. d3d8_format_is_swizzled agrees with the table's SZ_ / L* split
 * (XBA-138): LU_IMAGE_A8CR8CB8Y8 (0x26) and LC_IMAGE_A4V6YB6A4U6YA6 (0x34)
 * are linear, which it called swizzled. The SZ_DEPTH formats 0x2A-0x2D are
 * left out on purpose: the table has them swizzled (xemu), the classifier
 * linear, and the D3D side treats those codes as depth surfaces -- a
 * separate question from the ticket's two formats. */
static void check_swizzle_classifier(void)
{
    uint32_t fmt;
    int mism = 0;

    for (fmt = 0; fmt < 0x42; fmt++) {
        const Nv2aTexFormat *f = nv2a_tex_format(fmt);
        int linear_block = fmt == 0x0C || fmt == 0x0E || fmt == 0x0F;   /* DXT: neither */
        if (!f->name || linear_block || (fmt >= 0x2A && fmt <= 0x2D))
            continue;
        if (!!d3d8_format_is_swizzled(fmt) != !!f->swizzled) {
            printf("format 0x%02X %s: table %s, d3d8_format_is_swizzled %s\n", fmt, f->name,
                   f->swizzled ? "swizzled" : "linear",
                   d3d8_format_is_swizzled(fmt) ? "swizzled" : "linear");
            mism++;
        }
    }
    printf("1b. swizzle classifier: %d disagreements with the format table\n", mism);
    CHECK(mism == 0);
    CHECK(!d3d8_format_is_swizzled(0x26));
    CHECK(!d3d8_format_is_swizzled(0x34));
}

/* 2. Method names. */
static void check_method_names(void)
{
    CHECK(!strcmp(nv2a_method_name(0x043C), "SET_POINT_SIZE"));
    CHECK(!strcmp(nv2a_method_name(0x17BC), "SET_LOGIC_OP_ENABLE"));
    CHECK(!strcmp(nv2a_method_name(0x0498), "SET_MODEL_VIEW_MATRIX"));       /* array */
    CHECK(!strcmp(nv2a_method_name(0x1000 + 3 * 0x80 + 0x6C), "SET_LIGHT_LOCAL_ATTENUATION"));
    CHECK(!strcmp(nv2a_method_name(0x1B04 + 2 * 0x40), "SET_TEXTURE_FORMAT"));
    CHECK(!strcmp(nv2a_method_name(0x03C8 + 0x10), "SET_TEXGEN_R"));
    CHECK(nv2a_method_name(0x0380) == NULL);           /* xemu names nothing there */
    CHECK(nv2a_method_name(0x1F00) == NULL);
    CHECK(!strcmp(nv2a_stage_mode_name(0x11), "texture stage mode 0x11 DOTPRODUCT"));
    printf("2. method names: ok\n");
}

/* The formats XBA-120 added: one texel each, decoded exactly. */
static void check_new_formats(void)
{
    static const struct { uint32_t fmt, bytes, raw, want; } c[] = {
        { 0x14, 1, 0x9A, 0xFF9A9A9Au },          /* LU_IMAGE_SY8 */
        { 0x15, 2, 0x0135, 0xFF9A9A9Au },        /* LU_IMAGE_X7SY9 */
        { 0x16, 2, 0x12AB, 0xFF12FFABu },        /* LU_IMAGE_R8B8 */
        { 0x29, 2, 0x12AB, 0xFF12FFABu },        /* SZ_R8B8 */
        { 0x17, 2, 0x34CD, 0xFFFF34CDu },        /* LU_IMAGE_G8B8 */
        { 0x28, 2, 0x34CD, 0xFFFF34CDu },        /* SZ_G8B8 (D3D V8U8) */
        { 0x18, 2, 0x34CD, 0xFFFF34CDu },        /* LU_IMAGE_SG8SB8 */
        { 0x1A, 2, 0x8040, 0x80404040u },        /* SZ_A8Y8 */
        { 0x20, 2, 0x8040, 0x80404040u },        /* LU_IMAGE_A8Y8 */
        { 0x27, 2, 0xFC1F, 0xFFFF00FFu },        /* SZ_R6G5B5 */
        { 0x37, 2, 0x03E0, 0xFF00FF00u },        /* LU_IMAGE_R6G5B5 */
        { 0x2E, 4, 0xAB123456u, 0xFFABABABu },   /* LU_IMAGE_DEPTH_X8_Y24_FIXED */
        { 0x30, 2, 0xCD12, 0xFFCDCDCDu },        /* LU_IMAGE_DEPTH_Y16_FIXED */
        { 0x32, 2, 0x7F00, 0xFF7F7F7Fu },        /* SZ_Y16 */
        { 0x35, 2, 0x7F00, 0xFF7F7F7Fu },        /* LU_IMAGE_Y16 */
        { 0x33, 4, 0x80004000u, 0xFFFF8040u },   /* SZ_YB16YA16 (D3D V16U16) */
        { 0x38, 2, 0xF801, 0xFFFF0000u },        /* SZ_R5G5B5A1 */
        { 0x3D, 2, 0x07C0, 0x0000FF00u },        /* LU_IMAGE_R5G5B5A1 */
        { 0x39, 2, 0xF00F, 0xFFFF0000u },        /* SZ_R4G4B4A4 */
        { 0x3E, 2, 0x0F80, 0x0000FF88u },        /* LU_IMAGE_R4G4B4A4 */
        { 0x3A, 4, 0x11223344u, 0x11443322u },   /* SZ_A8B8G8R8 */
        { 0x3B, 4, 0x11223344u, 0x44332211u },   /* SZ_B8G8R8A8 */
        { 0x3C, 4, 0x11223344u, 0x44112233u },   /* SZ_R8G8B8A8 */
        /* XBA-311, BT.601 video range: Y 81, Cb 90, Cr 240 is red. */
        { 0x26, 4, 0xC0F05A51u, 0xC0FF0000u },   /* LU_IMAGE_A8CR8CB8Y8: A Cr Cb Y */
        /* The even texel: A4 U6 Y6 in bits 15-0 (8, 0x16, 0x14), V6 in
         * 27-22 (0x3C); six bits scale to 81, 89, 242, four to 0x88. */
        { 0x34, 2, 0x0F008594u, 0x88FF0000u },   /* LC_IMAGE_A4V6YB6A4U6YA6 */
    };
    size_t i;
    int bad = 0;

    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        Nv2aTexture t;
        uint32_t out = 0;
        memset(&t, 0, sizeof t);
        t.offset = 0x30000;
        t.width = t.height = 1;
        t.pitch = 4;
        t.color = c[i].fmt;
        t.levels = 1;
        memcpy(s_mem + 0x30000, &c[i].raw, 4);
        if (!nv2a_backend_decode_texture(&t, &out) || out != c[i].want) {
            printf("format 0x%02X: 0x%08X reads 0x%08X, want 0x%08X\n", c[i].fmt, c[i].raw,
                   out, c[i].want);
            bad++;
        }
    }
    {
        /* 0x34's odd texel: its own A4 Y6 in bits 31-16 (4, 0x3F), the
         * pair's U and V. */
        Nv2aTexture t;
        uint32_t out[2] = { 0, 0 }, raw = 0x4F3F8594u;
        memset(&t, 0, sizeof t);
        t.offset = 0x30000;
        t.width = 2;
        t.height = 1;
        t.pitch = 4;
        t.color = 0x34;
        t.levels = 1;
        memcpy(s_mem + 0x30000, &raw, 4);
        if (!nv2a_backend_decode_texture(&t, out) || out[0] != 0x88FF0000u
            || out[1] != 0x44FFC9C8u) {
            printf("format 0x34 pair: 0x%08X 0x%08X\n", out[0], out[1]);
            bad++;
        }
    }
    CHECK(bad == 0);
    printf("2c. added formats: %d of %d wrong\n", bad, (int)(sizeof c / sizeof c[0]) + 1);
}

/* 5. Whole-image decode against one texel at a time. */
static uint32_t s_rng = 12345u;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 8;
}

#define DEC_AT  0x100000u
#define DEC_PAL 0x300000u
static int decode_matches(uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch)
{
    static uint32_t img[256 * 256];
    const Nv2aTexFormat *f = nv2a_tex_format(fmt);
    Nv2aTexture t;
    uint32_t x, y, bad = 0;

    memset(&t, 0, sizeof t);
    t.offset = DEC_AT;
    t.width = w;
    t.height = h;
    t.pitch = pitch;
    t.color = fmt;
    t.levels = 1;
    t.palette = DEC_PAL;
    if (!nv2a_backend_decode_texture(&t, img))
        return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            Nv2aTexture one = t;
            uint32_t want[2] = { 0, 0 }, k = 0;
            one.width = one.height = 1;
            one.pitch = 4;
            if (f->swizzled) {
                one.offset = DEC_AT + swizzle_offset(x, y, w, h) * f->bytes;
            } else if (fmt == 0x24 || fmt == 0x25 || fmt == 0x34) {  /* YUV: two texels a group */
                one.offset = DEC_AT + y * pitch + (x & ~1u) * 2u;
                one.width = 2;
                one.pitch = 4;
                k = x & 1u;
            } else {
                one.offset = DEC_AT + y * pitch + x * f->bytes;
            }
            if (!nv2a_backend_decode_texture(&one, want) || want[k] != img[y * w + x])
                bad++;
        }
    if (bad)
        printf("format 0x%02X %ux%u pitch %u: %u texels differ\n", fmt, w, h, pitch, bad);
    return bad == 0;
}

static void check_decode_walk(void)
{
    static const uint32_t sz[][2] = { { 8, 8 }, { 256, 64 }, { 16, 128 }, { 1, 32 }, { 64, 1 },
                                      { 32, 32 } };
    uint32_t fmt, i, n = 0, bad = 0;
    double ms;
    clock_t c0;

    for (i = 0; i < 0x80000; i++)
        s_mem[DEC_AT + i] = (uint8_t)rnd();
    for (i = 0; i < 1024; i++)
        s_mem[DEC_PAL + i] = (uint8_t)rnd();
    for (fmt = 0; fmt <= 0x41; fmt++) {
        const Nv2aTexFormat *f = nv2a_tex_format(fmt);
        if (!f->name || !f->decoded || !f->bytes)
            continue;
        for (i = 0; i < sizeof sz / sizeof sz[0]; i++) {
            uint32_t w = sz[i][0], h = sz[i][1], pitch = w * f->bytes;
            if (!f->swizzled) {                     /* odd sizes, padded rows */
                w = 1 + rnd() % 100u;
                if (fmt == 0x24 || fmt == 0x25 || fmt == 0x34)
                    w = (w + 1) & ~1u;
                h = 1 + rnd() % 60u;
                pitch = (w * f->bytes + 4u * (rnd() % 4u) + 3u) & ~3u;
            }
            n++;
            bad += !decode_matches(fmt, w, h, pitch);
        }
    }
    printf("5. whole-image decode: %u of %u images differ from the per-texel decode\n", bad, n);
    CHECK(n > 100 && bad == 0);
    for (fmt = 0x05; fmt <= 0x06; fmt++) {      /* SZ_R5G6B5, SZ_A8R8G8B8 */
        static uint32_t out[512 * 512];
        Nv2aTexture t;
        memset(&t, 0, sizeof t);
        t.offset = 0;
        t.width = t.height = 512;
        t.pitch = 2048;
        t.color = fmt;
        t.levels = 1;
        c0 = clock();
        for (i = 0; i < 20; i++)
            CHECK(nv2a_backend_decode_texture(&t, out));
        ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC / 20.0;
        printf("5. 512x512 %s decode: %.3f ms\n", nv2a_tex_format(fmt)->name, ms);
    }
}

/* Mip levels (XBA-120): one after another, each in the base layout. */
static void check_levels(void)
{
    Nv2aTexture t, l;
    memset(&t, 0, sizeof t);
    t.offset = 0x10000;
    t.width = t.height = 16;
    t.color = 0x06;                          /* SZ_A8R8G8B8 */
    t.levels = 5;
    CHECK(nv2a_backend_texture_level(&t, 2, &l) && l.offset == 0x10000 + 1024 + 256
          && l.width == 4 && l.height == 4 && l.levels == 1);
    CHECK(nv2a_backend_texture_level(&t, 4, &l) && l.width == 1 && l.offset == 0x10000 + 1360);
    CHECK(!nv2a_backend_texture_level(&t, 5, &l));
    t.color = 0x0C;                          /* DXT1: 8 bytes a 4x4 block */
    t.width = 16; t.height = 8;
    CHECK(nv2a_backend_texture_level(&t, 1, &l) && l.offset == 0x10000 + 64
          && l.width == 8 && l.height == 4);
    CHECK(nv2a_backend_texture_level(&t, 3, &l) && l.offset == 0x10000 + 64 + 16 + 8
          && l.width == 2 && l.height == 1);
    t.color = 0x12;                          /* linear: level 0 only */
    t.pitch = 64;
    CHECK(nv2a_backend_texture_level(&t, 0, &l) && !nv2a_backend_texture_level(&t, 1, &l));
    t.color = 0x28; t.width = t.height = 4; t.levels = 2;   /* SZ_G8B8: 2 bytes */
    CHECK(nv2a_backend_texture_level(&t, 1, &l) && l.offset == 0x10000 + 32);
    /* Volumes: xemu's 3D Morton order takes one bit of x, y, z in turn
     * while each axis has bits left; levels halve all three sizes. */
    CHECK(xbox_swizzle_offset_3d(1, 0, 0, 4, 4, 4) == 1 && xbox_swizzle_offset_3d(0, 1, 0, 4, 4, 4) == 2
          && xbox_swizzle_offset_3d(0, 0, 1, 4, 4, 4) == 4 && xbox_swizzle_offset_3d(2, 0, 0, 4, 4, 4) == 8);
    CHECK(xbox_swizzle_offset_3d(0, 1, 0, 4, 2, 1) == 2 && xbox_swizzle_offset_3d(2, 0, 0, 4, 2, 1) == 4);
    memset(&t, 0, sizeof t);
    t.offset = 0x20000;
    t.width = t.height = t.depth = 4;
    t.color = 0x06;
    t.levels = 3;
    CHECK(nv2a_backend_texture_bytes(&t) == (64 + 8 + 1) * 4);
    CHECK(nv2a_backend_texture_level(&t, 1, &l) && l.offset == 0x20000 + 256 && l.depth == 2
          && l.width == 2);
    {
        uint32_t x, y, z, out[64], bad = 0;
        for (z = 0; z < 4; z++)
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++) {
                    uint32_t v = 0xFF000000u | z << 16 | y << 8 | x;
                    memcpy(s_mem + 0x20000 + 4 * xbox_swizzle_offset_3d(x, y, z, 4, 4, 4), &v, 4);
                }
        t.levels = 1;
        CHECK(nv2a_backend_decode_texture(&t, out));
        for (z = 0; z < 4; z++)
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    bad += out[(z * 4 + y) * 4 + x] != (0xFF000000u | z << 16 | y << 8 | x);
        CHECK(bad == 0);
        t.color = 0x0C;                      /* DXT volumes: not decoded */
        CHECK(!nv2a_backend_decode_texture(&t, out));
    }
    printf("2b. mip levels, volumes: ok\n");
}

/* A fixed-function quad over a 64x64 surface (composite = viewport), 4 vertices
 * at 0x2000 (float3 position) with float2 texcoords at 0x2100. */
static void setup_ffp(void)
{
    static const float M[16] = { 32, 0, 0, 32, 0, -32, 0, 32, 0, 0, 1, 0, 0, 0, 0, 1 };
    static const float P[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    uint32_t i;

    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8 */
    m(0x0200, 64u << 16);
    m(0x0204, 64u << 16);
    m(0x020C, 256);
    m(0x0210, 0x80000);                      /* colour at 0x80000 */
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, M[i]);
    m(0x1E94, 0);                            /* fixed function */
    for (i = 0; i < 4; i++) {
        putf(0x2000 + 12 * i, P[i][0]);
        putf(0x2004 + 12 * i, P[i][1]);
        putf(0x2008 + 12 * i, 0.5f);
        putf(0x2100 + 8 * i, P[i][0] > 0);
        putf(0x2104 + 8 * i, P[i][1] > 0);
    }
    m(0x1720, 0x2000); m(0x1760, 2 | (3 << 4) | (12 << 8));     /* v0: float3 */
    m(0x1744, 0x2100); m(0x1784, 2 | (2 << 4) | (8 << 8));      /* v9: texcoord 0, float2 */
}

static void draw(uint32_t prim, uint32_t n)
{
    m(0x17FC, prim);
    m(0x1810, (n - 1) << 24);
    m(0x17FC, 0);
}

/* A swizzled 8x8 texture on stage 0 of format fmt, dimensionality dims. */
static void texture0(uint32_t fmt, uint32_t dims)
{
    m(0x1B00, 0x40000);
    m(0x1B04, (fmt << 8) | (dims << 4) | (1u << 16) | (3u << 20) | (3u << 24) | (dims == 3 ? 1u << 28 : 0));
    m(0x1B08, 0x0303);
    m(0x1B0C, 1u << 30);
}

int main(void)
{
    char path[300];
    const char *tmp = getenv("TMPDIR");
    char *l;

    snprintf(path, sizeof path, "%s/xba_cov_%d.jsonl", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    remove(path);
    set_env("RECOMP_STATUS", path);
    recomp_status_reset();

    check_format_table();
    check_swizzle_classifier();
    check_method_names();
    check_levels();
    check_new_formats();
    check_decode_walk();

    /* 3. Gaps, on the CPU rasteriser. */
    setup_ffp();
    texture0(0x06, 2);                        /* SZ_A8R8G8B8: decoded */
    draw(8, 4);                               /* QUADS */
    CHECK(nv2a_gap_count("texture format 0x06 SZ_A8R8G8B8") == 0);
    texture0(0x26, 2);                        /* LU_IMAGE_A8CR8CB8Y8: decoded (XBA-311) */
    m(0x1B10, 32u << 16);                     /* CONTROL1: pitch (linear) */
    m(0x1B1C, (8u << 16) | 8u);               /* IMAGE_RECT 8x8 */
    draw(8, 4);
    CHECK(nv2a_gap_count("texture format 0x26 LU_IMAGE_A8CR8CB8Y8") == 0);
    texture0(0x2F, 2);                        /* LU_IMAGE_DEPTH_X8_Y24_FLOAT: nothing decodes it */
    m(0x1B10, 32u << 16);
    m(0x1B1C, (8u << 16) | 8u);
    draw(8, 4);
    draw(8, 4);
    CHECK(nv2a_gap_count("texture format 0x2F LU_IMAGE_DEPTH_X8_Y24_FLOAT") == 2);
    texture0(0x06, 3);                        /* a volume texture */
    draw(8, 4);
    CHECK(nv2a_gap_count("volume texture") == 1);
    texture0(0x06, 2);
    draw(1, 4);                               /* POINTS */
    draw(3, 4);                               /* LINE_LOOP */
    CHECK(nv2a_gap_count("points") == 1 && nv2a_gap_count("line loop") == 1);
    m(0x17BC, 1);                             /* SET_LOGIC_OP_ENABLE */
    m(0x17C0, 0x1506);                        /* SET_LOGIC_OP: GL_XOR */
    draw(8, 4);
    m(0x17BC, 0);
    CHECK(nv2a_gap_count("logic op") == 1);
    m(0x03C0, 0x2402);                        /* SET_TEXGEN_S: sphere map (modelled) */
    draw(8, 4);
    m(0x03C0, 0);
    CHECK(nv2a_gap_count("fixed-function texgen mode") == 0);
    m(0x03C8, 0x2402);                        /* SET_TEXGEN_R: sphere map, which R has not */
    draw(8, 4);
    m(0x03C8, 0);
    CHECK(nv2a_gap_count("fixed-function texgen mode") == 1);
    /* Combiners with the stage modes XBA-311 models: none is counted. */
    m(0x1E60, 0x00001001u);                   /* COMBINER_CONTROL: 1 stage */
    m(0x0288, 0x0000000Cu);
    m(0x1E70, 0x1u | (0x11u << 5) | (0x09u << 10));  /* 2D, DOTPRODUCT, DOT_ST */
    draw(8, 4);
    m(0x1E70, 0x1u | (0x11u << 5) | (0x0Bu << 10) | (0x0Cu << 15));  /* reflections */
    draw(8, 4);
    m(0x1E70, 0x1u | (0x0Fu << 5) | (0x10u << 10) | (0x06u << 15));  /* AR, GB, bump-env */
    draw(8, 4);
    CHECK(nv2a_gap_count("texture stage mode 0x11 DOTPRODUCT") == 0);
    CHECK(nv2a_gap_count("texture stage mode 0x09 DOT_ST") == 0);
    CHECK(nv2a_gap_count("texture stage mode 0x0C DOT_RFLCT_SPEC") == 0);
    CHECK(nv2a_gap_count("texture stage mode 0x0F DPNDNT_AR") == 0);
    CHECK(nv2a_gap_count("texture stage mode 0x06 BUMPENVMAP") == 0);
    /* Still counted: BRDF, a mode on a stage it is not valid on (DOT_ST
     * on stage 1, bump-env on stage 0), DOT_ZW's depth on the CPU. */
    m(0x1E70, 0x1u | (0x11u << 5) | (0x08u << 10));
    draw(8, 4);
    CHECK(nv2a_gap_count("texture stage mode 0x08 BRDF") == 1);
    m(0x1E70, 0x1u | (0x09u << 5));
    draw(8, 4);
    CHECK(nv2a_gap_count("texture stage mode 0x09 DOT_ST") == 1);
    m(0x1E70, 0x06u);
    draw(8, 4);
    CHECK(nv2a_gap_count("texture stage mode 0x06 BUMPENVMAP") == 1);
    m(0x1E70, 0x1u | (0x11u << 5) | (0x0Au << 10));
    draw(8, 4);
    CHECK(nv2a_gap_count("dot-product depth replace (CPU rasteriser)") == 1);
    nv2a_backend_register(&s_backend_px);     /* a back end with the pixel state */
    draw(8, 4);
    CHECK(s_be.draws == 1);
    CHECK(nv2a_gap_count("dot-product depth replace (CPU rasteriser)") == 1);
    nv2a_backend_register(NULL);
    m(0x1E70, 0x1u);

    recomp_status_write_now();
    l = last_line(path);
    printf("3. status line: %s", l);
    CHECK(strstr(l, "\"points\":1") != NULL);
    CHECK(strstr(l, "\"texture format 0x2F LU_IMAGE_DEPTH_X8_Y24_FLOAT\":2") != NULL);
    CHECK(strstr(l, "\"volume texture\":1") != NULL);
    CHECK(strstr(l, "\"logic op\":1") != NULL);

    /* 4. The report: unsupported line, and unmodelled methods by name. */
    nv2a_pb_exec_report();
    recomp_status_write_now();
    l = last_line(path);
    CHECK(strstr(l, "\"unmodelled\":{") != NULL);
    CHECK(strstr(l, "\"SET_LOGIC_OP\":1") != NULL);
    CHECK(strstr(l, "\"SET_LOGIC_OP_ENABLE\":2") != NULL);
    CHECK(strstr(l, "SET_SURFACE_FORMAT") == NULL);    /* modelled */

    recomp_status_reset();
    remove(path);
    if (s_fail)
        return 1;
    printf("nv2a coverage: ok\n");
    return 0;
}
