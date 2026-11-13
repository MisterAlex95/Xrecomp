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
 *   3. a batch of points, a texture format nothing decodes, a volume
 *      texture, logic op, a dot-product stage mode and fixed-function texgen
 *      are each counted once per batch under their names, and reach the
 *      status file's "unsupported";
 *   4. the [GPU] report runs and the status file's "unmodelled" names the
 *      method nothing models (SET_LOGIC_OP).
 *
 * Guest memory is a buffer addressed by the offsets the title would use. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "nv2a_coverage.h"
#include "recomp_status.h"

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
    check_method_names();

    /* 3. Gaps, on the CPU rasteriser. */
    setup_ffp();
    texture0(0x06, 2);                        /* SZ_A8R8G8B8: decoded */
    draw(8, 4);                               /* QUADS */
    CHECK(nv2a_gap_count("texture format 0x06 SZ_A8R8G8B8") == 0);
    texture0(0x26, 2);                        /* LU_IMAGE_A8CR8CB8Y8: nothing decodes it */
    m(0x1B10, 32u << 16);                     /* CONTROL1: pitch (linear) */
    m(0x1B1C, (8u << 16) | 8u);               /* IMAGE_RECT 8x8 */
    draw(8, 4);
    draw(8, 4);
    CHECK(nv2a_gap_count("texture format 0x26 LU_IMAGE_A8CR8CB8Y8") == 2);
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
    m(0x03C0, 0x2402);                        /* SET_TEXGEN_S: sphere map */
    draw(8, 4);
    m(0x03C0, 0);
    CHECK(nv2a_gap_count("fixed-function texgen") == 1);
    /* Combiners with stage 1 in a dot-product mode. */
    m(0x1E60, 0x00001001u);                   /* COMBINER_CONTROL: 1 stage */
    m(0x1E70, 0x1u | (0x11u << 5));           /* stage 0 2D, stage 1 DOTPRODUCT */
    m(0x0288, 0x0000000Cu);
    draw(8, 4);
    CHECK(nv2a_gap_count("texture stage mode 0x11 DOTPRODUCT") == 1);
    /* Bump-env on stage 1: the CPU rasteriser reads zero; a back end with
     * the pixel state draws it. */
    m(0x1E70, 0x1u | (0x06u << 5));
    draw(8, 4);
    CHECK(nv2a_gap_count("texture stage mode 0x06 BUMPENVMAP") == 1);
    nv2a_backend_register(&s_backend_px);
    draw(8, 4);
    CHECK(s_be.draws == 1);
    CHECK(nv2a_gap_count("texture stage mode 0x06 BUMPENVMAP") == 1);
    nv2a_backend_register(NULL);
    m(0x1E70, 0x1u);

    recomp_status_write_now();
    l = last_line(path);
    printf("3. status line: %s", l);
    CHECK(strstr(l, "\"points\":1") != NULL);
    CHECK(strstr(l, "\"texture format 0x26 LU_IMAGE_A8CR8CB8Y8\":2") != NULL);
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
