/* xenos_draw_test DIR (XBA-270): the GLES renderer (xenos_gl.c) on a hand-built PM4 stream -- the first pixels.
 *
 * DIR holds make_draw.py's shaders (vs.bin, ps.bin, cache/). Guest memory is a big-endian array as the CPU sees it;
 * the packets load both shaders (IM_LOAD_IMMEDIATE), set fetch constant 0 and the pixel shader's c0 (type-0 register
 * writes), and draw:
 *   frame 0: a red triangle (auto indices, triangle list), c0 = 1: the centre is red, a corner stays clear;
 *   frame 1: a full-target quad from four green vertices and six 16-bit indices (DMA, 8-in-16 swap mode, the way a
 *            big-endian CPU writes them), c0 = (1, 0.5, 1, 1): every pixel is half green;
 *   frame 2: the same quad with the viewport scale / offset enabled for the target's top-left quarter only;
 *   frame 3: depth: a green quad at z 0.25, then a red one at z 0.75 with the test "less": green stays;
 *   frame 4: blending: red, then green at alpha 0.5 with src alpha / one minus src alpha: (0.5, 0.5, 0);
 *   frame 5: culling of back faces: the red triangle (counter-clockwise on screen) is drawn with "front is CCW" and
 *            culled with "front is CW";
 *   frame 6: a quad list (one quad of four vertices) covers the target;
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
 *            depth frame's buffer, and a strip cut by the reset index (two triangles of a quad, then a restart).
 * Each frame is read back at the swap. Exit 77 when EGL / GLES 3.0 cannot be had. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xenos_cp.h"
#include "xenos_gl.h"

#define MEM_SIZE 0x40000u
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

static void run(xenos_cp *cp)
{
    for (uint32_t i = 0; i < npk; i++)
        w32(0x8000 + 4 * i, pk[i]);
    xenos_cp_execute(cp, 0x8000, npk);
    npk = 0;
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

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: xenos_draw_test DIR\n");
        return 2;
    }
    char cache[1024], why[256];
    snprintf(cache, sizeof cache, "%s/cache", argv[1]);
    xenos_gl *g = xenos_gl_create(cache, W, H, why, sizeof why);
    if (!g) {
        printf("skipped: %s\n", why);
        return 77;
    }
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

    /* the render state a title sets first: all colour channels written, no blending (src * 1 + dst * 0) */
    reg(0x2104, 0xFu);
    reg(0x2201, 0x00010001u);

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
    for (int y = 0; y < H; y += 7)
        for (int x = 0; x < W; x += 7) {
            const uint8_t *p = px(g, x, y);
            if (p[0] != 0 || p[1] < 126 || p[1] > 129 || p[2] != 0)
                all = 0;
        }
    CHECK(all, "frame 1: not every pixel half green (corner %u %u %u)", px(g, 0, 0)[0], px(g, 0, 0)[1],
          px(g, 0, 0)[2]);

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
    reg(0x2206, 0);
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
    uint32_t fc[6] = {2u | 2u << 10 | 2u << 13 | 1u << 22, 6u | 2u << 6 | 0x10000u, 1u | 1u << 13, sw_id << 1, 0,
                      1u << 9};
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
    /* DXT1: one block, colour 0 = 5:6:5 0x001F (X = 31), colour 1 = 0, every index 0 */
    static const uint16_t blk[4] = {0x001F, 0x0000, 0x0000, 0x0000};
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
    reg(0x231E, 0xFF00FF00u);                           /* clear: green (D3DCOLOR) */
    uint32_t rect = 8u | 2u << 6 | 3u << 16;
    type3(PM4_DRAW_INDX_2, &rect, 1);
    reg(0x2208, 4);                                     /* back to colour + depth */
    type3(PM4_XE_SWAP, swap, 4);
    run(&cp);
    c = px(g, W / 2, H / 2);
    CHECK(c[0] == 0 && c[1] == 255, "frame 12 (resolve clear): %u,%u,%u, want green", c[0], c[1], c[2]);
    /* the copy: texel (x, y) of the tiled destination holds red (X = R in the low byte, 8-in-32: big-endian) */
    uint32_t t10 = rd32(NULL, 0x18000 + 0), tmid = 0;
    {
        int x = 37, y = 21;
        int32_t outer = ((y >> 5) * (W >> 5) + (x >> 5)) << 6, inner = (((y >> 1) & 7) << 3) | (x & 7);
        int32_t oi = (outer | inner) << 2;
        uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
        int32_t off = (int32_t)((uint32_t)(y & 1) << 4 | pipe << 6 | bank << 11) | (oi & 15) | ((oi >> 4) & 1) << 5 |
                      ((oi >> 5) & 7) << 8 | (oi >> 8) << 12;
        tmid = rd32(NULL, 0x18000 + (uint32_t)off);
    }
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
    const uint8_t *rtl = px(g, 0, 0), *rtr = px(g, W - 1, 0), *rbl = px(g, 0, H - 1), *rbr = px(g, W - 1, H - 1);
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

    xenos_gl_stats st;
    xenos_gl_get_stats(g, &st);
    CHECK(st.drawn == 23 && st.frames == 20 && st.resolves == 1 && st.no_shader == 0 && st.unsupported == 0 &&
              st.unsupported_textures == 0,
          "stats: drawn %llu frames %llu no shader %llu unsupported %llu", (unsigned long long)st.drawn,
          (unsigned long long)st.frames, (unsigned long long)st.no_shader, (unsigned long long)st.unsupported);
    xenos_gl_destroy(g);
    free(pst);
    free(vs);
    free(ps);
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("xenos_gl: twenty frames drawn and read back as expected, one resolve\n");
    return 0;
}
