/* Internal resolution on the GL back end, end to end (XBA-237): the same
 * pushbuffer scene drawn into a 640x480 frame-buffer surface at 1x, 2x and
 * 4x, through the real executor (nv2a_pb_exec.c, linked) and the real GL
 * back end (src/gpu/nv2a_gl.c, included so the checks can set its scale
 * and call its present hook), over a fake guest memory. Built for desktop
 * GL and for the GLES 3.0 path Android runs (NV2A_GL_FORCE_ES); on Linux
 * both run on Mesa's llvmpipe through EGL_PLATFORM=surfaceless. Exit 77
 * without a GL context.
 *
 * The scale comes from RECOMP_UPSCALE=2 (the setting every console shares,
 * Settings > Graphics > Internal resolution) with RECOMP_GPU_SCALE unset:
 *   1. the back end starts at 2 from it;
 * then each pass sets the back end's scale and draws, on a surface of its
 * own (a scaled target's scale sticks to its memory): a clear, a clear
 * of a clip rectangle, a smooth-shaded triangle, a quad under a surface clip (the
 * scissor), points of size 4 and a line strip, all in screen space;
 *   2. the host frame (what the window shows) is 640n x 480n;
 *   3. that frame averaged down n x n matches the 1x picture within a small
 *      tolerance (edge pixels only differ);
 *   4. points cover n^2 times the pixels, and lines are n pixels thick
 *      where the context offers wide lines (desktop core GL: 1);
 *   5. what the guest reads back (the flip's sync read-back into its
 *      memory) is 640x480 at its own pitch, matches the 1x read-back within
 *      the same tolerance, and leaves the bytes after the surface alone. */
#include "../../src/gpu/nv2a_gl.c"
#include <math.h>

#define MEM_BYTES (16u << 20)
#define VB      0x00080000u         /* vertices */
#define FB1     0x00100000u         /* 640x480 A8R8G8B8, pitch 2560: 1x */
#define FB2     0x00400000u         /* 2x */
#define FB4     0x00700000u         /* 4x */
#define W 640
#define H 480
#define PITCH 2560u
#define FB_BYTES (PITCH * H)
#define SENTINEL 0x5Au

static uint8_t *s_mem;
static int s_fail;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
const char *xbox_path_save_dir_utf8(void) { return NULL; }
void xbox_HostThreadFloat(const char *role) { (void)role; }   /* the shader worker's placement */
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
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

static void surface(uint32_t va, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, linear */
    m(0x0200, x | w << 16);                  /* SET_SURFACE_CLIP_HORIZONTAL */
    m(0x0204, y | h << 16);
    m(0x020C, PITCH);
    m(0x0210, va);
}

/* A clear of a linear surface covers its clip. */
static void clear(uint32_t argb)
{
    m(0x1D90, argb);                         /* SET_COLOR_CLEAR_VALUE */
    m(0x1D94, 0xF0);                         /* CLEAR_SURFACE: colour */
}

/* Vertex i at VB + 32 i: float3 position (screen pixels), D3DCOLOR. */
static void vtx(uint32_t i, float x, float y, uint32_t argb)
{
    uint32_t at = VB + 32 * i;
    putf(at, x); putf(at + 4, y); putf(at + 8, 0.0f);
    put32(at + 12, argb);
}

static void arrays(void)
{
    uint32_t a;
    for (a = 0; a < 16; a++)
        m(0x1760 + 4 * a, 2);                /* size 0: off */
    m(0x1720, VB);      m(0x1760, 2 | (3 << 4) | (32 << 8));        /* v0: float3 */
    m(0x172C, VB + 12); m(0x176C, 0 | (4 << 4) | (32 << 8));        /* v3: D3DCOLOR */
}

static void draw(uint32_t prim, uint32_t first, uint32_t n)
{
    m(0x17FC, prim);
    m(0x1810, first | ((n - 1) << 24));
    m(0x17FC, 0);
}

/* The scene, into the 640x480 surface at va. */
static void scene(uint32_t va)
{
    surface(va, 0, 0, W, H);
    m(0x0358, 0x01010101u);                  /* SET_COLOR_MASK: all */
    clear(0xFF101010u);
    surface(va, 400, 40, 200, 100);          /* a clear of a clip rectangle */
    clear(0xFF2060A0u);
    surface(va, 0, 0, W, H);
    arrays();
    /* A smooth-shaded triangle with edges off the pixel grid. */
    vtx(0, 40.3f, 30.7f, 0xFFFF0000u);
    vtx(1, 350.1f, 90.2f, 0xFF00FF00u);
    vtx(2, 120.6f, 420.9f, 0xFF0000FFu);
    draw(5, 0, 3);                           /* TRIANGLES */
    /* A quad over most of the surface under a clip of x 420..620, y 200..330:
     * only the clip (the scissor) is drawn. */
    surface(va, 420, 200, 200, 130);
    vtx(3, 300.0f, 160.0f, 0xFFFFFF00u);
    vtx(4, 639.0f, 160.0f, 0xFFFFFF00u);
    vtx(5, 639.0f, 470.0f, 0xFF00FFFFu);
    vtx(6, 300.0f, 470.0f, 0xFF00FFFFu);
    draw(8, 3, 4);                           /* QUADS */
    surface(va, 0, 0, W, H);
    /* Points of size 4 on whole-pixel corners: 16 guest pixels each. */
    m(0x043C, 32);                           /* SET_POINT_SIZE 4.0 (6.3 fixed point) */
    vtx(7, 402.0f, 402.0f, 0xFFFF00FFu);
    vtx(8, 442.0f, 402.0f, 0xFFFF00FFu);
    vtx(9, 482.0f, 402.0f, 0xFFFF00FFu);
    draw(1, 7, 3);                           /* POINTS */
    m(0x043C, 8);                            /* back to 1.0 */
    /* A horizontal line strip along row 450 (pixel centres). */
    vtx(10, 40.0f, 450.5f, 0xFFFFFFFFu);
    vtx(11, 360.0f, 450.5f, 0xFFFFFFFFu);
    draw(4, 10, 2);                          /* LINE_STRIP */
}

/* The flip's sync read-back of the 640x480 surface at va into guest memory. */
static void readback(uint32_t va)
{
    Nv2aPresentSurface now, ready;
    memset(&now, 0, sizeof now);
    now.va = va;
    now.pitch = PITCH;
    now.bpp = 4;
    now.format = 0x0008;
    now.w = W;
    now.h = H;
    gl_present(&now, 1, &ready);
    gl_flip();
}

/* The host frame of the surface at va (async present: read at one flip,
 * handed over at the next), as 0x00RRGGBB words in out (w x h, at most
 * 2560x1920). Returns 1 when one came. */
static uint32_t *s_host;
static int host_frame(uint32_t va, uint32_t *w, uint32_t *h)
{
    Nv2aPresentSurface now, ready;
    int i, got = 0;
    memset(&now, 0, sizeof now);
    now.va = va;
    now.pitch = PITCH;
    now.bpp = 4;
    now.format = 0x0008;
    now.w = W;
    now.h = H;
    for (i = 0; i < 3 && !got; i++) {
        memset(&ready, 0, sizeof ready);
        if (gl_present(&now, 0, &ready) && ready.host_px && ready.host_w <= 2560
            && ready.host_h <= 1920) {
            uint32_t x, y;
            int rgba = ready.host_format == NV2A_HOST_FORMAT_RGBX8888;
            for (y = 0; y < ready.host_h; y++) {
                const uint8_t *p = (const uint8_t *)ready.host_px + (size_t)y * ready.host_pitch;
                for (x = 0; x < ready.host_w; x++, p += 4)
                    s_host[(size_t)y * ready.host_w + x] =
                        rgba ? ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2])
                             : ((uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
            }
            *w = ready.host_w;
            *h = ready.host_h;
            got = 1;
        }
        gl_flip();
    }
    return got;
}

static uint32_t guest_px(uint32_t va, uint32_t x, uint32_t y)
{
    uint32_t v;
    memcpy(&v, s_mem + va + (size_t)y * PITCH + x * 4u, 4);
    return v & 0x00FFFFFFu;
}

static int chan_diff(uint32_t a, uint32_t b)
{
    int d = 0, s;
    for (s = 0; s < 24; s += 8) {
        int e = abs((int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF));
        if (e > d)
            d = e;
    }
    return d;
}

/* The 1x guest picture, and how a picture compares with it. */
static uint32_t s_ref[W * H];
typedef struct { uint32_t off; double mean; } Cmp;

/* Pixels of 1x guest picture ref that differ by more than 24 in a channel
 * from px(x, y), and the mean channel difference. Rows 440..460 (the line)
 * are left to check 4. */
static Cmp compare(uint32_t (*px)(void *, uint32_t, uint32_t), void *ctx)
{
    Cmp c = { 0, 0.0 };
    uint32_t x, y, n = 0;
    double sum = 0.0;
    for (y = 0; y < H; y++) {
        if (y >= 440 && y < 460)
            continue;
        for (x = 0; x < W; x++) {
            int d = chan_diff(s_ref[y * W + x], px(ctx, x, y));
            c.off += d > 24;
            sum += d;
            n++;
        }
    }
    c.mean = sum / n;
    return c;
}

typedef struct { uint32_t va; } GuestCtx;
static uint32_t guest_get(void *ctx, uint32_t x, uint32_t y)
{
    return guest_px(((GuestCtx *)ctx)->va, x, y);
}

/* The host frame averaged down n x n: guest pixel (x, y). */
typedef struct { uint32_t w, n; } HostCtx;
static uint32_t host_down(void *ctx, uint32_t x, uint32_t y)
{
    const HostCtx *h = (const HostCtx *)ctx;
    uint32_t i, j, r = 0, g = 0, b = 0, nn = h->n * h->n;
    for (j = 0; j < h->n; j++)
        for (i = 0; i < h->n; i++) {
            uint32_t v = s_host[(size_t)(y * h->n + j) * h->w + x * h->n + i];
            r += (v >> 16) & 0xFF;
            g += (v >> 8) & 0xFF;
            b += v & 0xFF;
        }
    return ((r + nn / 2) / nn) << 16 | ((g + nn / 2) / nn) << 8 | ((b + nn / 2) / nn);
}

/* Host pixels of colour c in guest rectangle [x0,x1)x[y0,y1) at scale n. */
static uint32_t host_count(uint32_t w, uint32_t n, uint32_t c, uint32_t x0, uint32_t y0,
                           uint32_t x1, uint32_t y1)
{
    uint32_t x, y, k = 0;
    for (y = y0 * n; y < y1 * n; y++)
        for (x = x0 * n; x < x1 * n; x++)
            k += s_host[(size_t)y * w + x] == c;
    return k;
}

static int sentinel_intact(uint32_t va)
{
    uint32_t i;
    for (i = 0; i < 4096; i++)
        if (s_mem[va + FB_BYTES + i] != SENTINEL)
            return 0;
    return 1;
}

int main(void)
{
    static const struct { uint32_t va; float sc; } pass[] = { { FB1, 1.0f }, { FB2, 2.0f },
                                                              { FB4, 4.0f } };
    uint32_t p, ref_points = 0;

    s_mem = (uint8_t *)calloc(1, MEM_BYTES);
    s_host = (uint32_t *)calloc((size_t)2560 * 1920, 4);
    for (p = 0; p < 3; p++)
        memset(s_mem + pass[p].va + FB_BYTES, SENTINEL, 4096);
    setenv("RECOMP_GPU", "1", 1);
    unsetenv("RECOMP_GPU_SCALE");
    setenv("RECOMP_UPSCALE", "2", 1);
    setenv("RECOMP_GPU_PRESENT", "host", 1); /* host frames at 1x too */
    nv2a_gl_set_display_size(2560, 1920);    /* a window: host frames */
    if (!nv2a_gl_install_from_env()) {
        fprintf(stderr, "back end not registered\n");
        return 1;
    }
    surface(FB1, 0, 0, W, H);
    m(0x0358, 0x01010101u);
    clear(0xFF000000u);
    if (s_init <= 0) {
        puts("no GL context: GL upscale checks skipped");
        return 77;
    }
    printf("GL: %s | %s (%s path), points to %.0f px, lines to %.0f px\n",
           (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION),
           NV2A_GL_ES ? "GLES" : "desktop GL", (double)s_point_max, (double)s_line_max);

    /* 1. RECOMP_UPSCALE=2, RECOMP_GPU_SCALE unset: scale 2. */
    printf("1. RECOMP_UPSCALE=2: internal scale %.2f\n", (double)s_scale);
    CHECK(s_scale == 2.0f && s_scale_cfg.scale == 2.0f);

    for (p = 0; p < 3; p++) {
        uint32_t va = pass[p].va, n = (uint32_t)pass[p].sc, hw = 0, hh = 0, pts, thick, x, y;
        GuestCtx g = { va };
        HostCtx hc;
        Cmp gc, hcmp;
        float want_line;

        s_scale_cfg.scale = s_scale = pass[p].sc;
        scene(va);
        readback(va);
        if (p == 0) {
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++)
                    s_ref[y * W + x] = guest_px(FB1, x, y);
            ref_points = 0;
            for (y = 390; y < 420; y++)
                for (x = 390; x < 500; x++)
                    ref_points += guest_px(FB1, x, y) == 0xFF00FFu;
            printf("   1x reference: %u point pixels, triangle centre 0x%06X, clip 0x%06X,"
                   " outside 0x%06X\n", ref_points, guest_px(FB1, 170, 180),
                   guest_px(FB1, 500, 250), guest_px(FB1, 380, 250));
            CHECK(ref_points == 48);
            CHECK(guest_px(FB1, 500, 100) == 0x2060A0u);
            CHECK(guest_px(FB1, 500, 30) == 0x101010u && guest_px(FB1, 390, 100) == 0x101010u);
            CHECK(guest_px(FB1, 380, 250) == 0x101010u);          /* the quad's clipped part */
            CHECK(guest_px(FB1, 500, 250) != 0x101010u);          /* the quad inside the clip */
        }

        /* 5. The guest's read-back. */
        gc = compare(guest_get, &g);
        printf("%u. %ux read-back: %u of %u pixels off, mean difference %.2f, after the"
               " surface %s\n", 5, n, gc.off, W * H, gc.mean,
               sentinel_intact(va) ? "untouched" : "WRITTEN");
        CHECK(sentinel_intact(va));
        CHECK(gc.off <= 2000 && gc.mean < 1.5);
        if (n == 1)
            CHECK(gc.off == 0);

        /* 2. The host frame's size. */
        if (!host_frame(va, &hw, &hh)) {
            fprintf(stderr, "FAIL: no host frame at %ux\n", n);
            s_fail = 1;
            continue;
        }
        printf("2. %ux host frame: %ux%u\n", n, hw, hh);
        CHECK(hw == W * n && hh == H * n);
        if (hw != W * n || hh != H * n)
            continue;

        /* 3. Averaged down, the 1x picture. */
        hc.w = hw;
        hc.n = n;
        hcmp = compare(host_down, &hc);
        printf("3. %ux host frame averaged %ux%u: %u pixels off, mean difference %.2f\n", n, n,
               n, hcmp.off, hcmp.mean);
        CHECK(hcmp.off <= 2000 && hcmp.mean < 1.5);

        /* 4. Points and lines. */
        pts = host_count(hw, n, 0xFF00FFu, 390, 390, 500, 420);
        thick = 0;
        for (y = 440 * n; y < 460 * n; y++)
            thick += s_host[(size_t)y * hw + 200 * n] == 0xFFFFFFu;
        want_line = (float)n > s_line_max ? s_line_max : (float)n;
        printf("4. %ux: %u point pixels (want %u), line %u pixels thick (want %.0f)\n", n, pts,
               ref_points * n * n, thick, (double)want_line);
        CHECK(pts == ref_points * n * n);
        CHECK(thick == (uint32_t)want_line);
    }

    CHECK(glGetError() == GL_NO_ERROR);
    if (!s_fail)
        printf("GL upscale (%s): ok\n", NV2A_GL_ES ? "GLES" : "GL");
    return s_fail;
}
