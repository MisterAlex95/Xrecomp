/* NV2A coverage on the GL back end (XBA-120): pushbuffer methods into the
 * real executor (nv2a_pb_exec.c, linked), which hands its batches to the
 * real GL back end (src/gpu/nv2a_gl.c, included so the checks can read its
 * statistics and call its present hook), over a fake guest memory; the
 * pixels are read back into guest memory and checked. Built for desktop GL
 * and for the GLES 3.0 path Android runs (NV2A_GL_FORCE_ES); on Linux both
 * run on Mesa's llvmpipe through EGL_PLATFORM=surfaceless. Exit 77 without
 * a GL context.
 *
 * Points and lines:
 *   1. a screen-space line strip (no matrix, positions in pixels);
 *   2. fixed-function points of SET_POINT_SIZE 4 (6.3 fixed point: 32), each
 *      a 4x4 square of its vertex colour;
 *   3. a fixed-function line loop: its closing segment is drawn;
 *   4. a point sprite: a 2x2 texture over an 8x8 point, texel (0,0) in the
 *      point's top left quarter;
 *   5. vertex-program points and lines (the GLSL vertex program path);
 *   6. none of it is counted as unsupported; the CPU rasteriser draws no
 *      point and counts one.
 *
 * Mip chains:
 *   7. a 16x16 texture of five one-colour levels over 4x4 pixels: level 2
 *      with BOX_NEARESTLOD, level 3 with a LOD bias of +1, level 1 with the
 *      LOD clamped to 1, half level 2 half level 3 trilinear at LOD 2.5,
 *      level 0 with a LOD0 filter and with RECOMP_GPU_MIPS=0. */
#include "../../src/gpu/nv2a_gl.c"
#include "nv2a_coverage.h"

#define MEM_BYTES (8u << 20)
#define FB      0x00100000u         /* 64x64 A8R8G8B8, pitch 256 */
#define VB      0x00200000u         /* vertices */
#define TEX     0x00300000u         /* textures */
#define W 64
#define H 64

static uint8_t *s_mem;
static int s_fail;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
const char *xbox_path_save_dir_utf8(void) { return NULL; }
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

/* ---- driving the executor ---------------------------------------------- */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

static uint32_t px(uint32_t x, uint32_t y)
{
    uint32_t v;
    memcpy(&v, s_mem + FB + (size_t)y * 256 + x * 4u, 4);
    return v & 0x00FFFFFFu;
}

/* Pixels of colour c (RGB) in [x0,x1)x[y0,y1). */
static uint32_t count(uint32_t c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint32_t x, y, n = 0;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            n += px(x, y) == c;
    return n;
}

/* The surface back into guest memory, now (the flip's sync present). */
static void readback(void)
{
    Nv2aPresentSurface now, ready;
    memset(&now, 0, sizeof now);
    now.va = FB;
    now.pitch = 256;
    now.bpp = 4;
    now.format = 0x0008;
    now.w = W;
    now.h = H;
    gl_present(&now, 1, &ready);
    gl_flip();
}

static void clear_black(void)
{
    m(0x1D90, 0xFF000000u);                  /* SET_COLOR_CLEAR_VALUE */
    m(0x1D94, 0xF0);                         /* CLEAR_SURFACE: colour */
}

static void surface(void)
{
    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, linear */
    m(0x0200, (uint32_t)W << 16);
    m(0x0204, (uint32_t)H << 16);
    m(0x020C, 256);
    m(0x0210, FB);
}

/* Fixed function: composite = the 64x64 viewport, y down. */
static void ffp(void)
{
    static const float M[16] = { 32, 0, 0, 32, 0, -32, 0, 32, 0, 0, 1, 0, 0, 0, 0, 1 };
    uint32_t i;
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, M[i]);
    m(0x1E94, 0);
}

/* Vertex i at VB + 32 i: float3 position, D3DCOLOR, float2 texcoord. */
static void vtx(uint32_t i, float x, float y, float z, uint32_t argb, float u, float v)
{
    uint32_t at = VB + 32 * i;
    putf(at, x); putf(at + 4, y); putf(at + 8, z);
    put32(at + 12, argb);
    putf(at + 16, u); putf(at + 20, v);
}

static void arrays(void)
{
    uint32_t a;
    for (a = 0; a < 16; a++)
        m(0x1760 + 4 * a, 2);                /* size 0: off */
    m(0x1720, VB);      m(0x1760, 2 | (3 << 4) | (32 << 8));        /* v0: float3 */
    m(0x172C, VB + 12); m(0x176C, 0 | (4 << 4) | (32 << 8));        /* v3: D3DCOLOR */
    m(0x1744, VB + 16); m(0x1784, 2 | (2 << 4) | (32 << 8));        /* v9: float2 */
}

static void draw(uint32_t prim, uint32_t first, uint32_t n)
{
    m(0x17FC, prim);
    m(0x1810, first | ((n - 1) << 24));
    m(0x17FC, 0);
}

/* NDC of pixel position (x, y) on the 64x64 viewport above. */
static float nx(float x) { return x / 32.0f - 1.0f; }
static float ny(float y) { return 1.0f - y / 32.0f; }

int main(void)
{
    uint32_t n;

    s_mem = (uint8_t *)calloc(1, MEM_BYTES);
    setenv("RECOMP_GPU", "1", 1);
    if (!nv2a_gl_install_from_env()) {
        fprintf(stderr, "back end not registered\n");
        return 1;
    }
    surface();
    m(0x0358, 0x01010101u);                  /* SET_COLOR_MASK: all */
    clear_black();
    if (s_init <= 0) {
        puts("no GL context: GL coverage checks skipped");
        return 77;
    }
    printf("GL: %s | %s (%s path), points to %.0f px, lines to %.0f px\n",
           (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION),
           NV2A_GL_ES ? "GLES" : "desktop GL", (double)s_point_max, (double)s_line_max);
    arrays();

    /* 1. Screen space (no composite matrix yet): a line strip along row 10
     * (y 10.5, pixel centres) then down column 40. */
    vtx(0, 4.0f, 10.5f, 0, 0xFFFF0000u, 0, 0);
    vtx(1, 40.5f, 10.5f, 0, 0xFFFF0000u, 0, 0);
    vtx(2, 40.5f, 50.0f, 0, 0xFFFF0000u, 0, 0);
    draw(4, 0, 3);                           /* LINE_STRIP */
    readback();
    n = count(0xFF0000u, 6, 10, 38, 11);
    printf("1. screen-space line strip: %u of 32 row pixels, %u of 36 column pixels\n",
           n, count(0xFF0000u, 40, 12, 41, 48));
    CHECK(n == 32);
    CHECK(count(0xFF0000u, 40, 12, 41, 48) == 36);
    CHECK(count(0xFF0000u, 0, 20, 30, 64) == 0);

    /* 2. Fixed-function points, size 4: corners on whole pixels, so each
     * covers exactly 4x4. */
    surface();
    clear_black();
    ffp();
    m(0x043C, 32);                           /* SET_POINT_SIZE 4.0 */
    vtx(0, nx(10), ny(10), 0.5f, 0xFF00FF00u, 0, 0);
    vtx(1, nx(30), ny(20), 0.5f, 0xFF0000FFu, 0, 0);
    vtx(2, nx(50), ny(50), 0.5f, 0xFFFFFF00u, 0, 0);
    draw(1, 0, 3);                           /* POINTS */
    readback();
    printf("2. fixed-function points: %u, %u, %u pixels\n", count(0x00FF00u, 0, 0, W, H),
           count(0x0000FFu, 0, 0, W, H), count(0xFFFF00u, 0, 0, W, H));
    CHECK(count(0x00FF00u, 8, 8, 12, 12) == 16 && count(0x00FF00u, 0, 0, W, H) == 16);
    CHECK(count(0x0000FFu, 28, 18, 32, 22) == 16 && count(0x0000FFu, 0, 0, W, H) == 16);
    CHECK(count(0xFFFF00u, 48, 48, 52, 52) == 16);

    /* 3. A fixed-function line loop: a square whose closing (left) edge is
     * the loop's last segment. */
    clear_black();
    vtx(0, nx(20.5f), ny(20.5f), 0.5f, 0xFFFFFFFFu, 0, 0);
    vtx(1, nx(40.5f), ny(20.5f), 0.5f, 0xFFFFFFFFu, 0, 0);
    vtx(2, nx(40.5f), ny(40.5f), 0.5f, 0xFFFFFFFFu, 0, 0);
    vtx(3, nx(20.5f), ny(40.5f), 0.5f, 0xFFFFFFFFu, 0, 0);
    draw(3, 0, 4);                           /* LINE_LOOP */
    readback();
    n = count(0xFFFFFFu, 20, 22, 21, 39);
    printf("3. fixed-function line loop: closing edge %u of 17 pixels, %u in all\n", n,
           count(0xFFFFFFu, 0, 0, W, H));
    CHECK(n == 17);
    CHECK(count(0xFFFFFFu, 22, 20, 39, 21) == 17);   /* the first segment */
    CHECK(count(0xFFFFFFu, 22, 22, 39, 39) == 0);    /* inside: empty */

    /* 4. A point sprite: an 8x8 point at (32, 32) over a 2x2 linear
     * A8R8G8B8 texture (red, green / blue, white), nearest. */
    clear_black();
    put32(TEX + 0, 0xFFFF0000u); put32(TEX + 4, 0xFF00FF00u);
    put32(TEX + 8, 0xFF0000FFu); put32(TEX + 12, 0xFFFFFFFFu);
    m(0x1B00, TEX);
    m(0x1B04, (0x12u << 8) | (2u << 4) | (1u << 16));   /* LU_IMAGE_A8R8G8B8, 2D, 1 level */
    m(0x1B08, 0x0303);
    m(0x1B0C, 1u << 30);
    m(0x1B10, 8u << 16);                     /* pitch 8 */
    m(0x1B14, (1u << 16) | (1u << 24));      /* nearest */
    m(0x1B1C, (2u << 16) | 2u);
    m(0x043C, 64);                           /* size 8 */
    m(0x031C, 1);                            /* SET_POINT_SMOOTH_ENABLE: sprites */
    vtx(0, nx(32), ny(32), 0.5f, 0xFFFFFFFFu, 0, 0);
    draw(1, 0, 1);
    readback();
    printf("4. point sprite: quarters %06X %06X / %06X %06X\n", px(29, 29), px(34, 29),
           px(29, 34), px(34, 34));
    CHECK(count(0xFF0000u, 28, 28, 32, 32) == 16 && count(0x00FF00u, 32, 28, 36, 32) == 16);
    CHECK(count(0x0000FFu, 28, 32, 32, 36) == 16 && count(0xFFFFFFu, 32, 32, 36, 36) == 16);
    m(0x031C, 0);
    m(0x1B0C, 0);
    m(0x1B00, 0);                            /* no texture */

    /* 5. A vertex program (mov oPos, v0; mov oD0, v3): positions in pixels,
     * through the GLSL vertex program path. */
    {
        static const uint32_t prog[2][4] = {
            { 0, (1u << 21) | (0u << 9) | 0x1Bu, 2u << 26, (0xFu << 12) | (1u << 11) | (0u << 3) },
            { 0, (1u << 21) | (3u << 9) | 0x1Bu, 2u << 26, (0xFu << 12) | (1u << 11) | (3u << 3) | 1u },
        };
        uint32_t draws0 = s_st.vsh_draws, k, w;
        m(0x1E9C, 0);
        for (k = 0; k < 2; k++)
            for (w = 0; w < 4; w++)
                m(0x0B00 + 4 * w, prog[k][w]);
        m(0x1EA0, 0);
        m(0x1E94, 2);                        /* program */
        clear_black();
        m(0x043C, 16);                       /* size 2 */
        vtx(0, 10, 50, 100, 0xFF00FFFFu, 0, 0);
        vtx(1, 12.5f, 30.5f, 100, 0xFFFF00FFu, 0, 0);
        vtx(2, 52.5f, 30.5f, 100, 0xFFFF00FFu, 0, 0);
        draw(1, 0, 1);                       /* POINTS: one */
        draw(2, 1, 2);                       /* LINES: one segment */
        readback();
        printf("5. vertex-program point %u pixels, line %u of 38 pixels (%u GLSL draws)\n",
               count(0x00FFFFu, 0, 0, W, H), count(0xFF00FFu, 14, 30, 52, 31),
               s_st.vsh_draws - draws0);
        CHECK(count(0x00FFFFu, 9, 49, 11, 51) == 4 && count(0x00FFFFu, 0, 0, W, H) == 4);
        CHECK(count(0xFF00FFu, 14, 30, 52, 31) == 38);
        m(0x1E94, 0);
    }

    /* 7. A mip chain: a 16x16 swizzled A8R8G8B8 texture with 5 levels, each
     * level one colour, drawn over 4x4 pixels (LOD 2). */
    {
        static const uint32_t col[5] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
                                         0xFFFFFF00u, 0xFFFFFFFFu };
        uint32_t lv, off = 0, sz = 16, i, c[7];
        for (lv = 0; lv < 5; lv++, sz >>= 1)
            for (i = 0; i < sz * sz; i++, off += 4)
                put32(TEX + off, col[lv]);
        vtx(0, nx(8), ny(8), 0.5f, 0xFFFFFFFFu, 0, 0);
        vtx(1, nx(12), ny(8), 0.5f, 0xFFFFFFFFu, 1, 0);
        vtx(2, nx(12), ny(12), 0.5f, 0xFFFFFFFFu, 1, 1);
        vtx(3, nx(8), ny(12), 0.5f, 0xFFFFFFFFu, 0, 1);
        m(0x1B00, TEX);
        m(0x1B04, (0x06u << 8) | (2u << 4) | (5u << 16) | (4u << 20) | (4u << 24));
        m(0x1B08, 0x0303);
        m(0x1B0C, (1u << 30) | ((4u << 8) << 6));          /* LOD 0 .. 4 */
        m(0x1B14, (3u << 16) | (1u << 24));                 /* BOX_NEARESTLOD */
        clear_black();
        draw(8, 0, 4);                                      /* QUADS */
        readback();
        c[0] = px(10, 10);
        m(0x1B14, (3u << 16) | (1u << 24) | 256u);          /* LOD bias +1 */
        draw(8, 0, 4);
        readback();
        c[1] = px(10, 10);
        m(0x1B14, (3u << 16) | (1u << 24));
        m(0x1B0C, (1u << 30) | ((1u << 8) << 6));          /* LOD clamped to 0 .. 1 */
        draw(8, 0, 4);
        readback();
        c[2] = px(10, 10);
        m(0x1B0C, (1u << 30) | ((4u << 8) << 6));
        m(0x1B14, (6u << 16) | (2u << 24));                 /* TENT_TENT_LOD: trilinear */
        draw(8, 0, 4);
        readback();
        c[3] = px(10, 10);
        m(0x1B14, (6u << 16) | (2u << 24) | 128u);          /* trilinear, LOD 2.5 */
        draw(8, 0, 4);
        readback();
        c[6] = px(10, 10);
        m(0x1B14, (1u << 16) | (1u << 24));                 /* BOX_LOD0 */
        draw(8, 0, 4);
        readback();
        c[4] = px(10, 10);
        /* RECOMP_GPU_MIPS=0: level 0 whatever the filter. */
        s_mips = 0;
        tex_drop_all();
        m(0x1B14, (3u << 16) | (1u << 24));
        draw(8, 0, 4);
        readback();
        c[5] = px(10, 10);
        s_mips = 1;
        tex_drop_all();
        printf("7. mip chain: LOD 2 %06X, bias +1 %06X, clamp 1 %06X, trilinear %06X,"
               " LOD0 filter %06X, mips off %06X; trilinear at LOD 2.5 %06X\n",
               c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
        CHECK(c[0] == 0x0000FFu && c[1] == 0xFFFF00u && c[2] == 0x00FF00u);
        CHECK(c[3] == 0x0000FFu);               /* exactly LOD 2: level 2 alone */
        CHECK(c[4] == 0xFF0000u && c[5] == 0xFF0000u);
        /* Half blue (level 2), half yellow (level 3). */
        CHECK((c[6] >> 16) >= 0x78 && (c[6] >> 16) <= 0x88 && ((c[6] >> 8) & 0xFF) >= 0x78
              && ((c[6] >> 8) & 0xFF) <= 0x88 && (c[6] & 0xFF) >= 0x78 && (c[6] & 0xFF) <= 0x88);
        m(0x1B0C, 0);
        m(0x1B00, 0);
    }

    /* 6. Nothing counted as unsupported; the CPU rasteriser draws none and
     * counts. */
    CHECK(nv2a_gap_count("points") == 0 && nv2a_gap_count("lines") == 0
          && nv2a_gap_count("line strip") == 0 && nv2a_gap_count("line loop") == 0);
    nv2a_backend_register(NULL);
    draw(1, 0, 3);
    CHECK(nv2a_gap_count("points") == 1);
    nv2a_gl_install_from_env();
    printf("6. gaps: points on the GPU 0, on the CPU rasteriser %llu\n",
           (unsigned long long)nv2a_gap_count("points"));

    CHECK(glGetError() == GL_NO_ERROR);
    if (!s_fail)
        printf("GL coverage (%s): ok\n", NV2A_GL_ES ? "GLES" : "GL");
    return s_fail;
}
