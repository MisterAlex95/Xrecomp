/* xenos_draw_test DIR (XBA-270): the GLES renderer (xenos_gl.c) on a hand-built PM4 stream -- the first pixels.
 *
 * DIR holds make_draw.py's shaders (vs.bin, ps.bin, cache/). Guest memory is a big-endian array as the CPU sees it;
 * the packets load both shaders (IM_LOAD_IMMEDIATE), set fetch constant 0 and the pixel shader's c0 (type-0 register
 * writes), and draw:
 *   frame 0: a red triangle (auto indices, triangle list), c0 = 1: the centre is red, a corner stays clear;
 *   frame 1: a full-target quad from four green vertices and six 16-bit indices (DMA, 8-in-16 swap mode, the way a
 *            big-endian CPU writes them), c0 = (1, 0.5, 1, 1): every pixel is half green;
 *   frame 2: the same quad with the viewport scale / offset enabled for the target's top-left quarter only (the
 *            VTE register's z bits on too: all six, as Direct3D sets them);
 *   frame 3: depth: a green quad at z 0.25, then a red one at z 0.75 with the test "less": green stays;
 *   frame 4: blending: red, then green at alpha 0.5 with src alpha / one minus src alpha: (0.5, 0.5, 0);
 *   frame 5: culling of back faces: the red triangle (counter-clockwise on screen) is drawn with "front is CCW" and
 *            culled with "front is CW";
 *   frame 6: a quad list (one quad of four vertices) covers the target;
 *   (every frame uses Direct3D's full-target viewport but frame 2 and frame 24: with the viewport off, positions
 *   are pixels -- a screen-space quad from (16, 16) to (48, 48));
 *   frames 7-10, textures (ps_tex.bin: a 2D fetch from texture fetch constant 2 at the interpolated colour's xy, a
 *            full-target quad with D3D texture coordinates, point sampling): a linear 2x2 8_8_8_8 texture (each
 *            quarter of the target one texel), the same with a red / blue swizzle, a tiled 32x32 8_8_8_8 texture
 *            (texel x, y = 8x, 8y, 0), a linear DXT1 block (8-in-16 swap, big-endian 16-bit words as the CPU writes
 *            them);
 *   frames 11-12, a resolve: a red quad, then a resolve of the whole target to a tiled 64x64 8_8_8_8 texture with
 *            a green colour clear (the target is green afterwards, the texture red), and the next frame samples that
 *            texture back (red again: render to texture);
 *   frame 14: a rectangle list: three corners (top-left red, top-right green, bottom-left blue) of a full-target
 *            rectangle; the fourth (bottom-right) gets green + blue - red, i.e. cyan once clamped;
 *   frame 15: the alpha test (less than 0.5): a green quad at alpha 0.25 passes, then a red one at 0.75 fails;
 *   frames 16-17: packed vertex colours: 16_16_16_16 normalised (65535, 0, 32768, 65535) and 16_16_16_16_FLOAT
 *            (0, 1, 0.5, 1), X in each dword's low half;
 *   frame 18: the pixel parameters (SQ_PROGRAM_CNTL param_gen, SQ_CONTEXT_MISC register 1): ps_pos writes
 *            r1 * (1/64, 1/64, 0, 1), so a pixel's red / green are its x / y from the top-left, times 4;
 *   frame 19: VGT_INDX_OFFSET 4 shifts the indices 0..3 of a strip onto the red quad's vertices 4..7 of the
 *            depth frame's buffer, and a strip cut by the reset index (two triangles of a quad, then a restart);
 *   frame 20: two render targets: ps_mrt writes the green quad's colour to oC0 and c1 (blue) to oC1; a resolve of
 *            render target 1 copies blue into memory while the frame (target 0) is green;
 *   frames 21-23: a linear 4x4 cube texture (one colour per face, faces 4 KB apart) sampled through tfetchCube's
 *            (S, T, face) coordinates at the centres of faces 4 (+Z: cyan) and 1 (-X: green), and a linear 4x4x4
 *            3D texture (one colour per slice) at depth 0.625 (slice 2: blue).
 * Then WP-26's window offset (a picture drawn and resolved as two tiles: scissor and resolve rectangle moved, the
 * right tile resolved into the left one's picture), eagerly and with lazy resolves; WP-27's viewport z (scale -1,
 * offset 1, the test "greater"); WP-28's clear through a 4x view of 1x tiles.
 * At the end (renderers of their own): WP-2's lazy resolves; WP-10's rectangle lists, 20 in one list (two passes of
 * the GPU path), drawn on the GPU and with RECOMP_X360_RECTS=cpu, the same bytes, and WP-29's start-up self-test
 * of the capture agreeing with what the read-back path drew.
 * Each frame is read back at the swap. Exit 77 when EGL / GLES 3.0 cannot be had. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xenos_cp.h"
#include "xenos_gl.h"

#define MEM_SIZE 0x100000u
#define W 64
#define H 64

static uint8_t mem[MEM_SIZE];
static int failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void wr32(void *ctx, uint32_t phys, uint32_t v)
{
    (void)ctx;
    if (phys + 4 <= MEM_SIZE)
        mem[phys] = (uint8_t)(v >> 24), mem[phys + 1] = (uint8_t)(v >> 16), mem[phys + 2] = (uint8_t)(v >> 8),
        mem[phys + 3] = (uint8_t)v;
}

static uint32_t rd32(void *ctx, uint32_t phys)
{
    (void)ctx;
    if (phys + 4 > MEM_SIZE)
        return 0;
    return (uint32_t)mem[phys] << 24 | (uint32_t)mem[phys + 1] << 16 | (uint32_t)mem[phys + 2] << 8 | mem[phys + 3];
}

static void w32(uint32_t a, uint32_t v)
{
    mem[a] = (uint8_t)(v >> 24), mem[a + 1] = (uint8_t)(v >> 16);
    mem[a + 2] = (uint8_t)(v >> 8), mem[a + 3] = (uint8_t)v;
}

static uint32_t fbits(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

static uint32_t *load(const char *dir, const char *name, uint32_t *n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t b[4096];
    size_t len = fread(b, 1, sizeof b, f);
    fclose(f);
    uint32_t *w = malloc(len);
    for (size_t i = 0; i + 4 <= len; i += 4)
        w[i / 4] = (uint32_t)b[i] << 24 | (uint32_t)b[i + 1] << 16 | (uint32_t)b[i + 2] << 8 | b[i + 3];
    *n = (uint32_t)(len / 4);
    return w;
}

/* a packet stream at 0x8000 */
static uint32_t pk[4096];
static uint32_t npk;

static void type3(uint32_t op, const uint32_t *w, uint32_t n)
{
    pk[npk++] = 3u << 30 | (n - 1) << 16 | op << 8;
    for (uint32_t i = 0; i < n; i++)
        pk[npk++] = w[i];
}

static void reg(uint32_t index, uint32_t v)
{
    pk[npk++] = 0u << 30 | 0u << 16 | index;
    pk[npk++] = v;
}

/* the viewport Direct3D sets for the whole target: VTE scale / offset on, (W/2, -H/2) around the centre, z 0..1 */
static void full_viewport(void)
{
    reg(0x2206, 0x3Fu);
    reg(0x2113, fbits(1.0f));
    reg(0x2114, 0);
    reg(0x210F, fbits(W / 2.0f));
    reg(0x2110, fbits(W / 2.0f));
    reg(0x2111, fbits(-H / 2.0f));
    reg(0x2112, fbits(H / 2.0f));
}

/* the byte offset of texel (x, y) of a tiled 32-bit texture with a 64-texel pitch (the renderer's formula) */
static uint32_t tiled_off(int x, int y)
{
    int32_t outer = ((y >> 5) * (W >> 5) + (x >> 5)) << 6, inner = (((y >> 1) & 7) << 3) | (x & 7);
    int32_t oi = (outer | inner) << 2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return (uint32_t)((int32_t)((uint32_t)(y & 1) << 4 | pipe << 6 | bank << 11) | (oi & 15) | ((oi >> 4) & 1) << 5 |
                      ((oi >> 5) & 7) << 8 | (oi >> 8) << 12);
}

/* the same for a texture of 2^log2 bytes a texel */
static uint32_t tiled_off_log2(int x, int y, unsigned log2)
{
    int32_t outer = ((y >> 5) * (W >> 5) + (x >> 5)) << 6, inner = (((y >> 1) & 7) << 3) | (x & 7);
    int32_t oi = (outer | inner) << log2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return (uint32_t)((int32_t)((uint32_t)(y & 1) << 4 | pipe << 6 | bank << 11) | (oi & 15) | ((oi >> 4) & 1) << 5 |
                      ((oi >> 5) & 7) << 8 | (oi >> 8) << 12);
}

/* the same for any pitch (texels, a multiple of 32) */
static uint32_t tiled_off_pitch(int x, int y, int pitch, unsigned log2)
{
    int32_t outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6, inner = (((y >> 1) & 7) << 3) | (x & 7);
    int32_t oi = (outer | inner) << log2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return (uint32_t)((int32_t)((uint32_t)(y & 1) << 4 | pipe << 6 | bank << 11) | (oi & 15) | ((oi >> 4) & 1) << 5 |
                      ((oi >> 5) & 7) << 8 | (oi >> 8) << 12);
}

static void run(xenos_cp *cp)
{
    for (uint32_t i = 0; i < npk; i++)
        w32(0x8000 + 4 * i, pk[i]);
    xenos_cp_execute(cp, 0x8000, npk);
    npk = 0;
}

/* XBA-296's frames: the target black again (a title that clears through resolves gets no clear at the swap): frame 1's
 * quad at 0x2000 with the pixel shader's c0 at 0, then c0 back to 1 */
static void black(void)
{
    uint32_t strip = 6u | 2u << 6 | 4u << 16;
    for (uint32_t i = 0; i < 4; i++)
        reg(0x4400 + i, 0);
    reg(0x4800, 0x2000u | 3u);
    reg(0x4801, 2u | (32u << 2));
    type3(PM4_DRAW_INDX_2, &strip, 1);
    for (uint32_t i = 0; i < 4; i++)
        reg(0x4400 + i, fbits(1.0f));
}

static void vertex_z(uint32_t at, float x, float y, float z, float r, float g, float b)
{
    const float v[8] = {x, y, z, 1.0f, r, g, b, 1.0f};
    for (int i = 0; i < 8; i++)
        w32(at + 4 * i, fbits(v[i]));
}

static void vertex(uint32_t at, float x, float y, float r, float g, float b)
{
    const float v[8] = {x, y, 0.5f, 1.0f, r, g, b, 1.0f};
    for (int i = 0; i < 8; i++)
        w32(at + 4 * i, fbits(v[i]));
}

static const uint8_t *px(xenos_gl *g, int x, int y)
{
    int w, h;
    const uint8_t *f = xenos_gl_frame(g, &w, &h);
    return f + 4 * ((size_t)y * w + x);
}

/* a screen-space quad (VTE off: positions are pixels) from (x0, y0) to (x1, y1) of one colour at z, vertices at `at`,
 * drawn with the colour pixel shader (ps) as a strip */
static void screen_quad(uint32_t at, float x0, float y0, float x1, float y1, float z, float r, float g, float b,
                        const uint32_t *ps, uint32_t nps)
{
    static uint32_t im[2 + 1024];
    vertex_z(at, x0, y0, z, r, g, b), vertex_z(at + 32, x1, y0, z, r, g, b);
    vertex_z(at + 64, x0, y1, z, r, g, b), vertex_z(at + 96, x1, y1, z, r, g, b);
    im[0] = 1, im[1] = nps;
    memcpy(im + 2, ps, 4 * nps);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
    reg(0x4800, at | 3u), reg(0x4801, 2u | (32u << 2));
    uint32_t strip = 6u | 2u << 6 | 4u << 16;
    type3(PM4_DRAW_INDX_2, &strip, 1);
}

/* a resolve of render target 0's rectangle (x0, y0)-(x1, y1) to a tiled 8_8_8_8 at dst (pitch 64, 64 rows), no clear;
 * its three corners at `at` */
static void resolve_rect(uint32_t at, float x0, float y0, float x1, float y1, uint32_t dst)
{
    const float v[6] = {x0, y0, x1, y0, x1, y1};
    for (int i = 0; i < 6; i++)
        w32(at + 4 * (uint32_t)i, fbits(v[i]));
    reg(0x4800, at | 3u), reg(0x4801, 2u | (6u << 2));
    reg(0x2208, 6), reg(0x2318, 0), reg(0x2319, dst), reg(0x231A, 64u | 64u << 16), reg(0x231B, 2u | 6u << 7);
    uint32_t rect = 8u | 2u << 6 | 3u << 16;
    type3(PM4_DRAW_INDX_2, &rect, 1);
    reg(0x2208, 4);
}

/* WP-26, PA_SC_WINDOW_OFFSET: a 64-wide picture drawn as two 32-wide tiles on one surface (pitch 32, tile 0), the way
 * a 1080p title draws 1920 as 2 x 960. Left tile: green over everything, resolved (0,0)-(32,64) to dst. Right tile:
 * the window offset -32 (PA_SU_SC_MODE_CNTL bit 16 on), the window scissor 40..64 (in window coordinates: 8..32 on the
 * surface), a red quad over all the window; resolved from (32,0)-(64,64) (window coordinates) into dst + one
 * 32x32 block (Direct3D moves the base by the left tile's blocks). Memory: x 0..39 green, 40..63 red. Lazy: both
 * resolves into dst's one host picture (one pending rectangle: written once). */
static void window_offset_tiles(xenos_gl *g, xenos_cp *cp, const uint32_t *ps, uint32_t nps, int lazy)
{
    enum { DST = 0xE4000, VQ = 0xE8000, RQ = 0xE8800 };
    for (uint32_t i = 0; i < 64 * 64 * 4; i += 4)
        w32(DST + i, 0xABABABABu);
    xenos_gl_stats s0, s1;
    xenos_gl_get_stats(g, &s0);
    for (uint32_t i = 0; i < 4; i++)
        reg(0x4400 + i, fbits(1.0f));
    reg(0x2104, 0xFFFFu), reg(0x2201, 0x00010001u), reg(0x2200, 0), reg(0x2206, 0), reg(0x2208, 4);
    reg(0x2000, 32u), reg(0x2001, 0), reg(0x2002, 0x200u);
    reg(0x2205, 1u << 16), reg(0x2080, 0);
    reg(0x2081, 0), reg(0x2082, 32u | 64u << 16);
    screen_quad(VQ, -8, -8, 72, 72, 0.5f, 0, 1, 0, ps, nps);
    resolve_rect(RQ, 0, 0, 32, 64, DST);
    reg(0x2080, 0x7FE0u);                               /* x -32 */
    reg(0x2081, 40u), reg(0x2082, 64u | 64u << 16);
    screen_quad(VQ + 0x100, -8, -8, 72, 72, 0.5f, 1, 0, 0, ps, nps);
    resolve_rect(RQ + 0x40, 32, 0, 64, 64, DST + 32 * 32 * 4);
    reg(0x2080, 0), reg(0x2205, 0), reg(0x2081, 0), reg(0x2082, 0);
    reg(0x2000, 0), reg(0x2001, 0), reg(0x2002, 0), reg(0x2206, 0x3Fu);
    run(cp);
    xenos_gl_get_stats(g, &s1);
    const char *mode = lazy ? "lazy" : "eager";
    CHECK(s1.unsupported == s0.unsupported && s1.resolves == s0.resolves + 2,
          "window offset tiles (%s): resolves +%llu (want 2), unsupported +%llu (want 0)", mode,
          (unsigned long long)(s1.resolves - s0.resolves), (unsigned long long)(s1.unsupported - s0.unsupported));
    if (lazy) {
        CHECK(s1.resolves_deferred == s0.resolves_deferred + 2 &&
                  rd32(NULL, DST + tiled_off_pitch(50, 10, 64, 2)) == 0xABABABABu,
              "window offset tiles (lazy): deferred +%llu (want 2), memory %08X (want ABABABAB)",
              (unsigned long long)(s1.resolves_deferred - s0.resolves_deferred),
              rd32(NULL, DST + tiled_off_pitch(50, 10, 64, 2)));
        xenos_gl_flush_resolves(g, cp);
        xenos_gl_get_stats(g, &s1);
        CHECK(s1.resolve_flushes == s0.resolve_flushes + 1,
              "window offset tiles (lazy): written %llu times, want once (the two tiles one rectangle)",
              (unsigned long long)(s1.resolve_flushes - s0.resolve_flushes));
    }
    static const struct {
        int x, y;
        uint32_t want;
        const char *what;
    } at[6] = {
        {1, 1, 0xFF00FF00u, "left tile: green"},
        {31, 63, 0xFF00FF00u, "left tile's last column: green"},
        {32, 5, 0xFF00FF00u, "right tile, left of its scissor: green (the left tile's quad)"},
        {39, 40, 0xFF00FF00u, "right tile, the scissor's edge - 1: green"},
        {40, 40, 0xFF0000FFu, "right tile, in the scissor moved by the window offset: red"},
        {63, 63, 0xFF0000FFu, "right tile's last pixel: red"},
    };
    for (int i = 0; i < 6; i++) {
        uint32_t t = rd32(NULL, DST + tiled_off_pitch(at[i].x, at[i].y, 64, 2));
        CHECK(t == at[i].want, "window offset tiles (%s), %s: %08X, want %08X", mode, at[i].what, t, at[i].want);
    }
}

/* WP-27, the viewport's z (PA_CL_VPORT_ZSCALE -1, ZOFFSET 1: depth reversed, as Adventure Time draws): window z =
 * 1 - z. A blue quad at z 1 (window 0, test always, z write) "clears" the depth to 0; a green one at z 0.25 (window
 * 0.75) passes the test "greater" and writes; a red one at z 0.75 (window 0.25) then fails it: green. Without the
 * viewport's z the green quad fails (0.25 < 1): blue. */
static void viewport_z_reversed(xenos_gl *g, xenos_cp *cp, const uint32_t *ps, uint32_t nps)
{
    enum { VQ = 0xE9000 };
    static const uint32_t swap[4] = {0x53574150u, 0, W, H};
    static uint32_t im[2 + 1024];
    static const float qz[3] = {1.0f, 0.25f, 0.75f}, qc[3][3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}};
    static const uint32_t zfunc[3] = {7, 4, 4}, zwrite[3] = {4, 4, 0}; /* always / greater; z write */
    for (uint32_t q = 0; q < 3; q++)
        for (uint32_t k = 0; k < 4; k++)
            vertex_z(VQ + 0x80 * q + 32 * k, k & 1 ? 1.0f : -1.0f, k & 2 ? -1.0f : 1.0f, qz[q], qc[q][0], qc[q][1],
                     qc[q][2]);
    im[0] = 1, im[1] = nps;
    memcpy(im + 2, ps, 4 * nps);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
    for (uint32_t i = 0; i < 4; i++)
        reg(0x4400 + i, fbits(1.0f));
    reg(0x2104, 0xFFFFu), reg(0x2201, 0x00010001u), reg(0x2205, 0), reg(0x2208, 4);
    reg(0x2000, 0), reg(0x2001, 0), reg(0x2002, 0x100u), reg(0x2081, 0), reg(0x2082, 0);
    reg(0x2206, 0x3Fu);
    reg(0x210F, fbits(W / 2.0f)), reg(0x2110, fbits(W / 2.0f)), reg(0x2111, fbits(-H / 2.0f));
    reg(0x2112, fbits(H / 2.0f)), reg(0x2113, fbits(-1.0f)), reg(0x2114, fbits(1.0f));
    reg(0x4801, 2u | (32u << 2));
    uint32_t strip = 6u | 2u << 6 | 4u << 16;
    for (uint32_t q = 0; q < 3; q++) {
        reg(0x2200, 2u | zwrite[q] | zfunc[q] << 4);
        reg(0x4800, (VQ + 0x80 * q) | 3u);
        type3(PM4_DRAW_INDX_2, &strip, 1);
    }
    type3(PM4_XE_SWAP, swap, 4);
    reg(0x2200, 0), reg(0x2002, 0), reg(0x2113, fbits(1.0f)), reg(0x2114, 0);
    run(cp);
    const uint8_t *c = px(g, W / 2, H / 2);
    CHECK(c[0] == 0 && c[1] == 255 && c[2] == 0,
          "viewport z scale -1 offset 1, test greater: %u,%u,%u, want green (blue: the viewport's z ignored)", c[0],
          c[1], c[2]);
}

/* WP-28, an MSAA view of single-sampled tiles (Direct3D's clears): a 64x64 1x picture (pitch 64, colour tile 0, depth
 * tile 0x200) green at z 0.5; then a rectangle list through the 4x view of the same tiles (pitch 32, RB_SURFACE_INFO
 * msaa 4x) over the view's (0,0)-(32,16): red at z 0.25 (test always, z write); then, at 1x again, a blue quad at
 * z 0.35 with the test "less" (fails where the view's clear put 0.25). The view's pixel (x, y) is the picture's
 * 2x2 block at (2x, 2y): the top half red, the bottom blue. Resolved to dst and read back. */
static void msaa_view_clear(xenos_gl *g, xenos_cp *cp, const uint32_t *ps, uint32_t nps)
{
    enum { DST = 0xE4000, VQ = 0xEA000, RQ = 0xEA800 };
    static uint32_t im[2 + 1024];
    xenos_gl_stats s0, s1;
    xenos_gl_get_stats(g, &s0);
    for (uint32_t i = 0; i < 64 * 64 * 4; i += 4)
        w32(DST + i, 0xABABABABu);
    for (uint32_t i = 0; i < 4; i++)
        reg(0x4400 + i, fbits(1.0f));
    reg(0x2104, 0xFFFFu), reg(0x2201, 0x00010001u), reg(0x2205, 0), reg(0x2206, 0), reg(0x2208, 4);
    reg(0x2080, 0), reg(0x2081, 0), reg(0x2082, 0);
    reg(0x2000, 64u), reg(0x2001, 0), reg(0x2002, 0x200u);
    reg(0x2200, 2u | 4u | 7u << 4); /* z test always, z write */
    screen_quad(VQ, -8, -8, 72, 72, 0.5f, 0, 1, 0, ps, nps);
    reg(0x2000, 32u | 2u << 16); /* the 4x view */
    vertex_z(VQ + 0x100, 0, 0, 0.25f, 1, 0, 0), vertex_z(VQ + 0x120, 32, 0, 0.25f, 1, 0, 0);
    vertex_z(VQ + 0x140, 0, 16, 0.25f, 1, 0, 0);
    im[0] = 1, im[1] = nps;
    memcpy(im + 2, ps, 4 * nps);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
    reg(0x4800, (VQ + 0x100) | 3u), reg(0x4801, 2u | (24u << 2));
    uint32_t rect = 8u | 2u << 6 | 3u << 16;
    type3(PM4_DRAW_INDX_2, &rect, 1);
    reg(0x2000, 64u);
    reg(0x2200, 2u | 1u << 4); /* z test less, no write */
    screen_quad(VQ + 0x200, -8, -8, 72, 72, 0.35f, 0, 0, 1, ps, nps);
    reg(0x2200, 0);
    resolve_rect(RQ, 0, 0, 64, 64, DST);
    reg(0x2000, 0), reg(0x2001, 0), reg(0x2002, 0), reg(0x2206, 0x3Fu);
    run(cp);
    xenos_gl_get_stats(g, &s1);
    CHECK(s1.msaa_views == s0.msaa_views + 1, "MSAA view: drawn into its 1x surfaces %llu times, want 1",
          (unsigned long long)(s1.msaa_views - s0.msaa_views));
    static const struct {
        int x, y;
        uint32_t want;
        const char *what;
    } at[5] = {
        {1, 1, 0xFF0000FFu, "the view's clear: red"},
        {62, 30, 0xFF0000FFu, "the view's clear, its last 2x2 block: red"},
        {62, 33, 0xFFFF0000u, "below it: blue (depth 0.5 from the green quad, 0.35 passes)"},
        {1, 40, 0xFFFF0000u, "below it: blue"},
        {40, 63, 0xFFFF0000u, "the last row: blue"},
    };
    for (int i = 0; i < 5; i++) {
        uint32_t t = rd32(NULL, DST + tiled_off_pitch(at[i].x, at[i].y, 64, 2));
        CHECK(t == at[i].want, "MSAA view of 1x tiles, %s: %08X, want %08X", at[i].what, t, at[i].want);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: xenos_draw_test DIR\n");
        return 2;
    }
    char cache[1024], why[256];
    snprintf(cache, sizeof cache, "%s/cache", argv[1]);
    /* XENOS_DRAW_SCALE=N: the same frames at an internal resolution of N x (read back at the guest size) */
    const char *scale_env = getenv("XENOS_DRAW_SCALE");
    int scale = scale_env && *scale_env ? atoi(scale_env) : 1;
    xenos_gl *g = xenos_gl_create_scaled(cache, W, H, scale, why, sizeof why);
    if (!g) {
        printf("skipped: %s\n", why);
        return 77;
    }
    /* the frames: the first two written as PPM, the rest only counted */
    char frames[1100];
    snprintf(frames, sizeof frames, "%s/frames", argv[1]);
    mkdir(frames, 0755);
    xenos_gl_set_frames_dir(g, frames, 2);
    uint32_t nvs, nps;
    uint32_t *vs = load(argv[1], "vs.bin", &nvs), *ps = load(argv[1], "ps.bin", &nps);
    if (!vs || !ps) {
        fprintf(stderr, "no vs.bin / ps.bin in %s (make_draw.py)\n", argv[1]);
        return 1;
    }
    static xenos_cp cp; /* zeroed: xenos_cp_init keeps the callbacks and the trace it finds */
    xenos_cp_init(&cp);
    cp.read32 = rd32;
    cp.write32 = wr32;
    cp.render_ctx = g;
    cp.draw_hook = (void (*)(void *, const xenos_cp *, const xenos_draw *))xenos_gl_draw;
    cp.swap_hook = (void (*)(void *, const xenos_cp *))xenos_gl_swap;

    uint32_t im[2 + 1024];
    im[0] = 0, im[1] = nvs;
    memcpy(im + 2, vs, 4 * nvs);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nvs);
    im[0] = 1, im[1] = nps;
    memcpy(im + 2, ps, 4 * nps);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
    static const uint32_t swap[4] = {0x53574150u, 0, W, H};

    /* the render state a title sets first: all colour channels of the four targets written, no blending (src * 1 +
     * dst * 0) */
    reg(0x2104, 0xFFFFu);
    reg(0x2201, 0x00010001u), reg(0x2209, 0x00010001u), reg(0x220A, 0x00010001u), reg(0x220B, 0x00010001u);
    full_viewport();

    /* frame 0: a red triangle */
    vertex(0x1000, -0.5f, -0.5f, 1, 0, 0);
    vertex(0x1020, 0.5f, -0.5f, 1, 0, 0);
    vertex(0x1040, 0.0f, 0.5f, 1, 0, 0);
    reg(0x4800, 0x1000u | 3u);            /* fetch constant 0: vertex, address */
    reg(0x4801, 2u | (24u << 2));         /* 8-in-32 swap, 24 dwords */
    for (int i = 0; i < 4; i++)
        reg(0x4400 + i, fbits(1.0f));     /* pixel c0 */
    uint32_t init = 4u | 2u << 6 | 3u << 16; /* triangle list, auto indices, 3 */
    type3(PM4_DRAW_INDX_2, &init, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    const uint8_t *c = px(g, W / 2, H / 2), *k = px(g, 1, 1);
    CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0, "frame 0 centre %u %u %u, want red", c[0], c[1], c[2]);
    CHECK(k[0] == 0 && k[1] == 0 && k[2] == 0, "frame 0 corner %u %u %u, want clear", k[0], k[1], k[2]);
    /* Xenos's y grows downwards, so the apex (y = +0.5) is in the top half: a pixel left of centre near the top
     * edge of the triangle is outside, near its base it is inside */
    const uint8_t *top = px(g, W / 2, H / 4 + 2), *base = px(g, W / 2 - 8, 3 * H / 4 - 2);
    CHECK(top[0] == 255 && base[0] == 255, "frame 0 apex / base pixels %u %u", top[0], base[0]);

    /* frame 1: a full-target quad, 16-bit big-endian indices */
    vertex(0x2000, -1, -1, 0, 1, 0);
    vertex(0x2020, 1, -1, 0, 1, 0);
    vertex(0x2040, -1, 1, 0, 1, 0);
    vertex(0x2060, 1, 1, 0, 1, 0);
    static const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
    for (int i = 0; i < 6; i++)
        mem[0x3000 + 2 * i] = (uint8_t)(idx[i] >> 8), mem[0x3000 + 2 * i + 1] = (uint8_t)idx[i];
    reg(0x4800, 0x2000u | 3u);
    reg(0x4801, 2u | (32u << 2));
    reg(0x4401, fbits(0.5f));
    uint32_t dr[3] = {4u | 0u << 6 | 6u << 16, 0x3000, 3u | 1u << 30}; /* triangle list, DMA, 16-bit, 8-in-16 */
    type3(PM4_DRAW_INDX_2, dr, 3);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    int all = 1;
    /* the edges of a full-target quad run through pixel centres (Direct3D's half-pixel offset): inner pixels */
    for (int y = 1; y < H - 1; y += 7)
        for (int x = 1; x < W - 1; x += 7) {
            const uint8_t *p = px(g, x, y);
            if (p[0] != 0 || p[1] < 126 || p[1] > 129 || p[2] != 0)
                all = 0;
        }
    CHECK(all, "frame 1: not every pixel half green (corner %u %u %u)", px(g, 1, 1)[0], px(g, 1, 1)[1],
          px(g, 1, 1)[2]);

    /* frame 2: the viewport: scale W/4, H/4 (y negative: Xenos's downward y), offset W/4, H/4 -> the top-left
     * quarter */
    reg(0x2206, 0x3Fu);
    reg(0x210F, fbits(W / 4.0f));
    reg(0x2110, fbits(W / 4.0f));
    reg(0x2111, fbits(-H / 4.0f));
    reg(0x2112, fbits(H / 4.0f));
    type3(PM4_DRAW_INDX_2, dr, 3);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    const uint8_t *in = px(g, W / 8, H / 8), *out = px(g, 3 * W / 4, 3 * H / 4), *out2 = px(g, 3 * W / 4, H / 8);
    CHECK(in[1] > 100 && out[1] == 0 && out2[1] == 0, "frame 2: top-left %u, bottom-right %u, top-right %u", in[1],
          out[1], out2[1]);

    /* frame 3: depth (back to the full-target viewport) */
    full_viewport();
    reg(0x4401, fbits(1.0f));
    vertex_z(0x4000, -1, -1, 0.25f, 0, 1, 0);
    vertex_z(0x4020, 1, -1, 0.25f, 0, 1, 0);
    vertex_z(0x4040, -1, 1, 0.25f, 0, 1, 0);
    vertex_z(0x4060, 1, 1, 0.25f, 0, 1, 0);
    vertex_z(0x4080, -1, -1, 0.75f, 1, 0, 0);
    vertex_z(0x40A0, 1, -1, 0.75f, 1, 0, 0);
    vertex_z(0x40C0, -1, 1, 0.75f, 1, 0, 0);
    vertex_z(0x40E0, 1, 1, 0.75f, 1, 0, 0);
    reg(0x2200, 2u | 4u | 1u << 4); /* z test, z write, less */
    reg(0x4800, 0x4000u | 3u);
    reg(0x4801, 2u | (64u << 2));
    uint32_t strip_a = 6u | 2u << 6 | 4u << 16; /* triangle strip of vertices 0..3 */
    type3(PM4_DRAW_INDX_2, &strip_a, 1);
    reg(0x4800, 0x4080u | 3u);
    type3(PM4_DRAW_INDX_2, &strip_a, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    c = px(g, W / 2, H / 2);
    CHECK(c[0] == 0 && c[1] == 255, "frame 3 (depth): %u %u %u, want green", c[0], c[1], c[2]);

    /* frame 4: blending */
    reg(0x2200, 0);
    reg(0x4800, 0x4080u | 3u);
    type3(PM4_DRAW_INDX_2, &strip_a, 1);                /* red, opaque */
    reg(0x2201, 6u | 7u << 8 | 1u << 16 | 0u << 24);   /* src alpha, one minus src alpha; alpha: one, zero */
    reg(0x4403, fbits(0.5f));                          /* c0.w: the pixel's alpha 0.5 */
    reg(0x4800, 0x4000u | 3u);
    type3(PM4_DRAW_INDX_2, &strip_a, 1);                /* green, half */
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    c = px(g, W / 2, H / 2);
    CHECK(c[0] >= 126 && c[0] <= 129 && c[1] >= 126 && c[1] <= 129 && c[2] == 0,
          "frame 4 (blend): %u %u %u, want 128 128 0", c[0], c[1], c[2]);

    /* frame 5: culling back faces: drawn with front = CCW, culled with front = CW */
    reg(0x2201, 0x00010001u);
    reg(0x4403, fbits(1.0f));
    reg(0x4800, 0x1000u | 3u);
    reg(0x2205, 2u);                                   /* cull back, front is CCW */
    type3(PM4_DRAW_INDX_2, &init, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    c = px(g, W / 2, H / 2);
    CHECK(c[0] == 255, "frame 5 (front CCW, cull back): centre %u, want the triangle", c[0]);
    reg(0x2205, 2u | 4u);                              /* cull back, front is CW */
    type3(PM4_DRAW_INDX_2, &init, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    c = px(g, W / 2, H / 2);
    CHECK(c[0] == 0, "frame 6 (front CW, cull back): centre %u, want culled", c[0]);

    /* frame 7: a quad list: vertices 0, 1, 3, 2 of the green quad (a quad goes round its corners) */
    reg(0x2205, 0);
    vertex(0x5000, -1, -1, 0, 0, 1);
    vertex(0x5020, 1, -1, 0, 0, 1);
    vertex(0x5040, 1, 1, 0, 0, 1);
    vertex(0x5060, -1, 1, 0, 0, 1);
    reg(0x4800, 0x5000u | 3u);
    uint32_t quad = 13u | 2u << 6 | 4u << 16;
    type3(PM4_DRAW_INDX_2, &quad, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    const uint8_t *q0 = px(g, 1, 1), *q1 = px(g, W - 2, H - 2), *q2 = px(g, W - 2, 1), *q3 = px(g, 1, H - 2);
    CHECK(q0[2] == 255 && q1[2] == 255 && q2[2] == 255 && q3[2] == 255, "frame 7 (quad list): corners %u %u %u %u",
          q0[2], q1[2], q2[2], q3[2]);

    /* frames 8..11: textures */
    uint32_t ntex;
    uint32_t *pst = load(argv[1], "ps_tex.bin", &ntex), *pst2 = pst;
    CHECK(pst != NULL, "no ps_tex.bin");
    if (pst) {
        im[0] = 1, im[1] = ntex;
        memcpy(im + 2, pst, 4 * ntex);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + ntex);
    }
    /* the quad: colour = texture coordinates (u, v), v = 0 at the top */
    vertex(0x6000, -1, 1, 0, 0, 0);
    vertex(0x6020, 1, 1, 1, 0, 0);
    vertex(0x6040, -1, -1, 0, 1, 0);
    vertex(0x6060, 1, -1, 1, 1, 0);
    reg(0x4800, 0x6000u | 3u);
    static const uint32_t texels[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu}; /* X in the low byte */
    for (int i = 0; i < 4; i++)
        w32(0x10000 + 256 * (i / 2) + 4 * (i % 2), texels[i]); /* rows 256-byte aligned */
    uint32_t sw_id = 0u | 1u << 3 | 2u << 6 | 3u << 9, sw_rb = 2u | 1u << 3 | 0u << 6 | 3u << 9;
    uint32_t fc[6] = {2u | 2u << 10 | 2u << 13 | 2u << 22, 6u | 2u << 6 | 0x10000u, 1u | 1u << 13, sw_id << 1, 0,
                      1u << 9}; /* pitch 64 texels: 256-byte rows, as Direct3D aligns them */
    for (int i = 0; i < 6; i++)
        reg(0x480C + i, fc[i]);
    type3(PM4_DRAW_INDX_2, &strip_a, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    const uint8_t *tl = px(g, 8, 8), *tr = px(g, W - 8, 8), *bl = px(g, 8, H - 8), *brp = px(g, W - 8, H - 8);
    CHECK(tl[0] == 255 && tl[1] == 0 && tr[1] == 255 && tr[0] == 0 && bl[2] == 255 && bl[0] == 0 && brp[0] == 255 &&
              brp[1] == 255 && brp[2] == 255,
          "frame 8 (linear texture): %u,%u,%u %u,%u,%u %u,%u,%u %u,%u,%u", tl[0], tl[1], tl[2], tr[0], tr[1], tr[2],
          bl[0], bl[1], bl[2], brp[0], brp[1], brp[2]);
    reg(0x480F, sw_rb << 1);
    type3(PM4_DRAW_INDX_2, &strip_a, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    tl = px(g, 8, 8);
    CHECK(tl[2] == 255 && tl[0] == 0, "frame 9 (swizzle): top-left %u,%u,%u, want blue", tl[0], tl[1], tl[2]);
    /* tiled 32x32: the Xenos 2D tiling (same formula as the renderer: this checks the plumbing, not the formula) */
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int32_t outer = ((y >> 5) * (32 >> 5) + (x >> 5)) << 6, inner = (((y >> 1) & 7) << 3) | (x & 7);
            int32_t oi = (outer | inner) << 2;
            uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
            int32_t off = (int32_t)((uint32_t)(y & 1) << 4 | pipe << 6 | bank << 11) | (oi & 15) |
                          ((oi >> 4) & 1) << 5 | ((oi >> 5) & 7) << 8 | (oi >> 8) << 12;
            w32(0x12000 + (uint32_t)off, 0xFF000000u | (uint32_t)(8 * y) << 8 | (uint32_t)(8 * x));
        }
    uint32_t fct[6] = {2u | 2u << 10 | 2u << 13 | 1u << 22 | 1u << 31, 6u | 2u << 6 | 0x12000u, 31u | 31u << 13,
                       sw_id << 1, 0, 1u << 9};
    for (int i = 0; i < 6; i++)
        reg(0x480C + i, fct[i]);
    type3(PM4_DRAW_INDX_2, &strip_a, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    static const int probe[4][2] = {{5, 9}, {31, 0}, {17, 23}, {0, 31}};
    int tiles_ok = 1;
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = px(g, 2 * probe[i][0] + 1, 2 * probe[i][1] + 1);
        if (p[0] != 8 * probe[i][0] || p[1] != 8 * probe[i][1])
            tiles_ok = 0, fprintf(stderr, "  texel %d,%d read %u,%u\n", probe[i][0], probe[i][1], p[0], p[1]);
    }
    CHECK(tiles_ok, "frame 10 (tiled texture)");
    /* DXT1: one block, colour 0 = S3TC's 5:6:5 (red in the 5 high bits: 0xF800), colour 1 = 0, every index 0 */
    static const uint16_t blk[4] = {0xF800, 0x0000, 0x0000, 0x0000};
    for (int i = 0; i < 4; i++)
        mem[0x14000 + 2 * i] = (uint8_t)(blk[i] >> 8), mem[0x14000 + 2 * i + 1] = (uint8_t)blk[i];
    uint32_t fcd[6] = {2u | 2u << 10 | 2u << 13 | 1u << 22, 18u | 1u << 6 | 0x14000u, 3u | 3u << 13, sw_id << 1, 0,
                       1u << 9};
    for (int i = 0; i < 6; i++)
        reg(0x480C + i, fcd[i]);
    type3(PM4_DRAW_INDX_2, &strip_a, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    c = px(g, W / 2, H / 2);
    CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0, "frame 11 (DXT1): %u,%u,%u, want red", c[0], c[1], c[2]);

    /* frames 12-13: a resolve, then the resolved texture sampled */
    im[0] = 1, im[1] = nps; /* the colour pixel shader again */
    memcpy(im + 2, ps, 4 * nps);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
    reg(0x4800, 0x4080u | 3u); /* the red quad */
    reg(0x4801, 2u | (32u << 2));
    type3(PM4_DRAW_INDX_2, &strip_a, 1);
    /* the resolve: three window-space vertices at vertex fetch constant 0, eDRAM mode copy */
    static const float rv[6] = {0, 0, W, 0, W, H};
    for (int i = 0; i < 6; i++)
        w32(0x7000 + 4 * (uint32_t)i, fbits(rv[i]));
    reg(0x4800, 0x7000u | 3u);
    reg(0x4801, 2u | (6u << 2));
    reg(0x2208, 6);                                     /* RB_MODECONTROL: copy */
    reg(0x2318, 0u | 1u << 8 | 0u << 20);               /* colour 0, clear colour, raw copy */
    reg(0x2319, 0x18000);                               /* RB_COPY_DEST_BASE */
    reg(0x231A, W | (uint32_t)H << 16);                  /* pitch, height */
    reg(0x231B, 2u | 6u << 7);                          /* 8-in-32, 8_8_8_8 */
    reg(0x231E, 0xFF00FF00u);                           /* clear: green (the raw eDRAM value) */
    uint32_t rect = 8u | 2u << 6 | 3u << 16;
    type3(PM4_DRAW_INDX_2, &rect, 1);
    reg(0x2208, 4);                                     /* back to colour + depth */
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    c = px(g, W / 2, H / 2);
    CHECK(c[0] == 0 && c[1] == 255, "frame 12 (resolve clear): %u,%u,%u, want green", c[0], c[1], c[2]);
    /* the copy: texel (x, y) of the tiled destination holds red (X = R in the low byte, 8-in-32: big-endian) */
    uint32_t t10 = rd32(NULL, 0x18000 + tiled_off(1, 1)), tmid = rd32(NULL, 0x18000 + tiled_off(37, 21));
    CHECK(t10 == 0xFF0000FFu && tmid == 0xFF0000FFu, "resolved texels %08X %08X, want FF0000FF", t10, tmid);
    /* frame 13: sample the resolved texture */
    if (pst2) {
        im[0] = 1, im[1] = ntex;
        memcpy(im + 2, pst2, 4 * ntex);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + ntex);
    }
    reg(0x4800, 0x6000u | 3u);
    reg(0x4801, 2u | (32u << 2));
    uint32_t fcr[6] = {2u | 2u << 10 | 2u << 13 | 2u << 22 | 1u << 31, 6u | 2u << 6 | 0x18000u, 63u | 63u << 13,
                       sw_id << 1, 0, 1u << 9};
    for (int i = 0; i < 6; i++)
        reg(0x480C + i, fcr[i]);
    type3(PM4_DRAW_INDX_2, &strip_a, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    c = px(g, W / 2, H / 2);
    CHECK(c[0] == 255 && c[1] == 0, "frame 13 (resolved texture sampled): %u,%u,%u, want red", c[0], c[1], c[2]);

    /* frame 14: a rectangle list */
    im[0] = 1, im[1] = nps;
    memcpy(im + 2, ps, 4 * nps);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
    vertex(0x7100, -1, 1, 1, 0, 0);
    vertex(0x7120, 1, 1, 0, 1, 0);
    vertex(0x7140, -1, -1, 0, 0, 1);
    reg(0x4800, 0x7100u | 3u);
    reg(0x4801, 2u | (24u << 2));
    type3(PM4_DRAW_INDX_2, &rect, 1);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    const uint8_t *rtl = px(g, 1, 1), *rtr = px(g, W - 2, 1), *rbl = px(g, 1, H - 2), *rbr = px(g, W - 2, H - 2);
    CHECK(rtl[0] > 240 && rtr[1] > 240 && rbl[2] > 240 && rbr[0] < 15 && rbr[1] > 240 && rbr[2] > 240,
          "frame 14 (rectangle list): corners %u,%u,%u %u,%u,%u %u,%u,%u %u,%u,%u", rtl[0], rtl[1], rtl[2], rtr[0],
          rtr[1], rtr[2], rbl[0], rbl[1], rbl[2], rbr[0], rbr[1], rbr[2]);

    /* frame 15: the alpha test, RB_COLORCONTROL less (1) + enable, RB_ALPHA_REF 0.5 */
    reg(0x2202, 1u | 1u << 3);
    reg(0x210E, fbits(0.5f));
    reg(0x4801, 2u | (32u << 2));
    reg(0x4403, fbits(0.25f));
    reg(0x4800, 0x4000u | 3u);
    type3(PM4_DRAW_INDX_2, &strip_a, 1); /* green at alpha 0.25: passes */
    reg(0x4403, fbits(0.75f));
    reg(0x4800, 0x4080u | 3u);
    type3(PM4_DRAW_INDX_2, &strip_a, 1); /* red at alpha 0.75: discarded */
    reg(0x2202, 0);
    reg(0x4403, fbits(1.0f));
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    c = px(g, W / 2, H / 2);
    CHECK(c[1] == 255 && c[0] == 0, "frame 15 (alpha test): %u,%u,%u, want green", c[0], c[1], c[2]);

    /* frames 16-17: packed vertex formats */
    static const char *const packed[2] = {"vs_u16.bin", "vs_h16.bin"};
    static const uint32_t colour_words[2][2] = {{0x0000FFFFu, 0xFFFF8000u}, {0x3C000000u, 0x3C003800u}};
    static const uint8_t want[2][3] = {{255, 0, 128}, {0, 255, 128}};
    for (int f = 0; f < 2; f++) {
        uint32_t npk_vs;
        uint32_t *pvs = load(argv[1], packed[f], &npk_vs);
        CHECK(pvs != NULL, "no %s", packed[f]);
        if (!pvs)
            continue;
        im[0] = 0, im[1] = npk_vs;
        memcpy(im + 2, pvs, 4 * npk_vs);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + npk_vs);
        free(pvs);
        static const float corner[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (int k = 0; k < 4; k++) {
            uint32_t at = 0x7200 + 24 * (uint32_t)k;
            w32(at, fbits(corner[k][0])), w32(at + 4, fbits(corner[k][1])), w32(at + 8, fbits(0.5f));
            w32(at + 12, fbits(1.0f)), w32(at + 16, colour_words[f][0]), w32(at + 20, colour_words[f][1]);
        }
        reg(0x4800, 0x7200u | 3u);
        reg(0x4801, 2u | (24u << 2));
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        c = px(g, W / 2, H / 2);
        CHECK(c[0] == want[f][0] && c[1] == want[f][1] && c[2] >= want[f][2] - 1 && c[2] <= want[f][2] + 1,
              "frame %d (%s): %u,%u,%u, want %u,%u,%u", 16 + f, packed[f], c[0], c[1], c[2], want[f][0], want[f][1],
              want[f][2]);
    }

    /* frame 18: the pixel parameters */
    {
        uint32_t nps2;
        uint32_t *pp = load(argv[1], "ps_pos.bin", &nps2), *pv = vs;
        CHECK(pp != NULL, "no ps_pos.bin");
        if (pp) {
            im[0] = 0, im[1] = nvs;
            memcpy(im + 2, pv, 4 * nvs);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nvs);
            im[0] = 1, im[1] = nps2;
            memcpy(im + 2, pp, 4 * nps2);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps2);
            free(pp);
            reg(0x2180, 1u << 18);
            reg(0x2181, 1u << 8);
            reg(0x4400, fbits(1.0f / 64)), reg(0x4401, fbits(1.0f / 64)), reg(0x4402, 0), reg(0x4403, fbits(1.0f));
            reg(0x4800, 0x4000u | 3u);
            reg(0x4801, 2u | (32u << 2));
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            reg(0x2180, 0);
            const uint8_t *q = px(g, 10, 50), *q2 = px(g, 40, 3);
            CHECK(q[0] >= 39 && q[0] <= 41 && q[1] >= 199 && q[1] <= 201 && q2[0] >= 159 && q2[0] <= 161 &&
                      q2[1] >= 11 && q2[1] <= 13,
                  "frame 18 (pixel parameters): (10, 50) %u,%u (40, 3) %u,%u, want 40,200 160,12", q[0], q[1], q2[0],
                  q2[1]);
        }
    }

    /* frame 19: the index offset and the primitive reset index */
    im[0] = 1, im[1] = nps;
    memcpy(im + 2, ps, 4 * nps);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
    reg(0x4800, 0x4000u | 3u);
    reg(0x4801, 2u | (64u << 2));
    for (int i = 0; i < 4; i++)
        reg(0x4400 + i, fbits(1.0f));                  /* pixel c0 back to 1 */
    reg(0x2102, 4);                                    /* VGT_INDX_OFFSET: vertices 4..7 (the red quad) */
    static const uint16_t sidx[6] = {0, 1, 2, 3, 0xFFFF, 0};
    for (int i = 0; i < 6; i++)
        mem[0x7400 + 2 * i] = (uint8_t)(sidx[i] >> 8), mem[0x7400 + 2 * i + 1] = (uint8_t)sidx[i];
    reg(0x2103, 0xFFFF);                               /* the reset index */
    reg(0x2205, 1u << 21);                             /* multi_prim_ib_ena */
    uint32_t sdr[3] = {6u | 0u << 6 | 6u << 16, 0x7400, 3u | 1u << 30}; /* strip, DMA, 16-bit, 8-in-16 */
    type3(PM4_DRAW_INDX_2, sdr, 3);
    reg(0x2102, 0);
    reg(0x2205, 0);
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    const uint8_t *s0 = px(g, 1, 1), *s1 = px(g, W - 2, H - 2);
    CHECK(s0[0] == 255 && s0[1] == 0 && s1[0] == 255, "frame 19 (index offset, restart): %u,%u,%u %u,%u,%u", s0[0],
          s0[1], s0[2], s1[0], s1[1], s1[2]);

    /* frame 20: two render targets */
    {
        uint32_t nm;
        uint32_t *pm = load(argv[1], "ps_mrt.bin", &nm);
        CHECK(pm != NULL, "no ps_mrt.bin");
        if (pm) {
            im[0] = 1, im[1] = nm;
            memcpy(im + 2, pm, 4 * nm);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nm);
            free(pm);
            reg(0x4404, 0), reg(0x4405, 0), reg(0x4406, fbits(1.0f)), reg(0x4407, fbits(1.0f)); /* c1: blue */
            reg(0x4800, 0x4000u | 3u);
            reg(0x4801, 2u | (64u << 2));
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            reg(0x4800, 0x7000u | 3u);
            reg(0x4801, 2u | (6u << 2));
            reg(0x2208, 6);
            reg(0x2318, 1u);                         /* render target 1, no clear, raw */
            reg(0x2319, 0x20000);
            type3(PM4_DRAW_INDX_2, &rect, 1);
            reg(0x2208, 4);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            c = px(g, W / 2, H / 2);
            uint32_t t1 = rd32(NULL, 0x20000 + tiled_off(5, 5));
            CHECK(c[1] == 255 && c[2] == 0 && t1 == 0xFFFF0000u,
                  "frame 20 (render targets): target 0 %u,%u,%u, target 1 resolved %08X (want FFFF0000)", c[0], c[1],
                  c[2], t1);
            /* each target its own blend and colour mask (RB_BLENDCONTROL1, RB_COLOR_MASK's second nibble): red added
             * to target 1's blue (one + one) while target 0 is written opaque, then green with target 1 masked off:
             * target 1 resolves magenta, target 0 shows green */
            reg(0x4404, fbits(1.0f)), reg(0x4405, 0), reg(0x4406, 0), reg(0x4407, fbits(1.0f)); /* c1: red */
            reg(0x2209, 1u | 1u << 8 | 1u << 16 | 1u << 24);
            reg(0x4800, 0x4000u | 3u);
            reg(0x4801, 2u | (64u << 2));
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            reg(0x2209, 0x00010001u);
            reg(0x4404, 0), reg(0x4405, fbits(1.0f));                                        /* c1: green */
            reg(0x2104, 0x000Fu);
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            reg(0x2104, 0xFFFFu);
            reg(0x4800, 0x7000u | 3u);
            reg(0x4801, 2u | (6u << 2));
            reg(0x2208, 6);
            type3(PM4_DRAW_INDX_2, &rect, 1);
            reg(0x2208, 4);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            c = px(g, W / 2, H / 2);
            t1 = rd32(NULL, 0x20000 + tiled_off(5, 5));
            CHECK(c[0] == 0 && c[1] == 255 && c[2] == 0 && t1 == 0xFFFF00FFu,
                  "per-target blend / mask: target 0 %u,%u,%u (want green), target 1 resolved %08X (want FFFF00FF)",
                  c[0], c[1], c[2], t1);
        }
    }

    /* frames 21-23: cube and 3D textures */
    {
        static const uint32_t face[6] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF00FFFFu, 0xFFFFFF00u, 0xFFFF00FFu};
        for (uint32_t f = 0; f < 6; f++)
            for (uint32_t y = 0; y < 4; y++)
                for (uint32_t x = 0; x < 4; x++) {
                    w32(0x24000 + f * 4096 + y * 128 + x * 4, face[f]);
                    if (f < 4)
                        w32(0x2C000 + f * 4096 + y * 128 + x * 4, face[f]); /* 3D: slice z = face[z] */
                }
        static const char *const names[3] = {"ps_cube.bin", "ps_cube.bin", "ps_vol.bin"};
        static const float coord[3][3] = {{1.5f, 1.5f, 4.0f}, {1.5f, 1.5f, 1.0f}, {0.5f, 0.5f, 0.625f}};
        static const uint8_t want_rgb[3][3] = {{0, 255, 255}, {0, 255, 0}, {0, 0, 255}};
        for (int f = 0; f < 3; f++) {
            uint32_t nn;
            uint32_t *pp = load(argv[1], names[f], &nn);
            CHECK(pp != NULL, "no %s", names[f]);
            if (!pp)
                continue;
            im[0] = 1, im[1] = nn;
            memcpy(im + 2, pp, 4 * nn);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nn);
            free(pp);
            if (f == 0) {
                im[0] = 0, im[1] = nvs; /* the float vertex shader again (frame 16-17 loaded others) */
                memcpy(im + 2, vs, 4 * nvs);
                type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nvs);
            }
            vertex(0x7600, -1, -1, coord[f][0], coord[f][1], coord[f][2]);
            vertex(0x7620, 1, -1, coord[f][0], coord[f][1], coord[f][2]);
            vertex(0x7640, -1, 1, coord[f][0], coord[f][1], coord[f][2]);
            vertex(0x7660, 1, 1, coord[f][0], coord[f][1], coord[f][2]);
            reg(0x4800, 0x7600u | 3u);
            reg(0x4801, 2u | (32u << 2));
            uint32_t tfc[6];
            if (f < 2) /* cube: 4x4 faces, pitch 32 texels */
                tfc[0] = 2u | 2u << 10 | 2u << 13 | 1u << 22, tfc[1] = 6u | 2u << 6 | 0x24000u, tfc[2] = 3u | 3u << 13,
                tfc[5] = 3u << 9;
            else /* 3D: 4x4x4 */
                tfc[0] = 2u | 2u << 10 | 2u << 13 | 2u << 16 | 1u << 22, tfc[1] = 6u | 2u << 6 | 0x2C000u,
                tfc[2] = 3u | 3u << 11 | 3u << 22, tfc[5] = 2u << 9;
            tfc[3] = sw_id << 1, tfc[4] = 0;
            for (int i = 0; i < 6; i++)
                reg(0x480C + i, tfc[i]);
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            c = px(g, W / 2, H / 2);
            CHECK(c[0] == want_rgb[f][0] && c[1] == want_rgb[f][1] && c[2] == want_rgb[f][2],
                  "frame %d (%s): %u,%u,%u, want %u,%u,%u", 21 + f, names[f], c[0], c[1], c[2], want_rgb[f][0],
                  want_rgb[f][1], want_rgb[f][2]);
        }
    }

    /* frame 24: the viewport off, positions in pixels: a red quad over (16, 16) .. (48, 48) */
    im[0] = 1, im[1] = nps;
    memcpy(im + 2, ps, 4 * nps);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
    for (int i = 0; i < 4; i++)
        reg(0x4400 + (uint32_t)i, fbits(1.0f));
    vertex(0x7800, 16, 16, 1, 0, 0);
    vertex(0x7820, 48, 16, 1, 0, 0);
    vertex(0x7840, 16, 48, 1, 0, 0);
    vertex(0x7860, 48, 48, 1, 0, 0);
    reg(0x4800, 0x7800u | 3u);
    reg(0x4801, 2u | (32u << 2));
    reg(0x2206, 0);
    type3(PM4_DRAW_INDX_2, &strip_a, 1);
    full_viewport();
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    const uint8_t *in2 = px(g, 20, 20), *in3 = px(g, 44, 44), *out3 = px(g, 10, 10), *out4 = px(g, 52, 30);
    CHECK(in2[0] == 255 && in3[0] == 255 && out3[0] == 0 && out4[0] == 0,
          "frame 24 (screen space): (20,20) %u (44,44) %u (10,10) %u (52,30) %u", in2[0], in3[0], out3[0], out4[0]);

    /* frames 25-28 (XBA-280): the eDRAM model, float targets, mips, a computed-index vertex fetch */
    {
        /* frame 25: a 2_10_10_10 target at eDRAM tile 0x100 (pitch 64), a green quad, resolved to a tiled
         * 2_10_10_10 texture; the frame is that target (no front buffer resolve) */
        for (int i = 0; i < 4; i++)
            reg(0x4400 + (uint32_t)i, fbits(1.0f));
        reg(0x2000, 64u);                 /* RB_SURFACE_INFO: pitch 64, no MSAA */
        reg(0x2001, 0x100u | 2u << 16);   /* RB_COLOR_INFO: base 0x100, 2_10_10_10 */
        reg(0x4800, 0x4000u | 3u);
        reg(0x4801, 2u | (64u << 2));
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        reg(0x4800, 0x7000u | 3u);
        reg(0x4801, 2u | (6u << 2));
        reg(0x2208, 6);
        reg(0x2318, 0u);                  /* colour 0, no clear, raw */
        reg(0x2319, 0x30000);
        reg(0x231A, W | (uint32_t)H << 16);
        reg(0x231B, 2u | 7u << 7);        /* 8-in-32, 2_10_10_10 */
        type3(PM4_DRAW_INDX_2, &rect, 1);
        reg(0x2208, 4);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        c = px(g, W / 2, H / 2);
        uint32_t t25 = rd32(NULL, 0x30000 + tiled_off(3, 3));
        CHECK(c[0] == 0 && c[1] == 255 && c[2] == 0 && t25 == 0xC00FFC00u,
              "frame 25 (2_10_10_10 target at tile 0x100): %u,%u,%u, resolved %08X (want C00FFC00)", c[0], c[1], c[2],
              t25);

        /* frame 26: a 16_16_16_16_FLOAT target at tile 0x200: red * (2, 1, 1, 1) keeps 2.0, resolved to a tiled
         * 16_16_16_16_FLOAT texture (8 bytes a texel) */
        reg(0x2001, 0x200u | 7u << 16);
        reg(0x4400, fbits(2.0f));
        reg(0x4800, 0x4080u | 3u);
        reg(0x4801, 2u | (32u << 2));
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        reg(0x4800, 0x7000u | 3u);
        reg(0x4801, 2u | (6u << 2));
        reg(0x2208, 6);
        reg(0x2319, 0x34000);
        reg(0x231B, 2u | 32u << 7);       /* 8-in-32, 16_16_16_16_FLOAT */
        type3(PM4_DRAW_INDX_2, &rect, 1);
        reg(0x2208, 4);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        c = px(g, W / 2, H / 2);
        uint32_t o26 = tiled_off_log2(9, 4, 3), lo = rd32(NULL, 0x34000 + o26), hi = rd32(NULL, 0x34000 + o26 + 4);
        CHECK(c[0] == 255 && c[1] == 0 && lo == 0x00004000u && hi == 0x3C000000u,
              "frame 26 (float target): %u,%u,%u, resolved %08X %08X (want 00004000 3C000000)", c[0], c[1], c[2], lo,
              hi);
        reg(0x2001, 0), reg(0x2000, 0), reg(0x4400, fbits(1.0f));

        /* frame 27: mips: a linear 32x32 8_8_8_8 texture (red), its level 1 (16x16, green) and level 2 (8x8,
         * blue) from the mip address (linear mip rows are 256 bytes, a level 32 rows: 8 KB apart); a full-target quad
         * with texture coordinates 0..8 (repeat) minifies 4x: level 2, blue */
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 32; x++) {
                w32(0x3C000 + y * 128 + x * 4, 0xFF0000FFu);
                if (x < 16 && y < 16)
                    w32(0x3D000 + y * 256 + x * 4, 0xFF00FF00u);
                if (x < 8 && y < 8)
                    w32(0x3F000 + y * 256 + x * 4, 0xFFFF0000u);
            }
        uint32_t nt;
        uint32_t *pt = load(argv[1], "ps_tex.bin", &nt);
        CHECK(pt != NULL, "no ps_tex.bin");
        if (pt) {
            im[0] = 1, im[1] = nt;
            memcpy(im + 2, pt, 4 * nt);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nt);
            free(pt);
        }
        vertex(0x7A00, -1, -1, 0, 0, 0);
        vertex(0x7A20, 1, -1, 8, 0, 0);
        vertex(0x7A40, -1, 1, 0, 8, 0);
        vertex(0x7A60, 1, 1, 8, 8, 0);
        reg(0x4800, 0x7A00u | 3u);
        reg(0x4801, 2u | (32u << 2));
        uint32_t mfc[6] = {2u | 1u << 22, 6u | 2u << 6 | 0x3C000u, 31u | 31u << 13, sw_id << 1, 2u << 6,
                           1u << 9 | 0x3D000u};
        for (int i = 0; i < 6; i++)
            reg(0x480C + (uint32_t)i, mfc[i]);
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        c = px(g, W / 2, H / 2);
        /* at an internal resolution of 2 (XENOS_DRAW_SCALE) a host pixel covers half as many texels: level 1, at 3
         * or more level 0 -- the finer mips upscaling is for */
        int want_level = scale == 1 ? 2 : scale == 2 ? 1 : 0;
        CHECK(c[0] == (want_level == 0 ? 255 : 0) && c[1] == (want_level == 1 ? 255 : 0) &&
                  c[2] == (want_level == 2 ? 255 : 0),
              "frame 27 (mip level %d): %u,%u,%u", want_level, c[0], c[1], c[2]);
        /* the same with the mip filter "base map": level 0 only, however far minified */
        mfc[3] |= 2u << 23;
        for (int i = 0; i < 6; i++)
            reg(0x480C + (uint32_t)i, mfc[i]);
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        c = px(g, W / 2, H / 2);
        CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0, "frame 27 (mip filter base map): %u,%u,%u, want level 0 (red)",
              c[0], c[1], c[2]);

        /* frame 28: vs_inst: positions at the vertex index, colours from fetch constant 1 at r0.x * 0.5 (vertices
         * 0, 1 -> green, 2, 3 -> red): the bottom edge green, the top red */
        uint32_t ni;
        uint32_t *pi = load(argv[1], "vs_inst.bin", &ni);
        CHECK(pi != NULL, "no vs_inst.bin");
        if (pi) {
            im[0] = 0, im[1] = ni;
            memcpy(im + 2, pi, 4 * ni);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + ni);
            free(pi);
        }
        im[0] = 1, im[1] = nps;
        memcpy(im + 2, ps, 4 * nps);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
        static const float pos[4][4] = {{-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}};
        static const float col[2][4] = {{0, 1, 0, 1}, {1, 0, 0, 1}};
        for (int v = 0; v < 4; v++)
            for (int i = 0; i < 4; i++)
                w32(0x7A80 + 16 * (uint32_t)v + 4 * (uint32_t)i, fbits(pos[v][i]));
        for (int v = 0; v < 2; v++)
            for (int i = 0; i < 4; i++)
                w32(0x7B00 + 16 * (uint32_t)v + 4 * (uint32_t)i, fbits(col[v][i]));
        reg(0x4800, 0x7A80u | 3u);
        reg(0x4801, 2u | (16u << 2));
        reg(0x4802, 0x7B00u | 3u);
        reg(0x4803, 2u | (8u << 2));
        for (int i = 0; i < 4; i++)
            reg(0x4000 + (uint32_t)i, fbits(0.5f)); /* vertex c0 */
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        const uint8_t *top28 = px(g, W / 2, 1), *bot28 = px(g, W / 2, H - 2);
        CHECK(top28[0] > 200 && top28[1] < 50 && bot28[1] > 200 && bot28[0] < 50,
              "frame 28 (computed index): top %u,%u,%u bottom %u,%u,%u", top28[0], top28[1], top28[2], bot28[0],
              bot28[1], bot28[2]);
        /* XBA-282: vs_mx exports c5 to memory at 0x7C00 (eA = c4: base 0x7C00 in dwords, index 0, bound 1,
         * 32_32_32_32_FLOAT, 8-in-32): with GLES 3.1 vertex storage blocks the four floats land there big-endian;
         * without, the draw is skipped and counted (the fallback) */
        uint32_t nm;
        uint32_t *pm = load(argv[1], "vs_mx.bin", &nm);
        CHECK(pm != NULL, "no vs_mx.bin");
        if (pm) {
            im[0] = 0, im[1] = nm;
            memcpy(im + 2, pm, 4 * nm);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nm);
            free(pm);
            reg(0x4010, 0x40000000u | (0x7C00u >> 2));     /* c4.x: the base in dwords */
            reg(0x4011, 0x4B000000u);                      /* c4.y: index 0 */
            reg(0x4012, 0x4B000000u | 2u | 38u << 8);      /* c4.z: 8-in-32, 32_32_32_32_FLOAT */
            reg(0x4013, 0x4B000000u | 1u);                 /* c4.w: bound 1 */
            static const float mxv[4] = {1.5f, -2.0f, 0.25f, 100.0f};
            for (int i = 0; i < 4; i++)
                reg(0x4014 + (uint32_t)i, fbits(mxv[i]));
            xenos_gl_stats s0, s1;
            xenos_gl_get_stats(g, &s0);
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            xenos_gl_get_stats(g, &s1);
            if (s1.memexport_skipped > s0.memexport_skipped) {
                printf("memory export: not available here, the draw was skipped (fallback)\n");
            } else {
                int ok = s1.memexport_draws == s0.memexport_draws + 1;
                for (int i = 0; i < 4; i++)
                    ok &= rd32(NULL, 0x7C00 + 4 * (uint32_t)i) == fbits(mxv[i]);
                CHECK(ok, "memory export: draws %llu, at 0x7C00 %08X %08X %08X %08X",
                      (unsigned long long)(s1.memexport_draws - s0.memexport_draws), rd32(NULL, 0x7C00),
                      rd32(NULL, 0x7C04), rd32(NULL, 0x7C08), rd32(NULL, 0x7C0C));
                if (ok)
                    printf("memory export: c5 written to 0x7C00 (%llu dwords)\n",
                           (unsigned long long)(s1.memexport_dwords - s0.memexport_dwords));
            }
        }
        im[0] = 0, im[1] = nvs;
        memcpy(im + 2, vs, 4 * nvs);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nvs);
        run(&cp);
    }

    /* frame 29: a depth resolve: a quad at z 0.25 (depth test always, written), the depth buffer resolved to a tiled
     * 24_8 texture: depth 0.25 in the high 24 bits (0x400000), stencil 0 */
    {
        reg(0x2200, 2u | 4u | 7u << 4);
        reg(0x4800, 0x4000u | 3u);
        reg(0x4801, 2u | (64u << 2));
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        reg(0x2200, 0);
        reg(0x4800, 0x7000u | 3u);
        reg(0x4801, 2u | (6u << 2));
        reg(0x2208, 6);
        reg(0x2318, 4u); /* source: depth, no clear, raw */
        reg(0x2319, 0x30000);
        reg(0x231A, W | (uint32_t)H << 16);
        reg(0x231B, 2u | 22u << 7); /* 8-in-32, 24_8 */
        type3(PM4_DRAW_INDX_2, &rect, 1);
        reg(0x2208, 4);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        uint32_t dw = rd32(NULL, 0x30000 + tiled_off(20, 20)), dd = dw >> 8;
        CHECK(dd >= 0x3FFFF0u && dd <= 0x400010u && (dw & 0xFF) == 0, "frame 29 (depth resolve): %08X, want 400000 << 8",
              dw);
    }

    /* two PPM frames of 64x64 (header + RGB), not a third */
    char fp[1200];
    struct stat sb;
    int f0, f1, f2;
    snprintf(fp, sizeof fp, "%s/frame_00000.ppm", frames);
    f0 = stat(fp, &sb) == 0 && sb.st_size == (off_t)(strlen("P6\n64 64\n255\n") + W * H * 3);
    unlink(fp);
    snprintf(fp, sizeof fp, "%s/frame_00001.ppm", frames);
    f1 = stat(fp, &sb) == 0;
    unlink(fp);
    snprintf(fp, sizeof fp, "%s/frame_00002.ppm", frames);
    f2 = stat(fp, &sb) == 0;
    rmdir(frames);
    CHECK(f0 && f1 && !f2, "PPM frames: first %d, second %d, third (over the limit) %d", f0, f1, f2);

    xenos_gl_stats st;
    xenos_gl_get_stats(g, &st);
    CHECK(st.drawn == 36 + st.memexport_draws && st.frames == 33 && st.resolves == 6 && st.depth_resolves == 1 &&
              st.no_shader == (st.memexport_skipped ? 1u : 0u) && /* XBA-282: the memory export draw, or its fallback */
              st.unsupported == 0 &&
              st.unsupported_textures == 0 && st.computed_fetches >= 1 && st.mipmapped >= 1 && st.surfaces >= 4 &&
              st.copy_textures >= 1,
          "stats: drawn %llu frames %llu no shader %llu unsupported %llu computed %llu mipmapped %llu surfaces %llu "
          "copies %llu", (unsigned long long)st.drawn, (unsigned long long)st.frames,
          (unsigned long long)st.no_shader, (unsigned long long)st.unsupported,
          (unsigned long long)st.computed_fetches, (unsigned long long)st.mipmapped,
          (unsigned long long)st.surfaces, (unsigned long long)st.copy_textures);

    /* ---- XBA-296: each block below checks its own frames; the totals above stay those of frames 0-30 ---- */
    reg(0x2200, 0), reg(0x2205, 0), reg(0x2208, 4), reg(0x2000, 0), reg(0x2001, 0), reg(0x2102, 0);

    /* WP-9: a draw with RB_COLOR_MASK 0 and depth / stencil off cannot write: counted, not drawn
     * (RECOMP_X360_NOOP_DRAWS=keep: counted and drawn) */
    {
        xenos_gl_stats n0, n1;
        xenos_gl_get_stats(g, &n0);
        reg(0x2104, 0);
        reg(0x4800, 0x4080u | 3u);
        reg(0x4801, 2u | (64u << 2));
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        reg(0x2104, 0xFFFFu);
        run(&cp);
        xenos_gl_get_stats(g, &n1);
        const char *noop_env = getenv("RECOMP_X360_NOOP_DRAWS");
        int keep = noop_env && !strcmp(noop_env, "keep");
        CHECK(n1.noop_draws - n0.noop_draws == 1u && n1.drawn - n0.drawn == (keep ? 1u : 0u),
              "draw that cannot write: %llu counted, %llu drawn", (unsigned long long)(n1.noop_draws - n0.noop_draws),
              (unsigned long long)(n1.drawn - n0.drawn));
    }
    full_viewport();
    for (int i = 0; i < 4; i++)
        reg(0x4400 + (uint32_t)i, fbits(1.0f));
    im[0] = 1, im[1] = nps;
    memcpy(im + 2, ps, 4 * nps);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
    run(&cp);

    /* line loops: only the closing edge (last to first vertex) draws the left side of the square */
    {
        vertex(0x40000, -0.5f, -0.5f, 1, 0, 0);
        vertex(0x40020, 0.5f, -0.5f, 1, 0, 0);
        vertex(0x40040, 0.5f, 0.5f, 1, 0, 0);
        vertex(0x40060, -0.5f, 0.5f, 1, 0, 0);
        black();
        reg(0x4800, 0x40000u | 3u);
        reg(0x4801, 2u | (32u << 2));
        uint32_t loop = 12u | 2u << 6 | 4u << 16;
        type3(PM4_DRAW_INDX_2, &loop, 1);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        CHECK(px(g, 16, 32)[0] > 40 && px(g, 48, 32)[0] > 40 && px(g, 32, 16)[0] > 40 && px(g, 32, 48)[0] > 40 &&
                  px(g, 32, 32)[0] == 0,
              "line loop: left %u right %u top %u bottom %u inside %u", px(g, 16, 32)[0], px(g, 48, 32)[0],
              px(g, 32, 16)[0], px(g, 32, 48)[0], px(g, 32, 32)[0]);
        /* two loops cut by the reset index, 16-bit DMA: each closes on its own first vertex, none joins the two */
        vertex(0x40080, -0.75f, -0.5f, 1, 0, 0);
        vertex(0x400A0, -0.25f, -0.5f, 1, 0, 0);
        vertex(0x400C0, -0.25f, 0.5f, 1, 0, 0);
        vertex(0x400E0, -0.75f, 0.5f, 1, 0, 0);
        vertex(0x40100, 0.25f, -0.5f, 1, 0, 0);
        vertex(0x40120, 0.75f, -0.5f, 1, 0, 0);
        vertex(0x40140, 0.75f, 0.5f, 1, 0, 0);
        vertex(0x40160, 0.25f, 0.5f, 1, 0, 0);
        static const uint16_t cut[9] = {0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7};
        for (int i = 0; i < 9; i++)
            mem[0x40200 + 2 * i] = (uint8_t)(cut[i] >> 8), mem[0x40200 + 2 * i + 1] = (uint8_t)cut[i];
        black();
        reg(0x4800, 0x40080u | 3u);
        reg(0x4801, 2u | (64u << 2));
        reg(0x2103, 0xFFFF), reg(0x2205, 1u << 21);
        uint32_t loops[3] = {12u | 0u << 6 | 9u << 16, 0x40200, 5u | 1u << 30};
        type3(PM4_DRAW_INDX_2, loops, 3);
        reg(0x2205, 0);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        CHECK(px(g, 8, 32)[0] > 40 && px(g, 40, 32)[0] > 40 && px(g, 32, 16)[0] == 0 && px(g, 32, 48)[0] == 0,
              "two line loops: closing edges %u %u, between them %u %u", px(g, 8, 32)[0], px(g, 40, 32)[0],
              px(g, 32, 16)[0], px(g, 32, 48)[0]);
    }

    /* a quad strip of two quads covers the target (taken as a quad list its quads would be bow ties); a polygon */
    {
        static const float qs[7][2] = {{-1, -1}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 1}, {1, 1}};
        for (uint32_t i = 0; i < 7; i++)
            vertex(0x40400 + 32 * i, qs[i][0], qs[i][1], 0, 1, 0);
        black();
        reg(0x4800, 0x40400u | 3u);
        reg(0x4801, 2u | (56u << 2));
        uint32_t strip = 14u | 2u << 6 | 7u << 16; /* the seventh vertex completes no quad */
        type3(PM4_DRAW_INDX_2, &strip, 1);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        static const int at[6][2] = {{4, 4}, {4, 59}, {28, 8}, {36, 56}, {59, 4}, {59, 59}};
        for (int i = 0; i < 6; i++)
            CHECK(px(g, at[i][0], at[i][1])[1] == 255, "quad strip: (%d, %d) green %u", at[i][0], at[i][1],
                  px(g, at[i][0], at[i][1])[1]);
        static const float pg[5][2] = {{0, 0.75f}, {0.75f, 0.25f}, {0.5f, -0.75f}, {-0.5f, -0.75f}, {-0.75f, 0.25f}};
        for (uint32_t i = 0; i < 5; i++)
            vertex(0x40500 + 32 * i, pg[i][0], pg[i][1], 0, 0, 1);
        black();
        reg(0x4800, 0x40500u | 3u);
        reg(0x4801, 2u | (40u << 2));
        uint32_t poly = 15u | 2u << 6 | 5u << 16;
        type3(PM4_DRAW_INDX_2, &poly, 1);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        CHECK(px(g, 32, 32)[2] == 255 && px(g, 32, 12)[2] == 255 && px(g, 12, 26)[2] == 255 && px(g, 50, 26)[2] == 255 &&
                  px(g, 20, 54)[2] == 255 && px(g, 2, 2)[2] == 0 && px(g, 61, 61)[2] == 0,
              "polygon: centre %u top %u left %u right %u bottom %u, outside %u %u", px(g, 32, 32)[2], px(g, 32, 12)[2],
              px(g, 12, 26)[2], px(g, 50, 26)[2], px(g, 20, 54)[2], px(g, 2, 2)[2], px(g, 61, 61)[2]);
    }

    /* immediate indices: the quad of frame 1 (vertices at 0x2000), 16-bit in 8-in-16 and 32-bit in 8-in-32 */
    {
        black();
        reg(0x4800, 0x2000u | 3u);
        reg(0x4801, 2u | (32u << 2));
        reg(XENOS_REG_VGT_DMA_SIZE, 1u << 30);
        uint32_t i16[4] = {4u | 1u << 6 | 6u << 16, 0u << 16 | 1u, 2u << 16 | 2u, 1u << 16 | 3u};
        type3(PM4_DRAW_INDX_2, i16, 4);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        CHECK(px(g, 4, 4)[1] == 255 && px(g, 59, 59)[1] == 255 && px(g, 4, 59)[1] == 255 && px(g, 59, 4)[1] == 255,
              "immediate 16-bit indices: corners %u %u %u %u", px(g, 4, 4)[1], px(g, 59, 59)[1], px(g, 4, 59)[1],
              px(g, 59, 4)[1]);
        black();
        reg(0x4400, 0), reg(0x4401, fbits(0.5f));
        reg(XENOS_REG_VGT_DMA_SIZE, 2u << 30);
        uint32_t i32[7] = {4u | 1u << 6 | 1u << 11 | 6u << 16, 0, 1, 2, 2, 1, 3};
        type3(PM4_DRAW_INDX_2, i32, 7);
        reg(0x4401, fbits(1.0f)), reg(0x4400, fbits(1.0f));
        reg(XENOS_REG_VGT_DMA_SIZE, 0);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        CHECK(px(g, 4, 4)[1] >= 126 && px(g, 4, 4)[1] <= 129 && px(g, 59, 59)[1] >= 126 && px(g, 59, 59)[1] <= 129,
              "immediate 32-bit indices: corners %u %u", px(g, 4, 4)[1], px(g, 59, 59)[1]);
    }
    /* packed mip tails: every level a colour of its own, written where the layout puts it (offsets worked out by hand
     * from the rule, not with the renderer's code); each frame samples one level through setTexLOD */
    {
        static const uint32_t lc[7] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF00FFFFu,
                                       0xFFFFFF00u, 0xFFFF00FFu, 0xFFFFFFFFu};
        static const uint8_t lrgb[7][3] = {{255, 0, 0},   {0, 255, 0},   {0, 0, 255},    {255, 255, 0},
                                           {0, 255, 255}, {255, 0, 255}, {255, 255, 255}};
        uint32_t nl;
        uint32_t *pl[3];
        static const char *const lname[3] = {"ps_lod1.bin", "ps_lod2.bin", "ps_lod3.bin"};
        for (int i = 0; i < 3; i++) {
            pl[i] = load(argv[1], lname[i], &nl);
            CHECK(pl[i] != NULL, "no %s", lname[i]);
        }
        /* draw with shader s, coordinates (u, v, w), level `lod`, fetch constant f: the centre pixel's colour */
#define LOD_FRAME(s, u, v, w, lod, f)                                                                                  \
    do {                                                                                                               \
        im[0] = 1, im[1] = nl;                                                                                         \
        memcpy(im + 2, pl[s], 4 * nl);                                                                                 \
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nl);                                                                      \
        vertex(0x41000, -1, -1, u, v, w), vertex(0x41020, 1, -1, u, v, w);                                             \
        vertex(0x41040, -1, 1, u, v, w), vertex(0x41060, 1, 1, u, v, w);                                               \
        reg(0x4800, 0x41000u | 3u);                                                                                    \
        reg(0x4801, 2u | (32u << 2));                                                                                  \
        reg(0x4404, fbits((float)(lod)));                                                                              \
        for (int r_ = 0; r_ < 6; r_++)                                                                                 \
            reg(0x480C + (uint32_t)r_, (f)[r_]);                                                                       \
        type3(PM4_DRAW_INDX_2, &strip_a, 1);                                                                           \
        type3(PM4_XE_SWAP, swap, 4);                                                                                   \
        run(&cp);                                                                                                      \
    } while (0)
        /* 64x64 linear, levels 0-6: the tail starts at level 2 (16x16). Level 1: 0x60000, rows of 256 bytes, 8 KB. The
         * tail's storage at 0x62000: level 2 at x 16, level 3 at x 8, level 4 at x 4, level 5 at y 8, level 6 at y 4 */
        for (uint32_t y = 0; y < 64; y++)
            for (uint32_t x = 0; x < 64; x++)
                w32(0x50000 + y * 256 + x * 4, lc[0]);
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 32; x++)
                w32(0x60000 + y * 256 + x * 4, lc[1]);
        static const uint32_t t2d[5][3] = {{16, 0, 16}, {8, 0, 8}, {4, 0, 4}, {0, 8, 2}, {0, 4, 1}};
        for (uint32_t l = 0; l < 5; l++)
            for (uint32_t y = 0; y < t2d[l][2]; y++)
                for (uint32_t x = 0; x < t2d[l][2]; x++)
                    w32(0x62000 + (y + t2d[l][1]) * 256 + (x + t2d[l][0]) * 4, lc[2 + l]);
        uint32_t f2d[6] = {2u | 2u << 10 | 2u << 13 | 2u << 22, 6u | 2u << 6 | 0x50000u, 63u | 63u << 13, sw_id << 1,
                           6u << 6, 1u << 9 | 1u << 11 | 0x60000u};
        for (int l = 0; l < 7; l++) {
            LOD_FRAME(0, 0.5f, 0.5f, 0.0f, l, f2d);
            c = px(g, W / 2, H / 2);
            CHECK(c[0] == lrgb[l][0] && c[1] == lrgb[l][1] && c[2] == lrgb[l][2], "packed 2D level %d: %u,%u,%u", l, c[0],
                  c[1], c[2]);
        }
        /* 16x16 tiled, all tail: level 0 at x 16 of the base's tile, levels 1 and 2 at x 8 and x 4 of the mips' */
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                w32(0x68000 + tiled_off_pitch(x + 16, y, 32, 2), lc[3]);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                w32(0x69000 + tiled_off_pitch(x + 8, y, 32, 2), lc[4]);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                w32(0x69000 + tiled_off_pitch(x + 4, y, 32, 2), lc[5]);
        uint32_t f16[6] = {2u | 2u << 10 | 2u << 13 | 1u << 22 | 1u << 31, 6u | 2u << 6 | 0x68000u, 15u | 15u << 13,
                           sw_id << 1, 2u << 6, 1u << 9 | 1u << 11 | 0x69000u};
        for (int l = 0; l < 3; l++) {
            LOD_FRAME(0, 0.5f, 0.5f, 0.0f, l, f16);
            c = px(g, W / 2, H / 2);
            CHECK(c[0] == lrgb[3 + l][0] && c[1] == lrgb[3 + l][1] && c[2] == lrgb[3 + l][2],
                  "packed 16x16 level %d: %u,%u,%u", l, c[0], c[1], c[2]);
        }
        /* a 32x32 cube: level 1 (16x16) and 2 (8x8) in the tail at 0x70000, faces 8 KB apart: +Z is face 4 */
        for (uint32_t f = 0; f < 6; f++) {
            for (uint32_t y = 0; y < 32; y++)
                for (uint32_t x = 0; x < 32; x++)
                    w32(0x6A000 + f * 4096 + y * 128 + x * 4, lc[0]);
            for (uint32_t y = 0; y < 16; y++)
                for (uint32_t x = 0; x < 16; x++)
                    w32(0x70000 + f * 8192 + y * 256 + (x + 16) * 4, f == 4 ? lc[1] : lc[6]);
            for (uint32_t y = 0; y < 8; y++)
                for (uint32_t x = 0; x < 8; x++)
                    w32(0x70000 + f * 8192 + y * 256 + (x + 8) * 4, f == 4 ? lc[2] : lc[6]);
        }
        uint32_t fcu[6] = {2u | 2u << 10 | 2u << 13 | 1u << 22, 6u | 2u << 6 | 0x6A000u, 31u | 31u << 13, sw_id << 1,
                           2u << 6, 3u << 9 | 1u << 11 | 0x70000u};
        for (int l = 1; l < 3; l++) {
            LOD_FRAME(2, 1.5f, 1.5f, 4.0f, l, fcu);
            c = px(g, W / 2, H / 2);
            CHECK(c[0] == lrgb[l][0] && c[1] == lrgb[l][1] && c[2] == lrgb[l][2], "packed cube level %d: %u,%u,%u", l, c[0],
                  c[1], c[2]);
        }
        /* 32x32x4 3D: level 1 (16x16x2) at x 16 of the tail at 0x80000, its slices 8 KB apart; level 2 (8x8x1) at x 8 */
        for (uint32_t z = 0; z < 4; z++)
            for (uint32_t y = 0; y < 32; y++)
                for (uint32_t x = 0; x < 32; x++)
                    w32(0x7C000 + z * 4096 + y * 128 + x * 4, lc[0]);
        for (uint32_t z = 0; z < 2; z++)
            for (uint32_t y = 0; y < 16; y++)
                for (uint32_t x = 0; x < 16; x++)
                    w32(0x80000 + z * 8192 + y * 256 + (x + 16) * 4, z ? lc[3] : lc[6]);
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 8; x++)
                w32(0x80000 + y * 256 + (x + 8) * 4, lc[5]);
        uint32_t f3d[6] = {2u | 2u << 10 | 2u << 13 | 2u << 16 | 1u << 22, 6u | 2u << 6 | 0x7C000u,
                           31u | 31u << 11 | 3u << 22, sw_id << 1, 2u << 6, 2u << 9 | 1u << 11 | 0x80000u};
        LOD_FRAME(1, 0.5f, 0.5f, 0.75f, 1, f3d);
        c = px(g, W / 2, H / 2);
        CHECK(c[0] == lrgb[3][0] && c[1] == lrgb[3][1] && c[2] == lrgb[3][2], "packed 3D level 1, slice 1: %u,%u,%u", c[0],
              c[1], c[2]);
        LOD_FRAME(1, 0.5f, 0.5f, 0.5f, 2, f3d);
        c = px(g, W / 2, H / 2);
        CHECK(c[0] == lrgb[5][0] && c[1] == lrgb[5][1] && c[2] == lrgb[5][2], "packed 3D level 2: %u,%u,%u", c[0], c[1],
              c[2]);
        /* 64x16, 8 bytes a texel, linear, all tail: level 0 at y 16 of the base (rows of 512 bytes); levels 1 and 2 at
         * y 8 and y 4 of the mips' storage, whose rows are sized from level 0 too: 512 bytes, not 256 */
        for (uint32_t y = 0; y < 16; y++)
            for (uint32_t x = 0; x < 64; x++)
                w32(0x90000 + (y + 16) * 512 + x * 8, 0x3C000000u), w32(0x90000 + (y + 16) * 512 + x * 8 + 4, 0x3C000000u);
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 32; x++)
                w32(0x94000 + (y + 8) * 512 + x * 8, 0x00003C00u), w32(0x94000 + (y + 8) * 512 + x * 8 + 4, 0x3C000000u);
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 16; x++)
                w32(0x94000 + (y + 4) * 512 + x * 8, 0), w32(0x94000 + (y + 4) * 512 + x * 8 + 4, 0x3C003C00u);
        uint32_t fwide[6] = {2u | 2u << 10 | 2u << 13 | 2u << 22, 32u | 2u << 6 | 0x90000u, 63u | 15u << 13, sw_id << 1,
                             2u << 6, 1u << 9 | 1u << 11 | 0x94000u};
        static const int wide_want[3] = {1, 0, 2}; /* green, red, blue */
        for (int l = 0; l < 3; l++) {
            LOD_FRAME(0, 0.5f, 0.5f, 0.0f, l, fwide);
            c = px(g, W / 2, H / 2);
            const uint8_t *w_ = lrgb[wide_want[l]];
            CHECK(c[0] == w_[0] && c[1] == w_[1] && c[2] == w_[2], "packed 64x16 level %d: %u,%u,%u", l, c[0], c[1], c[2]);
        }
#undef LOD_FRAME
        for (int i = 0; i < 3; i++)
            free(pl[i]);
        im[0] = 1, im[1] = nps;
        memcpy(im + 2, ps, 4 * nps);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
        run(&cp);
    }

    /* the texture queries through the renderer: xe_tq<k> comes from the fetch constant (an 8x8 texture, X clamped to its
     * border). getWeights at (0.28125, 0.3125): fract(2.25 - 0.5 + 1.5 / 1024), fract(2.5 - 0.5 + 1.5 / 1024); getBCF at
     * u = -0.5: the whole footprint is outside */
    {
        static const char *const qn[2] = {"ps_q19.bin", "ps_q16.bin"};
        static const float qc[2][2] = {{0.28125f, 0.3125f}, {-0.5f, 0.5f}};
        uint32_t fq[6] = {2u | 6u << 10 | 2u << 13 | 1u << 22, 6u | 2u << 6 | 0x50000u, 7u | 7u << 13, sw_id << 1, 0, 1u << 9};
        for (int q = 0; q < 2; q++) {
            uint32_t nq;
            uint32_t *pq = load(argv[1], qn[q], &nq);
            CHECK(pq != NULL, "no %s", qn[q]);
            if (!pq)
                continue;
            im[0] = 1, im[1] = nq;
            memcpy(im + 2, pq, 4 * nq);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nq);
            free(pq);
            for (uint32_t k = 0; k < 4; k++)
                vertex(0x41300 + 32 * k, k & 1 ? 1.0f : -1.0f, k & 2 ? 1.0f : -1.0f, qc[q][0], qc[q][1], 0.0f);
            reg(0x4800, 0x41300u | 3u);
            reg(0x4801, 2u | (32u << 2));
            for (int i = 0; i < 6; i++)
                reg(0x480C + (uint32_t)i, fq[i]);
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            c = px(g, W / 2, H / 2);
            if (q == 0)
                CHECK(c[0] >= 190 && c[0] <= 193 && c[1] <= 2, "getWeights through the renderer: %u,%u, want 191,0", c[0], c[1]);
            else
                CHECK(c[0] == 255, "getBCF through the renderer: %u, want 255", c[0]);
        }
        im[0] = 1, im[1] = nps;
        memcpy(im + 2, ps, 4 * nps);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
        run(&cp);
    }

    /* a pixel shader exports one byte (format 8, index 1 of the dword at 0x41800): the dword's other bytes stay; without
     * GLES 3.1 fragment storage blocks the draw is skipped and counted, memory untouched */
    {
        uint32_t nm;
        uint32_t *pm = load(argv[1], "ps_mx.bin", &nm);
        CHECK(pm != NULL, "no ps_mx.bin");
        if (pm) {
            w32(0x41800, 0xAABBCCDDu);
            im[0] = 1, im[1] = nm;
            memcpy(im + 2, pm, 4 * nm);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nm);
            free(pm);
            reg(0x4410, 0x40000000u | (0x41800u >> 2)); /* c4.x: the base in dwords */
            reg(0x4411, 0x4B000000u | 1u);              /* c4.y: index 1 */
            reg(0x4412, 0x4B000000u | 2u << 8);         /* c4.z: format 8 */
            reg(0x4413, 0x4B000000u | 2u);              /* c4.w: bound 2 */
            reg(0x4414, fbits(1.0f));                   /* c5.x */
            reg(0x4800, 0x2000u | 3u);
            reg(0x4801, 2u | (32u << 2));
            xenos_gl_stats s0, s1;
            xenos_gl_get_stats(g, &s0);
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            xenos_gl_get_stats(g, &s1);
            uint32_t got = rd32(NULL, 0x41800);
            if (s1.memexport_skipped > s0.memexport_skipped)
                CHECK(got == 0xAABBCCDDu, "pixel memory export skipped, yet memory is %08X", got);
            else
                CHECK(got == 0xAAFFCCDDu && s1.memexport_draws == s0.memexport_draws + 1,
                      "pixel memory export of one byte: %08X, want AAFFCCDD", got);
            im[0] = 1, im[1] = nps;
            memcpy(im + 2, ps, 4 * nps);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
            run(&cp);
        }
    }

    /* the stencil of a depth resolve: the square of the line loops (z 0.5) writes stencil 0x5A where it covers; the 24_8
     * copy holds it in the low byte there, 0 outside. A depth surface of its own, at tile 0x300 */
    {
        reg(0x2002, 0x300u);
        reg(0x4800, 0x40000u | 3u);
        reg(0x4801, 2u | (32u << 2));
        reg(0x210D, 0x5Au | 0xFFu << 8 | 0xFFu << 16);              /* reference, mask, write mask */
        reg(0x2200, 1u | 2u | 4u | 7u << 4 | 7u << 8 | 2u << 14);  /* stencil always, replace on pass; depth always */
        uint32_t fan = 5u | 2u << 6 | 4u << 16;
        type3(PM4_DRAW_INDX_2, &fan, 1);
        reg(0x2200, 0);
        reg(0x4800, 0x7000u | 3u);
        reg(0x4801, 2u | (6u << 2));
        reg(0x2208, 6);
        reg(0x2318, 4u);                                            /* source: depth, no clear, raw */
        reg(0x2319, 0x44000);
        reg(0x231A, W | (uint32_t)H << 16);
        reg(0x231B, 2u | 22u << 7);                                 /* 8-in-32, 24_8 */
        type3(PM4_DRAW_INDX_2, &rect, 1);
        reg(0x2208, 4), reg(0x2002, 0);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        uint32_t in_s = rd32(NULL, 0x44000 + tiled_off(32, 32)), out_s = rd32(NULL, 0x44000 + tiled_off(4, 4));
        CHECK((in_s & 0xFF) == 0x5A && (in_s >> 8) >= 0x7FFFF0u && (in_s >> 8) <= 0x800010u && (out_s & 0xFF) == 0,
              "depth resolve with stencil: inside %08X (want 800000 << 8 | 5A), outside %08X", in_s, out_s);
    }

    /* WP-38: depth and stencil of a depth resolve, every word: three quads (stencil replace, depth always) leave A5 at z
     * 0.25 over the target, 3C at z 0.75 over its left half, 7E at z 0.5 over its top-right quarter (between them all 8
     * stencil bits); the rectangle (8, 16)-(40, 56) is resolved to a 24_8 copy prefilled with a marker. Inside, each
     * word is its region's depth << 8 | stencil (the same word over a region, stencil exact); outside, the marker */
    {
        static const struct {
            float x0, y0, x1, y1, z;
            uint32_t ref;
        } q[3] = {{-1, -1, 1, 1, 0.25f, 0xA5}, {-1, -1, 0, 1, 0.75f, 0x3C}, {0, 0, 1, 1, 0.5f, 0x7E}};
        reg(0x2002, 0x300u);
        reg(0x2200, 1u | 2u | 4u | 7u << 4 | 7u << 8 | 2u << 14); /* stencil always, replace on pass; depth always */
        for (int k = 0; k < 3; k++) {
            uint32_t at = 0x7D80 + 128 * (uint32_t)k;
            vertex_z(at, q[k].x0, q[k].y0, q[k].z, 1, 1, 1), vertex_z(at + 32, q[k].x1, q[k].y0, q[k].z, 1, 1, 1);
            vertex_z(at + 64, q[k].x0, q[k].y1, q[k].z, 1, 1, 1), vertex_z(at + 96, q[k].x1, q[k].y1, q[k].z, 1, 1, 1);
            reg(0x210D, q[k].ref | 0xFFu << 8 | 0xFFu << 16);
            reg(0x4800, at | 3u);
            reg(0x4801, 2u | (32u << 2));
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
        }
        reg(0x2200, 0);
        static const float drv[6] = {8, 16, 40, 16, 40, 56};
        for (int i = 0; i < 6; i++)
            w32(0x7D00 + 4 * (uint32_t)i, fbits(drv[i]));
        for (uint32_t a = 0xC0000; a < 0xC0000 + W * H * 4; a += 4)
            w32(a, 0xDEADBEEFu);
        reg(0x4800, 0x7D00u | 3u);
        reg(0x4801, 2u | (6u << 2));
        reg(0x2208, 6);
        reg(0x2318, 4u);            /* source: depth, no clear, raw */
        reg(0x2319, 0xC0000);
        reg(0x231A, W | (uint32_t)H << 16);
        reg(0x231B, 2u | 22u << 7); /* 8-in-32, 24_8 */
        type3(PM4_DRAW_INDX_2, &rect, 1);
        reg(0x2208, 4), reg(0x2002, 0);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        /* the region's word (taken at a pixel well inside it), by region: 0 A5, 1 3C, 2 7E */
        uint32_t want[3] = {rd32(NULL, 0xC0000 + tiled_off(36, 50)), rd32(NULL, 0xC0000 + tiled_off(12, 40)),
                            rd32(NULL, 0xC0000 + tiled_off(36, 20))};
        static const uint32_t zw[3] = {0x400000u, 0xC00000u, 0x800000u};
        for (int k = 0; k < 3; k++) {
            uint32_t d = want[k] >> 8;
            CHECK((want[k] & 0xFF) == q[k].ref && d + 16 >= zw[k] && d <= zw[k] + 16,
                  "depth + stencil resolve, region %d: %08X, want %06X << 8 | %02X", k, want[k], zw[k], q[k].ref);
        }
        int bad = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                if (x == 31 || x == 32 || y == 31 || y == 32) /* the quads' edges run through pixel centres */
                    continue;
                int in = x >= 8 && x < 40 && y >= 16 && y < 56, region = x < 32 ? 1 : y < 32 ? 2 : 0;
                uint32_t got = rd32(NULL, 0xC0000 + tiled_off(x, y)), exp = in ? want[region] : 0xDEADBEEFu;
                if (got != exp && bad++ < 4)
                    CHECK(0, "depth + stencil resolve at (%d, %d): %08X, want %08X", x, y, got, exp);
            }
        CHECK(!bad, "depth + stencil resolve: %d words wrong", bad);
    }

    /* eDRAM aliasing: draw into one format, resolve the same tiles as another: the raw bits must come through */
    {
        /* resolve render target 0 (64 x 64) to `dest` in texture format `tf` */
#define RESOLVE_TO(dest, tf)                                                                                           \
    do {                                                                                                               \
        reg(0x4800, 0x7000u | 3u);                                                                                     \
        reg(0x4801, 2u | (6u << 2));                                                                                   \
        reg(0x2208, 6);                                                                                                \
        reg(0x2318, 0u);                                                                                               \
        reg(0x2319, dest);                                                                                             \
        reg(0x231A, W | (uint32_t)H << 16);                                                                            \
        reg(0x231B, 2u | (uint32_t)(tf) << 7);                                                                         \
        type3(PM4_DRAW_INDX_2, &rect, 1);                                                                              \
        reg(0x2208, 4);                                                                                                \
    } while (0)
#define FILL(r_, g_, b_, a_)                                                                                           \
    do {                                                                                                               \
        reg(0x4400, fbits(r_)), reg(0x4401, fbits(g_)), reg(0x4402, fbits(b_)), reg(0x4403, fbits(a_));                \
        vertex(0x41100, -1, -1, 1, 1, 1), vertex(0x41120, 1, -1, 1, 1, 1);                                             \
        vertex(0x41140, -1, 1, 1, 1, 1), vertex(0x41160, 1, 1, 1, 1, 1);                                               \
        reg(0x4800, 0x41100u | 3u);                                                                                    \
        reg(0x4801, 2u | (32u << 2));                                                                                  \
        type3(PM4_DRAW_INDX_2, &strip_a, 1);                                                                           \
    } while (0)
        xenos_gl_stats a0, a1;
        reg(0x2002, 0x700u); /* a depth buffer out of the way */
        reg(0x2000, 64u);
        /* 8_8_8_8 (FF, 00, 00, FF) read as 2_10_10_10: the same dword */
        reg(0x2001, 0x400u | 0u << 16);
        FILL(1.0f, 0.0f, 0.0f, 1.0f);
        reg(0x2001, 0x400u | 2u << 16);
        RESOLVE_TO(0x46000u, 7);
        run(&cp);
        uint32_t v = rd32(NULL, 0x46000 + tiled_off(9, 9));
        CHECK(v == 0xFF0000FFu, "8_8_8_8 read as 2_10_10_10: %08X, want FF0000FF", v);
        /* 8_8_8_8 (00, 3C, 00, 3C) read as 16_16_FLOAT: two halves of 1.0 */
        reg(0x2001, 0x420u | 0u << 16);
        FILL(0.0f, 60.0f / 255.0f, 0.0f, 60.0f / 255.0f);
        reg(0x2001, 0x420u | 6u << 16);
        RESOLVE_TO(0x48000u, 31);
        run(&cp);
        v = rd32(NULL, 0x48000 + tiled_off(9, 9));
        CHECK(v == 0x3C003C00u, "8_8_8_8 read as 16_16_FLOAT: %08X, want 3C003C00", v);
        /* the same format one tile row further on: rows 0-47 of the later surface are rows 16-63 of the earlier */
        reg(0x2001, 0x441u);
        FILL(0.0f, 1.0f, 0.0f, 1.0f);
        reg(0x2001, 0x440u);
        FILL(1.0f, 0.0f, 0.0f, 1.0f);
        reg(0x2001, 0x441u);
        RESOLVE_TO(0x4A000u, 6);
        run(&cp);
        uint32_t top = rd32(NULL, 0x4A000 + tiled_off(3, 3)), low = rd32(NULL, 0x4A000 + tiled_off(3, 60));
        CHECK(top == 0xFF0000FFu && low == 0xFF00FF00u, "a surface one tile row on: top %08X (want FF0000FF), bottom %08X "
              "(want FF00FF00)", top, low);
        /* another pitch over the same tiles cannot be carried over: green must not survive, and it is counted */
        xenos_gl_get_stats(g, &a0);
        reg(0x2000, 128u), reg(0x2001, 0x480u);
        FILL(0.0f, 1.0f, 0.0f, 1.0f);
        reg(0x2000, 64u);
        FILL(1.0f, 1.0f, 1.0f, 1.0f);
        reg(0x2000, 128u);
        RESOLVE_TO(0x4C000u, 6);
        reg(0x2000, 64u), reg(0x2001, 0), reg(0x2002, 0), reg(0x2000, 0);
        for (int i = 0; i < 4; i++)
            reg(0x4400 + (uint32_t)i, fbits(1.0f));
        run(&cp);
        xenos_gl_get_stats(g, &a1);
        v = rd32(NULL, 0x4C000 + tiled_off(5, 5));
        CHECK((v & 0x0000FF00u) == 0 && a1.edram_stale > a0.edram_stale,
              "another pitch: %08X must not be the stale green, %llu overlaps counted", v,
              (unsigned long long)(a1.edram_stale - a0.edram_stale));
        CHECK(a1.edram_reinterpreted >= 3, "reinterpreted surfaces: %llu", (unsigned long long)a1.edram_reinterpreted);
        /* the scissor left at its maximum says nothing of a draw's extent: two targets at disjoint bases stay apart
         * (of different pitches: an overlap between those would clear the rows) */
        reg(0x2002, 0x700u), reg(0x2082, 8191u | 8191u << 16);
        reg(0x2000, 64u), reg(0x2001, 0x610u);
        FILL(1.0f, 0.0f, 0.0f, 1.0f);
        reg(0x2000, 128u), reg(0x2001, 0x620u);
        FILL(0.0f, 0.0f, 1.0f, 1.0f);
        reg(0x2000, 64u), reg(0x2001, 0x610u);
        FILL(1.0f, 0.0f, 0.0f, 1.0f);
        reg(0x2000, 128u), reg(0x2001, 0x620u);
        RESOLVE_TO(0x4D000u, 6);
        reg(0x2082, 0), reg(0x2001, 0), reg(0x2002, 0), reg(0x2000, 0);
        for (int i = 0; i < 4; i++)
            reg(0x4400 + (uint32_t)i, fbits(1.0f));
        run(&cp);
        v = rd32(NULL, 0x4D000 + tiled_off(5, 5));
        CHECK(v == 0xFFFF0000u, "a neighbour under a maximum scissor: %08X, want FFFF0000 (blue)", v);
        /* blending on a 32_FLOAT target (R32F): 0.25 filled, then 0.75 drawn with src * 0 + dst * 1. With
         * EXT_float_blend the target keeps 0.25; without it (RECOMP_X360_FLOAT_BLEND=0, xenos_draw_x2) blending is
         * off on that target and the draw lands (0.75) rather than being lost to GL_INVALID_OPERATION */
        {
            const char *fb = getenv("RECOMP_X360_FLOAT_BLEND");
            int no_fb = fb && !strcmp(fb, "0");
            reg(0x2002, 0x700u), reg(0x2000, 64u), reg(0x2001, 0x6A0u | 14u << 16);
            FILL(0.25f, 0.0f, 0.0f, 1.0f);
            reg(0x2201, 0u | 1u << 8 | 0u << 16 | 1u << 24);
            FILL(0.75f, 0.0f, 0.0f, 1.0f);
            reg(0x2201, 0x00010001u);
            RESOLVE_TO(0xA4000u, 36); /* 32_FLOAT */
            reg(0x2001, 0), reg(0x2002, 0), reg(0x2000, 0);
            for (int i = 0; i < 4; i++)
                reg(0x4400 + (uint32_t)i, fbits(1.0f));
            run(&cp);
            v = rd32(NULL, 0xA4000 + tiled_off(5, 5));
            CHECK(v == fbits(0.75f) || (!no_fb && v == fbits(0.25f)),
                  "blending on a 32_FLOAT target%s: %08X, want %08X", no_fb ? " without EXT_float_blend" : "", v,
                  fbits(no_fb ? 0.75f : 0.25f));
        }
#undef RESOLVE_TO
#undef FILL
    }

    /* MSAA: a 4x target at tile 0x500: a triangle's diagonal edge resolves to values between red and black where the
     * driver gives multisampled buffers; inside and outside are exact either way (RECOMP_X360_MSAA=0: the fallback) */
    {
        xenos_gl_stats m0, m1;
        xenos_gl_get_stats(g, &m0);
        reg(0x2000, 64u | 2u << 16), reg(0x2001, 0x500u), reg(0x2002, 0x540u);
        black();
        vertex(0x41200, -1, -1, 1, 0, 0), vertex(0x41220, 1, -1, 1, 0, 0), vertex(0x41240, -1, 1, 1, 0, 0);
        reg(0x4800, 0x41200u | 3u);
        reg(0x4801, 2u | (24u << 2));
        type3(PM4_DRAW_INDX_2, &init, 1);
        reg(0x4800, 0x7000u | 3u);
        reg(0x4801, 2u | (6u << 2));
        reg(0x2208, 6);
        reg(0x2318, 0u);
        reg(0x2319, 0x4E000);
        reg(0x231A, W | (uint32_t)H << 16);
        reg(0x231B, 2u | 6u << 7);
        type3(PM4_DRAW_INDX_2, &rect, 1);
        reg(0x2208, 4);
        reg(0x2000, 0), reg(0x2001, 0), reg(0x2002, 0);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        xenos_gl_get_stats(g, &m1);
        int between = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint32_t r = rd32(NULL, 0x4E000 + tiled_off(x, y)) & 0xFF;
                between += r > 20 && r < 235;
            }
        uint32_t inside = rd32(NULL, 0x4E000 + tiled_off(8, 55)) & 0xFF, outside = rd32(NULL, 0x4E000 + tiled_off(55, 8)) & 0xFF;
        CHECK(inside == 255 && outside == 0, "MSAA target: inside %u, outside %u", inside, outside);
        if (m1.msaa_surfaces > m0.msaa_surfaces)
            CHECK(between >= 16, "MSAA target: %d edge pixels between red and black", between);
        else
            printf("MSAA: no multisampled buffers here, drawn single-sampled (fallback)\n");
    }

    /* three textures in one draw (ps_tex3 adds them up): 1x1 red, green and blue ones, uploaded during the same draw,
     * then all three rewritten as a video's planes are every frame: each sampler reads its own texture */
    {
        uint32_t n3;
        uint32_t *p3 = load(argv[1], "ps_tex3.bin", &n3);
        CHECK(p3 != NULL, "no ps_tex3.bin");
        if (p3) {
            im[0] = 1, im[1] = n3;
            memcpy(im + 2, p3, 4 * n3);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + n3);
            free(p3);
            for (uint32_t k = 0; k < 4; k++)
                vertex(0x41300 + 32 * k, k & 1 ? 1.0f : -1.0f, k & 2 ? 1.0f : -1.0f, 0.5f, 0.5f, 0.0f);
            static const uint32_t planes[2][3] = {{0xFF000040u, 0xFF008000u, 0xFFC00000u},
                                                  {0xFF0000C0u, 0xFF004000u, 0xFF800000u}};
            for (int f = 0; f < 2; f++) {
                for (uint32_t t = 0; t < 3; t++) {
                    w32(0x52000 + 0x1000 * t, planes[f][t]);
                    uint32_t ft[6] = {2u | 2u << 10 | 2u << 13 | 1u << 22, 6u | 2u << 6 | (0x52000u + 0x1000u * t), 0,
                                      sw_id << 1, 0, 1u << 9};
                    for (uint32_t i = 0; i < 6; i++)
                        reg(0x480C + 6 * t + i, ft[i]);
                }
                reg(0x4800, 0x41300u | 3u); /* the quad covers the target: nothing drawn before it with this shader */
                reg(0x4801, 2u | (32u << 2));
                type3(PM4_DRAW_INDX_2, &strip_a, 1);
                type3(PM4_XE_SWAP, swap, 4);
                run(&cp);
                const uint8_t *c3 = px(g, W / 2, H / 2);
                uint8_t want[3] = {(uint8_t)planes[f][0], (uint8_t)(planes[f][1] >> 8), (uint8_t)(planes[f][2] >> 16)};
                CHECK(c3[0] == want[0] && c3[1] == want[1] && c3[2] == want[2],
                      "three textures, pass %d: %u,%u,%u, want %u,%u,%u", f, c3[0], c3[1], c3[2], want[0], want[1],
                      want[2]);
            }
        }
        im[0] = 1, im[1] = nps;
        memcpy(im + 2, ps, 4 * nps);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
        run(&cp);
    }

    /* DXT colour endpoints are S3TC's: red in the 5 high bits, also behind DXT2_3's and DXT4_5's alpha. One block
     * each, colour 0 on every texel: 0x001F is blue, 0xFFE0 yellow, 0xF81F magenta */
    if (pst) {
        im[0] = 1, im[1] = ntex;
        memcpy(im + 2, pst, 4 * ntex);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + ntex);
        for (uint32_t k = 0; k < 4; k++)
            vertex(0x41300 + 32 * k, k & 1 ? 1.0f : -1.0f, k & 2 ? 1.0f : -1.0f, 0.5f, 0.5f, 0.0f);
        static const struct {
            uint32_t format;
            uint16_t colour;
            uint8_t want[3];
        } dx[3] = {{18, 0x001F, {0, 0, 255}}, {19, 0xFFE0, {255, 255, 0}}, {20, 0xF81F, {255, 0, 255}}};
        for (int k = 0; k < 3; k++) {
            uint32_t at = 0x55000, cb = dx[k].format == 18 ? at : at + 8;
            for (uint32_t i = 0; i < 16; i++)
                mem[at + i] = dx[k].format == 18 ? 0 : 0xFF; /* the alpha half: opaque */
            mem[cb] = (uint8_t)(dx[k].colour >> 8), mem[cb + 1] = (uint8_t)dx[k].colour; /* 8-in-16: big-endian words */
            for (uint32_t i = 2; i < 8; i++)
                mem[cb + i] = 0;
            uint32_t fx[6] = {2u | 2u << 10 | 2u << 13 | 1u << 22, dx[k].format | 1u << 6 | at, 3u | 3u << 13, sw_id << 1,
                              0, 1u << 9};
            for (uint32_t i = 0; i < 6; i++)
                reg(0x480C + i, fx[i]);
            reg(0x4800, 0x41300u | 3u);
            reg(0x4801, 2u | (32u << 2));
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            const uint8_t *cd = px(g, W / 2, H / 2);
            CHECK(cd[0] == dx[k].want[0] && cd[1] == dx[k].want[1] && cd[2] == dx[k].want[2],
                  "DXT colour %04X (format %u): %u,%u,%u, want %u,%u,%u", dx[k].colour, dx[k].format, cd[0], cd[1],
                  cd[2], dx[k].want[0], dx[k].want[1], dx[k].want[2]);
        }
        im[0] = 1, im[1] = nps;
        memcpy(im + 2, ps, 4 * nps);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
        run(&cp);
    }

    /* a resolve that exchanges red and blue (Direct3D's A8R8G8B8 destinations): memory has the red quad's red in Z,
     * and the texture is still sampled from the resolve's host picture (no trip through memory, the internal
     * resolution kept) with the two exchanged back: the fetch constant's swizzle sees what memory holds */
    if (pst) {
        xenos_gl_stats c0, c1;
        im[0] = 1, im[1] = nps;
        memcpy(im + 2, ps, 4 * nps);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
        reg(0x4800, 0x4080u | 3u); /* the red quad */
        reg(0x4801, 2u | (32u << 2));
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        reg(0x4800, 0x7000u | 3u); /* the resolve rectangle of frame 12 */
        reg(0x4801, 2u | (6u << 2));
        reg(0x2208, 6);
        reg(0x2318, 0u | 0u << 20);                    /* colour 0, no clear, raw copy */
        reg(0x2319, 0x58000);
        reg(0x231A, W | (uint32_t)H << 16);
        reg(0x231B, 2u | 6u << 7 | 1u << 24);          /* 8-in-32, 8_8_8_8, red and blue exchanged */
        uint32_t rect2 = 8u | 2u << 6 | 3u << 16;
        type3(PM4_DRAW_INDX_2, &rect2, 1);
        reg(0x2208, 4);
        run(&cp);
        uint32_t sw = rd32(NULL, 0x58000 + tiled_off(37, 21));
        CHECK(sw == 0xFFFF0000u, "resolved with the swap: texel %08X, want FFFF0000", sw);
        im[0] = 1, im[1] = ntex;
        memcpy(im + 2, pst, 4 * ntex);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + ntex);
        reg(0x4800, 0x6000u | 3u);
        reg(0x4801, 2u | (32u << 2));
        static const struct {
            int exchange;
            uint8_t want[3];
        } sx[2] = {{1, {255, 0, 0}}, {0, {0, 0, 255}}};
        xenos_gl_get_stats(g, &c0);
        for (int k = 0; k < 2; k++) {
            uint32_t fs[6] = {2u | 2u << 10 | 2u << 13 | 2u << 22 | 1u << 31, 6u | 2u << 6 | 0x58000u, 63u | 63u << 13,
                              (sx[k].exchange ? sw_rb : sw_id) << 1, 0, 1u << 9};
            for (int i = 0; i < 6; i++)
                reg(0x480C + (uint32_t)i, fs[i]);
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            const uint8_t *cs = px(g, W / 2, H / 2);
            CHECK(cs[0] == sx[k].want[0] && cs[1] == sx[k].want[1] && cs[2] == sx[k].want[2],
                  "swapped resolve sampled (swizzle %s): %u,%u,%u, want %u,%u,%u", sx[k].exchange ? "BGRA" : "RGBA",
                  cs[0], cs[1], cs[2], sx[k].want[0], sx[k].want[1], sx[k].want[2]);
        }
        xenos_gl_get_stats(g, &c1);
        CHECK(c1.copy_textures == c0.copy_textures + 2, "sampled from the resolve's host picture: +%llu, want +2",
              (unsigned long long)(c1.copy_textures - c0.copy_textures));
        im[0] = 1, im[1] = nps;
        memcpy(im + 2, ps, 4 * nps);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
        run(&cp);
    }

    /* a resolved texture reads as its memory does, host picture or not. An orange quad (1, 0.5, 0) resolved (a) to
     * an 8-bit destination: X is in every component, so the swizzle's Y is the red, not the surface's green; (b) with
     * an exponent bias of -1: half of each; (c) 8-in-32, then fetched with no endian swap: the texel's bytes the
     * other way round, alpha in X and red in W */
    if (pst) {
        static const struct {
            const char *what;
            uint32_t base, info, format, fetch_endian, swizzle;
            uint8_t lo[3], hi[3];
        } rk[3] = {
            {"to 8 bits", 0x5C000, 0u | 2u << 7, 2, 0, 1u | 4u << 3 | 4u << 6 | 5u << 9, {255, 0, 0}, {255, 0, 0}},
            {"with an exponent bias", 0x60000, 2u | 6u << 7 | 0x3Fu << 16, 6, 2, 0u | 1u << 3 | 2u << 6 | 3u << 9,
             {127, 63, 0}, {128, 64, 0}},
            {"fetched with another endian mode", 0x64000, 2u | 6u << 7, 6, 0, 0u | 1u << 3 | 2u << 6 | 3u << 9,
             {255, 0, 127}, {255, 0, 128}}};
        for (uint32_t k = 0; k < 4; k++)
            vertex(0x41500 + 32 * k, k & 1 ? 1.0f : -1.0f, k & 2 ? -1.0f : 1.0f, 1.0f, 0.5f, 0.0f);
        for (int k = 0; k < 3; k++) {
            im[0] = 1, im[1] = nps;
            memcpy(im + 2, ps, 4 * nps);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
            reg(0x4800, 0x41500u | 3u); /* the orange quad */
            reg(0x4801, 2u | (32u << 2));
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            reg(0x4800, 0x7000u | 3u);
            reg(0x4801, 2u | (6u << 2));
            reg(0x2208, 6);
            reg(0x2318, 0u | 0u << 20);
            reg(0x2319, rk[k].base);
            reg(0x231A, W | (uint32_t)H << 16);
            reg(0x231B, rk[k].info);
            uint32_t rect3 = 8u | 2u << 6 | 3u << 16;
            type3(PM4_DRAW_INDX_2, &rect3, 1);
            reg(0x2208, 4);
            im[0] = 1, im[1] = ntex;
            memcpy(im + 2, pst, 4 * ntex);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + ntex);
            reg(0x4800, 0x6000u | 3u);
            reg(0x4801, 2u | (32u << 2));
            uint32_t fk[6] = {2u | 2u << 10 | 2u << 13 | 2u << 22 | 1u << 31,
                              rk[k].format | rk[k].fetch_endian << 6 | rk[k].base, 63u | 63u << 13, rk[k].swizzle << 1,
                              0, 1u << 9};
            for (int i = 0; i < 6; i++)
                reg(0x480C + (uint32_t)i, fk[i]);
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            const uint8_t *ck = px(g, W / 2, H / 2);
            int ok = 1;
            for (int ch = 0; ch < 3; ch++)
                ok &= ck[ch] >= rk[k].lo[ch] && ck[ch] <= rk[k].hi[ch];
            CHECK(ok, "resolve %s read back: %u,%u,%u, want %u..%u,%u..%u,%u..%u", rk[k].what, ck[0], ck[1], ck[2],
                  rk[k].lo[0], rk[k].hi[0], rk[k].lo[1], rk[k].hi[1], rk[k].lo[2], rk[k].hi[2]);
        }
        im[0] = 1, im[1] = nps;
        memcpy(im + 2, ps, 4 * nps);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
        run(&cp);
    }

    /* a colour target that grows keeps its depth buffer: a green quad at z 0.25 fills a 64-row depth buffer through
     * one colour target, then a second colour target is drawn 32 rows tall with that depth buffer, then 64 rows tall
     * (it grows; the depth buffer does not): a red quad at z 0.75 with the test "less" must not show */
    {
        vertex_z(0x41700, -1, -1, 0.25f, 0, 1, 0), vertex_z(0x41720, 1, -1, 0.25f, 0, 1, 0);
        vertex_z(0x41740, -1, 1, 0.25f, 0, 1, 0), vertex_z(0x41760, 1, 1, 0.25f, 0, 1, 0);
        vertex_z(0x41780, -1, -1, 0.75f, 1, 0, 0), vertex_z(0x417A0, 1, -1, 0.75f, 1, 0, 0);
        vertex_z(0x417C0, -1, 1, 0.75f, 1, 0, 0), vertex_z(0x417E0, 1, 1, 0.75f, 1, 0, 0);
        for (uint32_t i = 0; i < 4; i++)
            reg(0x4400 + i, fbits(1.0f));
        reg(0x2000, 64u), reg(0x2002, 0x7A0u);
        reg(0x2200, 2u | 4u | 1u << 4); /* z test, z write, less */
        reg(0x4801, 2u | (64u << 2));
        reg(0x2001, 0x660u), reg(0x2082, 64u | 64u << 16);
        reg(0x4800, 0x41700u | 3u);
        type3(PM4_DRAW_INDX_2, &strip_a, 1); /* depth 0.25 everywhere */
        reg(0x2001, 0x640u), reg(0x2082, 64u | 32u << 16);
        reg(0x2200, 2u | 4u | 3u << 4);      /* less or equal: the same quad again passes */
        type3(PM4_DRAW_INDX_2, &strip_a, 1); /* the second target, 32 rows, green */
        reg(0x2200, 2u | 4u | 1u << 4);
        reg(0x2082, 64u | 64u << 16);
        reg(0x4800, 0x41780u | 3u);
        type3(PM4_DRAW_INDX_2, &strip_a, 1); /* it grows; red behind what the depth buffer holds */
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        const uint8_t *cg = px(g, W / 2, 8), *ch = px(g, W / 2, H - 8);
        CHECK(cg[0] == 0 && cg[1] == 255 && ch[0] == 0 && ch[1] == 0,
              "depth kept when the colour target grows: %u,%u,%u (want green) / %u,%u,%u (want black)", cg[0], cg[1],
              cg[2], ch[0], ch[1], ch[2]);
        reg(0x2200, 0), reg(0x2082, 0), reg(0x2001, 0), reg(0x2002, 0), reg(0x2000, 0);
        run(&cp);
    }

    /* a texture whose fetch constant starts at mip level 1 (a streamed texture: the mips are loaded, the base is not,
     * and the base address names the mip address again): a linear 32x32 8_8_8_8 texture, level 1 (16x16, green) at
     * the mip address; magnified, the sampler would take level 0, and reads level 1 */
    if (pst) {
        for (uint32_t y = 0; y < 16; y++)
            for (uint32_t x = 0; x < 16; x++)
                w32(0x68000 + y * 256 + x * 4, 0xFF00FF00u);
        im[0] = 1, im[1] = ntex;
        memcpy(im + 2, pst, 4 * ntex);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + ntex);
        reg(0x4800, 0x6000u | 3u);
        reg(0x4801, 2u | (32u << 2));
        uint32_t fm[6] = {2u | 2u << 10 | 2u << 13 | 1u << 22, 6u | 2u << 6 | 0x68000u, 31u | 31u << 13, sw_id << 1,
                          1u << 2 | 1u << 6, 1u << 9 | 0x68000u};
        for (int i = 0; i < 6; i++)
            reg(0x480C + (uint32_t)i, fm[i]);
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        const uint8_t *cm = px(g, W / 2, H / 2), *cn = px(g, W - 4, H - 4);
        CHECK(cm[0] == 0 && cm[1] == 255 && cm[2] == 0 && cn[1] == 255,
              "first mip level 1: centre %u,%u,%u corner %u,%u,%u, want green", cm[0], cm[1], cm[2], cn[0], cn[1], cn[2]);
        im[0] = 1, im[1] = nps;
        memcpy(im + 2, ps, 4 * nps);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
        run(&cp);
    }

    /* interpolator 0's z reaches the pixel shader with the pixel parameters off: ps_i0z writes r0.xyw, then puts
     * r0.z (the quad's blue, 0.75) in every channel */
    {
        uint32_t nz;
        uint32_t *pz = load(argv[1], "ps_i0z.bin", &nz);
        CHECK(pz != NULL, "no ps_i0z.bin");
        if (pz) {
            im[0] = 1, im[1] = nz;
            memcpy(im + 2, pz, 4 * nz);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nz);
            free(pz);
            uint32_t nvz;
            uint32_t *vz = load(argv[1], "vs_i0xyz.bin", &nvz); /* interpolator 0's w is never written */
            CHECK(vz != NULL, "no vs_i0xyz.bin");
            if (vz) {
                im[0] = 0, im[1] = nvz;
                memcpy(im + 2, vz, 4 * nvz);
                type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nvz);
                free(vz);
            }
            w32(0x52000, 0xFF102030u); /* the texture the shader fetches into r0.w: 1x1 */
            uint32_t fz[6] = {2u | 2u << 10 | 2u << 13 | 1u << 22, 6u | 2u << 6 | 0x52000u, 0, sw_id << 1, 0, 1u << 9};
            for (uint32_t i = 0; i < 6; i++)
                reg(0x480C + i, fz[i]);
            for (uint32_t k = 0; k < 4; k++)
                vertex(0x41600 + 32 * k, k & 1 ? 1.0f : -1.0f, k & 2 ? -1.0f : 1.0f, 0.25f, 0.5f, 0.75f);
            reg(0x2180, 0), reg(0x2181, 0);
            for (uint32_t i = 0; i < 4; i++)
                reg(0x4400 + i, fbits(0.125f));
            reg(0x4800, 0x41600u | 3u);
            reg(0x4801, 2u | (32u << 2));
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            const uint8_t *cz = px(g, W / 2, H / 2);
            CHECK(cz[0] >= 190 && cz[0] <= 192 && cz[1] == cz[0] && cz[2] == cz[0],
                  "interpolator 0's z: %u,%u,%u, want 191 in each", cz[0], cz[1], cz[2]);
        }
        for (uint32_t i = 0; i < 4; i++)
            reg(0x4400 + i, fbits(1.0f));
        im[0] = 0, im[1] = nvs;
        memcpy(im + 2, vs, 4 * nvs);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nvs);
        im[0] = 1, im[1] = nps;
        memcpy(im + 2, ps, 4 * nps);
        type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
        run(&cp);
    }

    /* no limit on programs: 300 pixel shaders missing from the cache (each a (vs, ps) pair the renderer keeps, as
     * failed), then a pair never seen (vs_i0xyz + ps, both in the cache) still draws: frame 1's green quad */
    {
        xenos_gl_stats s0, s1;
        xenos_gl_get_stats(g, &s0);
        FILE *quiet = fopen("/dev/null", "w");
        xenos_gl_set_log(g, quiet); /* 300 "not in the shader cache" lines */
        for (uint32_t i = 0; i < 300; i++) {
            im[0] = 1, im[1] = nps + 1;
            memcpy(im + 2, ps, 4 * nps);
            im[2 + nps] = 0xC0DE0000u | i; /* a word more: a crc the cache has no shader for */
            type3(PM4_IM_LOAD_IMMEDIATE, im, 3 + nps);
            reg(0x4800, 0x2000u | 3u);
            reg(0x4801, 2u | (32u << 2));
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            run(&cp);
        }
        xenos_gl_set_log(g, NULL);
        if (quiet)
            fclose(quiet);
        xenos_gl_get_stats(g, &s1);
        CHECK(s1.no_shader == s0.no_shader + 300 && s1.drawn == s0.drawn, "300 missing pixel shaders: no_shader +%llu, drawn +%llu, unsupported +%llu",
              (unsigned long long)(s1.no_shader - s0.no_shader), (unsigned long long)(s1.drawn - s0.drawn),
              (unsigned long long)(s1.unsupported - s0.unsupported));
        uint32_t nvz;
        uint32_t *vz = load(argv[1], "vs_i0xyz.bin", &nvz);
        CHECK(vz != NULL, "no vs_i0xyz.bin");
        if (vz) {
            im[0] = 0, im[1] = nvz;
            memcpy(im + 2, vz, 4 * nvz);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nvz);
            free(vz);
            im[0] = 1, im[1] = nps;
            memcpy(im + 2, ps, 4 * nps);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
            black();
            reg(0x4800, 0x2000u | 3u);
            reg(0x4801, 2u | (32u << 2));
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            xenos_gl_stats s2;
            xenos_gl_get_stats(g, &s2);
            const uint8_t *cp0 = px(g, W / 2, H / 2);
            CHECK(s2.no_shader == s1.no_shader && cp0[0] == 0 && cp0[1] == 255 && cp0[2] == 0,
                  "program 301+: no_shader +%llu, centre %u,%u,%u, want green",
                  (unsigned long long)(s2.no_shader - s1.no_shader), cp0[0], cp0[1], cp0[2]);
            im[0] = 0, im[1] = nvs;
            memcpy(im + 2, vs, 4 * nvs);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nvs);
            run(&cp);
        }
    }

    /* an 8_8_8_8_GAMMA target keeps what the shader writes gamma-encoded, and the guest shows those bytes (a resolve
     * copies them to memory, the front buffer is scanned out through the gamma ramp, linear by default): a white quad
     * times c0 = (0.5, 0.2, 0.02) is (188, 124, 39) in the frame -- as render target 0 at the swap, as the resolve's
     * host picture of the front buffer -- and in the resolved memory, not the linear (128, 51, 5) */
    {
        static const int want[3] = {188, 124, 39};
        for (uint32_t k = 0; k < 4; k++)
            vertex(0x5E000 + 32 * k, k & 1 ? 1.0f : -1.0f, k & 2 ? -1.0f : 1.0f, 1, 1, 1);
        reg(0x2000, 64u);               /* RB_SURFACE_INFO: pitch 64, no MSAA */
        reg(0x2001, 0x300u | 1u << 16); /* RB_COLOR_INFO: base 0x300, 8_8_8_8_GAMMA */
        reg(0x4400, fbits(0.5f)), reg(0x4401, fbits(0.2f)), reg(0x4402, fbits(0.02f)), reg(0x4403, fbits(1.0f));
        for (int pass = 0; pass < 2; pass++) {
            reg(0x4800, 0x5E000u | 3u);
            reg(0x4801, 2u | (32u << 2));
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            if (pass == 1) { /* resolved to a tiled 8_8_8_8 front buffer at 0xA0000, which the swap shows */
                reg(0x4800, 0x7000u | 3u);
                reg(0x4801, 2u | (6u << 2));
                reg(0x2208, 6);
                reg(0x2318, 0u);
                reg(0x2319, 0xA0000);
                reg(0x231A, W | (uint32_t)H << 16);
                reg(0x231B, 2u | 6u << 7);
                type3(PM4_DRAW_INDX_2, &rect, 1);
                reg(0x2208, 4);
                const uint32_t ff[6] = {2u | 1u << 31, 6u | 2u << 6 | 0xA0000u, (W - 1u) | (H - 1u) << 13,
                                        sw_id << 1, 0, 1u << 9};
                for (uint32_t i = 0; i < 6; i++)
                    reg(0x4800 + i, ff[i]);
            }
            xenos_gl_stats f0, f1;
            xenos_gl_get_stats(g, &f0);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            xenos_gl_get_stats(g, &f1);
            const uint8_t *cg = px(g, W / 2, H / 2);
            int ok = 1;
            for (int i = 0; i < 3; i++)
                ok &= cg[i] >= want[i] - 2 && cg[i] <= want[i] + 2;
            CHECK(ok && f1.front_buffer_frames == f0.front_buffer_frames + (uint64_t)pass,
                  "8_8_8_8_GAMMA target, %s: %u,%u,%u, want %d,%d,%d", pass ? "front buffer" : "render target 0",
                  cg[0], cg[1], cg[2], want[0], want[1], want[2]);
            if (pass == 1) {
                uint32_t tg = rd32(NULL, 0xA0000 + tiled_off(5, 5));
                ok = 1;
                for (int i = 0; i < 3; i++)
                    ok &= (int)((tg >> (8 * i)) & 0xFF) >= want[i] - 2 && (int)((tg >> (8 * i)) & 0xFF) <= want[i] + 2;
                CHECK(ok, "8_8_8_8_GAMMA target resolved: %08X, want about FF%02X%02X%02X", tg, want[2], want[1],
                      want[0]);
            }
        }
        reg(0x2001, 0), reg(0x2000, 0);
        for (uint32_t i = 0; i < 4; i++)
            reg(0x4400 + i, fbits(1.0f));
        run(&cp);
    }

    /* depth bias (PA_SU_SC_MODE_CNTL's poly_offset_front_enable, PA_SU_POLY_OFFSET_FRONT_OFFSET): a green quad at z 0.5
     * fills the depth buffer, then coplanar quads at z 0.5: red with the test "less" fails without a bias and passes
     * pulled 8 depth steps nearer; blue with "less or equal" passes without one and fails pushed 8 steps away */
    {
        static const float zrgb[4][3] = {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0}};
        for (uint32_t q = 0; q < 3; q++)
            for (uint32_t k = 0; k < 4; k++)
                vertex_z(0xA0000 + 0x80 * q + 32 * k, k & 1 ? 1.0f : -1.0f, k & 2 ? -1.0f : 1.0f, 0.5f, zrgb[q][0],
                         zrgb[q][1], zrgb[q][2]);
        reg(0x4801, 2u | (32u << 2));
        static const struct {
            uint32_t quad, zfunc, sc;
            float offset;
            uint8_t want[3];
            const char *what;
        } bias[4] = {
            {1, 1, 0, 0.0f, {0, 255, 0}, "red, less, no bias: fails"},
            {1, 1, 1u << 11, -8.0f / 16777215.0f, {255, 0, 0}, "red, less, pulled nearer: passes"},
            {2, 3, 0, 0.0f, {0, 0, 255}, "blue, less or equal, no bias: passes"},
            {2, 3, 1u << 11, 8.0f / 16777215.0f, {0, 255, 0}, "blue, less or equal, pushed away: fails"},
        };
        for (int t = 0; t < 4; t++) {
            reg(0x2205, 0), reg(0x2380, 0), reg(0x2381, 0);
            reg(0x2200, 2u | 4u | 7u << 4); /* z test always, z write: the green quad at 0.5 everywhere */
            reg(0x4800, 0xA0000u | 3u);
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            reg(0x2200, 2u | bias[t].zfunc << 4); /* z test, no write */
            reg(0x2205, bias[t].sc), reg(0x2381, fbits(bias[t].offset));
            reg(0x4800, (0xA0000u + 0x80 * bias[t].quad) | 3u);
            type3(PM4_DRAW_INDX_2, &strip_a, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            const uint8_t *cb = px(g, W / 2, H / 2);
            CHECK(cb[0] == bias[t].want[0] && cb[1] == bias[t].want[1] && cb[2] == bias[t].want[2],
                  "depth bias (%s): %u,%u,%u", bias[t].what, cb[0], cb[1], cb[2]);
        }
        reg(0x2200, 0), reg(0x2205, 0), reg(0x2381, 0);
        run(&cp);
    }

    /* a draw whose packet holds fewer indices than it says is counted, with its reason */
    {
        xenos_gl_stats s0, s1;
        xenos_gl_get_stats(g, &s0);
        uint32_t cutshort[2] = {4u | 1u << 6 | 6u << 16, 0};
        type3(PM4_DRAW_INDX_2, cutshort, 2);
        run(&cp);
        xenos_gl_get_stats(g, &s1);
        uint64_t count = 0;
        const char *why = xenos_gl_unsupported_reason(g, 0, &count);
        CHECK(s1.unsupported == s0.unsupported + 1 && s1.drawn == s0.drawn && why && count == 1 &&
                  strstr(why, "immediate"),
              "short immediate draw: unsupported +%llu, reason %s", (unsigned long long)(s1.unsupported - s0.unsupported),
              why ? why : "(none)");
    }

    /* a depth surface the eDRAM leaves shorter than the colour target (a 1080p title: pitch 1920, colour at tile 0,
     * RB_DEPTH_INFO at tile 0x3FC, (2048 - 1020) / 24 tiles a row = 672 rows of depth for 1080 of colour): a
     * screen-space green quad over all 1920x1080 with depth and stencil unused reaches row 1079; then a blue quad at
     * z 0.25 (z test always, z write) and a red one at z 0.75 (test less) still use the depth buffer: blue stays where
     * it reaches. A resolve of a 32-wide column at rows 640..1080 to memory reads the target back. */
    {
        const uint32_t vq = 0xF0000, rq = 0xF0400, dst = 0xC0000;
        static const float qz[3] = {0.5f, 0.25f, 0.75f}, qc[3][3] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}};
        for (uint32_t q = 0; q < 3; q++)
            for (uint32_t k = 0; k < 4; k++)
                vertex_z(vq + 0x80 * q + 32 * k, k & 1 ? 1920.0f : 0.0f, k & 2 ? 1080.0f : 0.0f, qz[q], qc[q][0],
                         qc[q][1], qc[q][2]);
        for (uint32_t i = 0; i < 4; i++)
            reg(0x4400 + i, fbits(1.0f));
        reg(0x2206, 0);                                 /* VTE off: positions are pixels */
        reg(0x2205, 0), reg(0x2208, 4);
        reg(0x2000, 1920u), reg(0x2001, 0), reg(0x2002, 0x3FCu);
        reg(0x2081, 0), reg(0x2082, 1920u | 1080u << 16);
        reg(0x4801, 2u | (32u << 2));
        reg(0x2200, 0); /* no stencil, no z test, no z write */
        reg(0x4800, vq | 3u);
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        reg(0x2200, 2u | 4u | 7u << 4); /* z test always, z write */
        reg(0x4800, (vq + 0x80) | 3u);
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        reg(0x2200, 2u | 1u << 4); /* z test less, no write: behind the blue quad */
        reg(0x4800, (vq + 0x100) | 3u);
        type3(PM4_DRAW_INDX_2, &strip_a, 1);
        reg(0x2200, 0);
        static const float rv2[6] = {0, 640, 32, 640, 32, 1080};
        for (int i = 0; i < 6; i++)
            w32(rq + 4 * (uint32_t)i, fbits(rv2[i]));
        reg(0x4800, rq | 3u);
        reg(0x4801, 2u | (6u << 2));
        reg(0x2208, 6);                 /* copy */
        reg(0x2318, 0u);                /* colour 0, raw copy, no clear */
        reg(0x2319, dst);
        reg(0x231A, 32u | 1080u << 16); /* pitch 32, height 1080 */
        reg(0x231B, 2u | 6u << 7);      /* 8-in-32, 8_8_8_8 */
        type3(PM4_DRAW_INDX_2, &rect, 1);
        reg(0x2208, 4);
        run(&cp);
        static const struct {
            int y;
            uint32_t want;
            const char *what;
        } rows[5] = {
            {650, 0xFFFF0000u, "row 650 (depth reaches it): blue, red fails the test"},
            {671, 0xFFFF0000u, "row 671, the depth surface's last: blue"},
            {672, 0xFF00FF00u, "row 672: green (black when the depth surface clips the draw)"},
            {900, 0xFF00FF00u, "row 900: green"},
            {1079, 0xFF00FF00u, "row 1079: green"},
        };
        for (int r = 0; r < 5; r++) {
            uint32_t t = rd32(NULL, dst + tiled_off_pitch(16, rows[r].y, 32, 2));
            CHECK(t == rows[r].want, "1080p target, unused short depth surface, %s: %08X, want %08X", rows[r].what, t,
                  rows[r].want);
        }
        reg(0x2206, 0x3Fu), reg(0x2082, 0), reg(0x2001, 0), reg(0x2002, 0), reg(0x2000, 0);
        run(&cp);
    }
    window_offset_tiles(g, &cp, ps, nps, 0);
    viewport_z_reversed(g, &cp, ps, nps);
    xenos_gl_destroy(g);

    /* WP-28's clear through an MSAA view, on a renderer of its own whose rectangle lists are read back
     * (RECOMP_X360_RECTS=cpu): the GPU path loses a rectangle now and then under SwiftShader at scale 2 on a loaded
     * machine (a plain 1x rectangle too: WP-29's capture read as a uniform block), which is not what this checks */
    {
        const char *env = getenv("RECOMP_X360_RECTS");
        char saved[16] = "";
        if (env)
            snprintf(saved, sizeof saved, "%s", env);
        setenv("RECOMP_X360_RECTS", "cpu", 1);
        g = xenos_gl_create_scaled(cache, W, H, scale, why, sizeof why);
        if (env)
            setenv("RECOMP_X360_RECTS", saved, 1);
        else
            unsetenv("RECOMP_X360_RECTS");
        CHECK(g != NULL, "MSAA view: no renderer: %s", why);
        if (g) {
            cp.render_ctx = g;
            msaa_view_clear(g, &cp, ps, nps);
            xenos_gl_destroy(g);
        }
    }

    /* WP-2, lazy resolves (RECOMP_X360_RESOLVE=lazy), on a renderer of their own: a resolve leaves memory as it was
     * and only draws into its host picture; the pixels reach memory when a texture reads that memory another way, 2
     * swaps after the last resolve, at xenos_gl_flush_resolves; a CPU write before that wins (the copy is dropped); a
     * state load (xenos_gl_reset) forgets what was pending. Destination 0xF0000, a tiled 64x64 8_8_8_8 (8-in-32). */
    if (pst2) {
        setenv("RECOMP_X360_RESOLVE", "lazy", 1);
        g = xenos_gl_create_scaled(cache, W, H, scale, why, sizeof why);
        unsetenv("RECOMP_X360_RESOLVE");
        CHECK(g && xenos_gl_lazy_resolves(g), "lazy resolves: renderer %p, lazy %d", (void *)g,
              g ? xenos_gl_lazy_resolves(g) : 0);
    } else {
        g = NULL;
    }
    if (g) {
        enum { DST = 0xF0000 };
        cp.render_ctx = g;
        for (uint32_t i = 0; i < 64 * 64 * 4; i += 4)
            w32(DST + i, 0xABABABABu);
        reg(0x2104, 0xFFFFu), reg(0x2200, 0), reg(0x2201, 0x00010001u), reg(0x2205, 0);
        full_viewport();
        for (int i = 0; i < 4; i++)
            reg(0x4400 + i, fbits(1.0f));
        /* a full-target quad of one colour at 0xEF000, drawn with the colour shader */
        #define LAZY_QUAD(r_, g_, b_)                                                                                \
            do {                                                                                                     \
                vertex(0xEF000, -1, -1, r_, g_, b_), vertex(0xEF020, 1, -1, r_, g_, b_);                             \
                vertex(0xEF040, -1, 1, r_, g_, b_), vertex(0xEF060, 1, 1, r_, g_, b_);                               \
                im[0] = 1, im[1] = nps;                                                                              \
                memcpy(im + 2, ps, 4 * nps);                                                                         \
                type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);                                                           \
                reg(0x4800, 0xEF000u | 3u), reg(0x4801, 2u | (32u << 2));                                            \
                type3(PM4_DRAW_INDX_2, &strip_a, 1);                                                                 \
            } while (0)
        /* the whole target resolved to DST (no clear) */
        #define LAZY_RESOLVE()                                                                                       \
            do {                                                                                                     \
                static const float lv[6] = {0, 0, W, 0, W, H};                                                       \
                for (int i_ = 0; i_ < 6; i_++)                                                                       \
                    w32(0xEF100 + 4 * (uint32_t)i_, fbits(lv[i_]));                                                  \
                reg(0x4800, 0xEF100u | 3u), reg(0x4801, 2u | (6u << 2));                                             \
                reg(0x2208, 6), reg(0x2318, 0), reg(0x2319, DST), reg(0x231A, W | (uint32_t)H << 16);                \
                reg(0x231B, 2u | 6u << 7);                                                                           \
                uint32_t rect_ = 8u | 2u << 6 | 3u << 16;                                                            \
                type3(PM4_DRAW_INDX_2, &rect_, 1);                                                                   \
                reg(0x2208, 4);                                                                                      \
            } while (0)
        /* DST sampled over the target: endian 2 reads it as the resolve wrote it (its host picture), 0 does not */
        #define LAZY_SAMPLE(endian_)                                                                                 \
            do {                                                                                                     \
                im[0] = 1, im[1] = ntex;                                                                             \
                memcpy(im + 2, pst2, 4 * ntex);                                                                      \
                type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + ntex);                                                          \
                reg(0x4800, 0x6000u | 3u), reg(0x4801, 2u | (32u << 2));                                             \
                uint32_t f_[6] = {2u | 2u << 10 | 2u << 13 | 2u << 22 | 1u << 31, 6u | (endian_) << 6 | DST,         \
                                  63u | 63u << 13, sw_id << 1, 0, 1u << 9};                                          \
                for (int i_ = 0; i_ < 6; i_++)                                                                       \
                    reg(0x480C + i_, f_[i_]);                                                                        \
                type3(PM4_DRAW_INDX_2, &strip_a, 1);                                                                 \
            } while (0)
        #define LAZY_AT(x_, y_) rd32(NULL, DST + tiled_off(x_, y_))
        xenos_gl_stats ls;
        /* a red resolve: memory untouched */
        LAZY_QUAD(1, 0, 0);
        LAZY_RESOLVE();
        run(&cp);
        xenos_gl_get_stats(g, &ls);
        CHECK(ls.resolves == 1 && ls.resolves_deferred == 1 && ls.resolve_flushes == 0,
              "lazy: resolves %llu, deferred %llu, written %llu, want 1, 1, 0", (unsigned long long)ls.resolves,
              (unsigned long long)ls.resolves_deferred, (unsigned long long)ls.resolve_flushes);
        CHECK(LAZY_AT(1, 1) == 0xABABABABu, "lazy: memory after the resolve %08X, want ABABABAB (as it was)",
              LAZY_AT(1, 1));
        /* sampled as written: the host picture, still nothing written; the swap shows red */
        LAZY_SAMPLE(2u);
        run(&cp);
        xenos_gl_get_stats(g, &ls);
        CHECK(ls.resolve_flushes == 0 && ls.copy_textures == 1 && LAZY_AT(37, 21) == 0xABABABABu,
              "lazy, sampled from the picture: written %llu, picture textures %llu, memory %08X",
              (unsigned long long)ls.resolve_flushes, (unsigned long long)ls.copy_textures, LAZY_AT(37, 21));
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        c = px(g, W / 2, H / 2);
        CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0, "lazy, sampled from the picture: %u,%u,%u, want red", c[0], c[1],
              c[2]);
        CHECK(LAZY_AT(1, 1) == 0xABABABABu, "lazy, one swap after the resolve: %08X, want ABABABAB", LAZY_AT(1, 1));
        /* a second swap without a resolve: written */
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        xenos_gl_get_stats(g, &ls);
        CHECK(ls.resolve_flushes == 1 && LAZY_AT(1, 1) == 0xFF0000FFu && LAZY_AT(37, 21) == 0xFF0000FFu,
              "lazy, two swaps after the resolve: written %llu, memory %08X %08X, want 1, FF0000FF",
              (unsigned long long)ls.resolve_flushes, LAZY_AT(1, 1), LAZY_AT(37, 21));
        /* a green resolve, then DST read another way (endian 0): written first */
        LAZY_QUAD(0, 1, 0);
        LAZY_RESOLVE();
        run(&cp);
        CHECK(LAZY_AT(5, 5) == 0xFF0000FFu, "lazy, green resolved: %08X, want the red still", LAZY_AT(5, 5));
        LAZY_SAMPLE(0u);
        run(&cp);
        xenos_gl_get_stats(g, &ls);
        CHECK(ls.resolve_flushes == 2 && LAZY_AT(5, 5) == 0xFF00FF00u,
              "lazy, read another way: written %llu, memory %08X, want 2, FF00FF00",
              (unsigned long long)ls.resolve_flushes, LAZY_AT(5, 5));
        /* a blue resolve, then a CPU write there before anything writes it: the CPU's bytes win, the rest of the
         * memory stays green, the copy is dropped */
        LAZY_QUAD(0, 0, 1);
        LAZY_RESOLVE();
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        w32(DST + tiled_off(1, 1), 0x12345678u);
        LAZY_SAMPLE(2u);
        type3(PM4_XE_SWAP, swap, 4);
        run(&cp);
        xenos_gl_get_stats(g, &ls);
        CHECK(ls.resolves_dropped == 1 && LAZY_AT(1, 1) == 0x12345678u && LAZY_AT(37, 21) == 0xFF00FF00u,
              "lazy, CPU write first: dropped %llu, memory %08X %08X, want 1, 12345678, FF00FF00",
              (unsigned long long)ls.resolves_dropped, LAZY_AT(1, 1), LAZY_AT(37, 21));
        /* red, then xenos_gl_flush_resolves (a save state): written */
        LAZY_QUAD(1, 0, 0);
        LAZY_RESOLVE();
        run(&cp);
        xenos_gl_flush_resolves(g, &cp);
        CHECK(LAZY_AT(1, 1) == 0xFF0000FFu && LAZY_AT(37, 21) == 0xFF0000FFu,
              "lazy, xenos_gl_flush_resolves: %08X %08X, want FF0000FF", LAZY_AT(1, 1), LAZY_AT(37, 21));
        /* green, then a state load (xenos_gl_reset): forgotten, memory is the state's */
        LAZY_QUAD(0, 1, 0);
        LAZY_RESOLVE();
        run(&cp);
        xenos_gl_reset(g);
        xenos_gl_flush_resolves(g, &cp);
        xenos_gl_get_stats(g, &ls);
        CHECK(LAZY_AT(1, 1) == 0xFF0000FFu && ls.resolves_deferred == 5,
              "lazy, pending at a state load: memory %08X (want FF0000FF), deferred %llu (want 5)", LAZY_AT(1, 1),
              (unsigned long long)ls.resolves_deferred);
        #undef LAZY_QUAD
        #undef LAZY_RESOLVE
        #undef LAZY_SAMPLE
        #undef LAZY_AT
        window_offset_tiles(g, &cp, ps, nps, 1);
        xenos_gl_destroy(g);
    }
    /* WP-10, rectangle lists on the GPU (the capture read as a uniform block, 16 rectangles a pass) and with
     * RECOMP_X360_RECTS=cpu (read back), each on a renderer of its own: 20 rectangles in one list (two passes) in a 4x5
     * grid, one colour each, every other one given with its corners in another order (the vertex opposite the longest
     * edge second, then third); each rectangle's centre is its colour, and the two pictures are the same bytes. The
     * read-back path first: where it draws wrong (a driver's transform feedback: ANGLE on Metal captures one vertex,
     * frame 14 fails the same way) the GPU path, which reads the same capture, is not checked. */
    {
        enum { RV = 0xEE000, NR = 20 };
        uint8_t *rect_frame[2] = {NULL, NULL};
        int cpu_ok = 1, g_selftest = -1;
        for (int mode = 1; mode >= 0; mode--) { /* 1: cpu, 0: gpu */
            const char *env = getenv("RECOMP_X360_RECTS");
            char saved[16] = "";
            if (env)
                snprintf(saved, sizeof saved, "%s", env);
            setenv("RECOMP_X360_RECTS", mode ? "cpu" : "gpu", 1);
            g = xenos_gl_create_scaled(cache, W, H, scale, why, sizeof why);
            if (env)
                setenv("RECOMP_X360_RECTS", saved, 1);
            else
                unsetenv("RECOMP_X360_RECTS");
            CHECK(g != NULL, "rectangle lists (%s): no renderer: %s", mode ? "cpu" : "gpu", why);
            if (!g)
                continue;
            cp.render_ctx = g;
            reg(0x2104, 0xFFFFu), reg(0x2200, 0), reg(0x2201, 0x00010001u), reg(0x2205, 0), reg(0x2202, 0);
            full_viewport();
            for (int i = 0; i < 4; i++)
                reg(0x4400 + i, fbits(1.0f));
            for (int r = 0; r < NR; r++) {
                float x0 = -1.0f + 0.5f * (float)(r % 4), x1 = x0 + 0.5f, y0 = 1.0f - 0.4f * (float)(r / 4),
                      y1 = y0 - 0.4f;
                float cr = (float)(r % 4) / 3.0f, cg = (float)(r / 4) / 4.0f, cb = (r & 1) ? 1.0f : 0.25f;
                /* top-left (the corner: opposite the diagonal), top-right, bottom-left, rotated by r % 3 */
                const float cx[3] = {x0, x1, x0}, cy[3] = {y0, y0, y1};
                for (int k = 0; k < 3; k++) {
                    int j = (k + r % 3) % 3;
                    vertex(RV + 32u * (uint32_t)(3 * r + k), cx[j], cy[j], cr, cg, cb);
                }
            }
            im[0] = 1, im[1] = nps;
            memcpy(im + 2, ps, 4 * nps);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
            reg(0x4800, (uint32_t)RV | 3u);
            reg(0x4801, 2u | ((uint32_t)NR * 3 * 8) << 2);
            uint32_t rects = 8u | 2u << 6 | (uint32_t)(NR * 3) << 16;
            type3(PM4_DRAW_INDX_2, &rects, 1);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            xenos_gl_stats rs;
            xenos_gl_get_stats(g, &rs);
            CHECK(rs.rect_selftest >= 0 && rs.rect_selftest <= 3, "rectangle lists: self-test %d of 3",
                  rs.rect_selftest);
            CHECK(rs.rect_draws == 1 && rs.rect_lists_read_back == (uint64_t)mode,
                  "rectangle lists (%s): %llu drawn, %llu read back", mode ? "cpu" : "gpu",
                  (unsigned long long)rs.rect_draws, (unsigned long long)rs.rect_lists_read_back);
            int bad = 0;
            for (int r = 0; r < NR && !bad && cpu_ok; r++) {
                int x = (r % 4) * W / 4 + W / 8, y = (int)((float)(r / 4) * H / 5.0f + H / 10.0f);
                const uint8_t *q = px(g, x, y);
                int want[3] = {(int)((float)(r % 4) / 3.0f * 255.0f + 0.5f),
                               (int)((float)(r / 4) / 4.0f * 255.0f + 0.5f), (r & 1) ? 255 : 64};
                for (int k = 0; k < 3; k++)
                    if (q[k] - want[k] > 2 || want[k] - q[k] > 2)
                        bad = 1;
                if (bad && mode) {
                    printf("rectangle lists: the read-back path draws rectangle %d as %u,%u,%u here (transform "
                           "feedback, as frame 14): the GPU path is not checked\n", r, q[0], q[1], q[2]);
                    cpu_ok = 0;
                    break;
                }
                CHECK(!bad, "rectangle lists (%s): rectangle %d at %d,%d is %u,%u,%u, want %d,%d,%d",
                      mode ? "cpu" : "gpu", r, x, y, q[0], q[1], q[2], want[0], want[1], want[2]);
            }
            if (mode)
                g_selftest = rs.rect_selftest;
            int fw, fh;
            const uint8_t *f = xenos_gl_frame(g, &fw, &fh);
            if (f && (rect_frame[mode] = malloc((size_t)fw * fh * 4)))
                memcpy(rect_frame[mode], f, (size_t)fw * fh * 4);
            xenos_gl_destroy(g);
        }
        /* WP-29: the start-up self-test says what the read-back path showed */
        if (g_selftest >= 0)
            CHECK(cpu_ok == (g_selftest == 3), "rectangle lists: drawn %s, but the self-test captured %d of 3",
                  cpu_ok ? "right" : "wrong", g_selftest);
        if (cpu_ok && rect_frame[0] && rect_frame[1])
            CHECK(!memcmp(rect_frame[0], rect_frame[1], (size_t)W * H * 4),
                  "rectangle lists: the GPU's picture differs from the read-back one");
        /* WP-35 step 2: the same 20 rectangles as an indexed list (16-bit DMA indices, 8-in-16), the vertices stored
         * out of order (logical vertex v at slot 7v mod 60), each mode on a fresh renderer: the same picture. Below
         * GLES 3.2 (the test's 3.0 context) the capture draws one point per index, as glDrawElements during transform
         * feedback is an error there. */
        enum { RVI = 0xEF000, RII = 0xEE800 };
        for (int r = 0; r < NR; r++) {
            float x0 = -1.0f + 0.5f * (float)(r % 4), x1 = x0 + 0.5f, y0 = 1.0f - 0.4f * (float)(r / 4),
                  y1 = y0 - 0.4f;
            float cr = (float)(r % 4) / 3.0f, cg = (float)(r / 4) / 4.0f, cb = (r & 1) ? 1.0f : 0.25f;
            const float cx[3] = {x0, x1, x0}, cy[3] = {y0, y0, y1};
            for (int k = 0; k < 3; k++) {
                int j = (k + r % 3) % 3;
                uint32_t v = (uint32_t)(3 * r + k), slot = (7u * v) % (NR * 3);
                vertex(RVI + 32u * slot, cx[j], cy[j], cr, cg, cb);
                mem[RII + 2 * v] = (uint8_t)(slot >> 8), mem[RII + 2 * v + 1] = (uint8_t)slot;
            }
        }
        for (int mode = 1; mode >= 0 && cpu_ok; mode--) {
            const char *env = getenv("RECOMP_X360_RECTS");
            char saved[16] = "";
            if (env)
                snprintf(saved, sizeof saved, "%s", env);
            setenv("RECOMP_X360_RECTS", mode ? "cpu" : "gpu", 1);
            g = xenos_gl_create_scaled(cache, W, H, scale, why, sizeof why);
            if (env)
                setenv("RECOMP_X360_RECTS", saved, 1);
            else
                unsetenv("RECOMP_X360_RECTS");
            CHECK(g != NULL, "indexed rectangle list (%s): no renderer: %s", mode ? "cpu" : "gpu", why);
            if (!g)
                continue;
            cp.render_ctx = g;
            reg(0x2104, 0xFFFFu), reg(0x2200, 0), reg(0x2201, 0x00010001u), reg(0x2205, 0), reg(0x2202, 0);
            full_viewport();
            for (int i = 0; i < 4; i++)
                reg(0x4400 + i, fbits(1.0f));
            im[0] = 1, im[1] = nps;
            memcpy(im + 2, ps, 4 * nps);
            type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
            reg(0x4800, (uint32_t)RVI | 3u);
            reg(0x4801, 2u | ((uint32_t)NR * 3 * 8) << 2);
            uint32_t idr[3] = {8u | 0u << 6 | (uint32_t)(NR * 3) << 16, RII, (uint32_t)(NR * 3 / 2) | 1u << 30};
            type3(PM4_DRAW_INDX_2, idr, 3);
            type3(PM4_XE_SWAP, swap, 4);
            run(&cp);
            xenos_gl_stats rs;
            xenos_gl_get_stats(g, &rs);
            CHECK(rs.rect_draws == 1, "indexed rectangle list (%s): %llu drawn", mode ? "cpu" : "gpu",
                  (unsigned long long)rs.rect_draws);
            int fw, fh;
            const uint8_t *f = xenos_gl_frame(g, &fw, &fh);
            int same = f && rect_frame[mode] && fw == W && fh == H && !memcmp(f, rect_frame[mode], (size_t)W * H * 4);
            const uint8_t *q = px(g, W / 8, H / 10);
            CHECK(same, "indexed rectangle list (%s): picture differs from the non-indexed one (rectangle 0 is "
                        "%u,%u,%u)", mode ? "cpu" : "gpu", q[0], q[1], q[2]);
            xenos_gl_destroy(g);
        }
        free(rect_frame[0]);
        free(rect_frame[1]);
    }
    free(pst);
    free(vs);
    free(ps);
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("xenos_gl: every frame drawn and read back as expected (the first 33 with six resolves, then XBA-296's)\n");
    return 0;
}
