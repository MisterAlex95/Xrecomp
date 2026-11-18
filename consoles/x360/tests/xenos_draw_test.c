/* xenos_draw_test DIR (XBA-270): the GLES renderer (xenos_gl.c) on a hand-built PM4 stream -- the first pixels.
 *
 * DIR holds make_draw.py's shaders (vs.bin, ps.bin, cache/). Guest memory is a big-endian array as the CPU sees it;
 * the packets load both shaders (IM_LOAD_IMMEDIATE), set fetch constant 0 and the pixel shader's c0 (type-0 register
 * writes), and draw:
 *   frame 0: a red triangle (auto indices, triangle list), c0 = 1: the centre is red, a corner stays clear;
 *   frame 1: a full-target quad from four green vertices and six 16-bit indices (DMA, 8-in-16 swap mode, the way a
 *            big-endian CPU writes them), c0 = (1, 0.5, 1, 1): every pixel is half green;
 *   frame 2: the same quad with the viewport scale / offset enabled for the target's top-left quarter only.
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

static void vertex(uint32_t at, float x, float y, float r, float g, float b)
{
    const float v[8] = {x, y, 0.0f, 1.0f, r, g, b, 1.0f};
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

    xenos_gl_stats st;
    xenos_gl_get_stats(g, &st);
    CHECK(st.drawn == 3 && st.frames == 3 && st.no_shader == 0 && st.unsupported == 0,
          "stats: drawn %llu frames %llu no shader %llu unsupported %llu", (unsigned long long)st.drawn,
          (unsigned long long)st.frames, (unsigned long long)st.no_shader, (unsigned long long)st.unsupported);
    xenos_gl_destroy(g);
    free(vs);
    free(ps);
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("xenos_gl: three frames drawn and read back as expected\n");
    return 0;
}
