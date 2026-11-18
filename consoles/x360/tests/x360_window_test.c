/* x360_window_test DIR OUT.ppm (XBA-280): the renderer on a window's context, as x360_play drives it -- the host
 * layer's window (host_window, GLES 3.0), xenos_gl_attach at an internal resolution of 2, a hand-built PM4 stream
 * (make_draw.py's shaders in DIR: a red triangle on a clear target, the swap), xenos_gl_end_frame, the frame texture
 * presented (host_window_present_texture, top row first) and the drawable captured (host_window_capture): the
 * centre is red, a corner clear, the apex in the upper half (the picture the right way up). Exit 77 without a
 * display or a GLES 3 window. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_window.h"
#include "xenos_cp.h"
#include "xenos_gl.h"

#define MEM_SIZE 0x20000u
static uint8_t mem[MEM_SIZE];

static uint32_t rd32(void *ctx, uint32_t phys)
{
    (void)ctx;
    return phys + 4 > MEM_SIZE ? 0
                               : (uint32_t)mem[phys] << 24 | (uint32_t)mem[phys + 1] << 16 |
                                     (uint32_t)mem[phys + 2] << 8 | mem[phys + 3];
}

static void w32(uint32_t a, uint32_t v)
{
    mem[a] = (uint8_t)(v >> 24), mem[a + 1] = (uint8_t)(v >> 16), mem[a + 2] = (uint8_t)(v >> 8);
    mem[a + 3] = (uint8_t)v;
}

static void wr32(void *ctx, uint32_t phys, uint32_t v)
{
    (void)ctx;
    if (phys + 4 <= MEM_SIZE)
        w32(phys, v);
}

static uint32_t fbits(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

static uint32_t pk[2048], npk;

static void type3(uint32_t op, const uint32_t *w, uint32_t n)
{
    pk[npk++] = 3u << 30 | (n - 1) << 16 | op << 8;
    for (uint32_t i = 0; i < n; i++)
        pk[npk++] = w[i];
}

static void reg(uint32_t index, uint32_t v)
{
    pk[npk++] = index;
    pk[npk++] = v;
}

static uint32_t *load(const char *dir, const char *name, uint32_t *n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    static uint8_t b[4096];
    size_t len = fread(b, 1, sizeof b, f);
    fclose(f);
    uint32_t *w = malloc(len + 4);
    for (size_t i = 0; i + 4 <= len; i += 4)
        w[i / 4] = (uint32_t)b[i] << 24 | (uint32_t)b[i + 1] << 16 | (uint32_t)b[i + 2] << 8 | b[i + 3];
    *n = (uint32_t)(len / 4);
    return w;
}

static void vertex(uint32_t at, float x, float y)
{
    const float v[8] = {x, y, 0.5f, 1.0f, 1, 0, 0, 1};
    for (int i = 0; i < 8; i++)
        w32(at + 4 * (uint32_t)i, fbits(v[i]));
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: x360_window_test DIR OUT.ppm\n");
        return 2;
    }
    if (host_window_headless() || !host_window_open("x360_window_test", 256, 256, 0)) {
        printf("skipped: no display / no GLES 3 window\n");
        return 77;
    }
    char cache[1100], why[256];
    snprintf(cache, sizeof cache, "%s/cache", argv[1]);
    xenos_gl *g = xenos_gl_attach(cache, 64, 64, 2, why, sizeof why);
    if (!g) {
        printf("skipped: %s\n", why);
        host_window_close();
        return 77;
    }
    uint32_t nvs, nps;
    uint32_t *vs = load(argv[1], "vs.bin", &nvs), *ps = load(argv[1], "ps.bin", &nps);
    if (!vs || !ps) {
        fprintf(stderr, "no vs.bin / ps.bin in %s\n", argv[1]);
        return 1;
    }
    static xenos_cp cp;
    xenos_cp_init(&cp);
    cp.read32 = rd32;
    cp.write32 = wr32;
    cp.render_ctx = g;
    cp.draw_hook = (void (*)(void *, const xenos_cp *, const xenos_draw *))xenos_gl_draw;
    cp.swap_hook = (void (*)(void *, const xenos_cp *))xenos_gl_swap;
    static uint32_t im[2 + 1024];
    im[0] = 0, im[1] = nvs;
    memcpy(im + 2, vs, 4 * nvs);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nvs);
    im[0] = 1, im[1] = nps;
    memcpy(im + 2, ps, 4 * nps);
    type3(PM4_IM_LOAD_IMMEDIATE, im, 2 + nps);
    reg(0x2104, 0xF);
    reg(0x2201, 0x00010001u);
    reg(0x2206, 0x3F);
    reg(0x210F, fbits(32)), reg(0x2110, fbits(32)), reg(0x2111, fbits(-32)), reg(0x2112, fbits(32));
    vertex(0x1000, -0.5f, -0.5f);
    vertex(0x1020, 0.5f, -0.5f);
    vertex(0x1040, 0.0f, 0.5f);
    reg(0x4800, 0x1000u | 3u);
    reg(0x4801, 2u | 24u << 2);
    for (int i = 0; i < 4; i++)
        reg(0x4400 + (uint32_t)i, fbits(1.0f));
    uint32_t init = 4u | 2u << 6 | 3u << 16;
    type3(PM4_DRAW_INDX_2, &init, 1);
    static const uint32_t swap[4] = {0x53574150u, 0, 64, 64};
    type3(PM4_XE_SWAP, swap, 4);
    for (uint32_t i = 0; i < npk; i++)
        w32(0x8000 + 4 * i, pk[i]);
    xenos_cp_execute(&cp, 0x8000, npk);
    xenos_gl_end_frame(g);
    int w = 0, h = 0, rc = 0;
    unsigned tex = xenos_gl_frame_texture(g, &w, &h);
    if (!tex || w != 128 || h != 128) {
        fprintf(stderr, "FAIL frame texture %u %dx%d, want 128x128 (64 x scale 2)\n", tex, w, h);
        rc = 1;
    }
    host_window_set_scaling(HOST_SCALE_STRETCH, HOST_FILTER_NEAREST);
    host_window_capture(argv[2]);
    if (tex && !host_window_present_texture(tex, w, h, 1)) {
        fprintf(stderr, "FAIL present\n");
        rc = 1;
    }
    xenos_gl_destroy(g);
    host_window_close();
    /* the capture: a binary PPM of the drawable (256 x 256 or the window's real size) */
    FILE *f = fopen(argv[2], "rb");
    int cw = 0, ch = 0, mx = 0;
    if (!f || fscanf(f, "P6 %d %d %d", &cw, &ch, &mx) != 3 || cw <= 0 || ch <= 0) {
        fprintf(stderr, "FAIL no capture in %s\n", argv[2]);
        return 1;
    }
    fgetc(f);
    uint8_t *px = malloc((size_t)cw * ch * 3);
    size_t got = fread(px, 3, (size_t)cw * ch, f);
    fclose(f);
    if (got != (size_t)cw * ch) {
        fprintf(stderr, "FAIL short capture\n");
        return 1;
    }
#define P(x, y) (px + 3 * ((size_t)(y) * cw + (x)))
    const uint8_t *c = P(cw / 2, ch / 2), *k = P(2, 2), *top = P(cw / 2, ch / 4 + ch / 32),
                  *base = P(cw / 2 - cw / 8, 3 * ch / 4 - ch / 32);
    if (!(c[0] > 200 && c[1] < 50 && k[0] < 50 && top[0] > 200 && base[0] > 200)) {
        fprintf(stderr, "FAIL window picture: centre %u,%u,%u corner %u,%u,%u apex %u base %u\n", c[0], c[1], c[2],
                k[0], k[1], k[2], top[0], base[0]);
        rc = 1;
    }
    free(px);
    if (!rc)
        printf("x360 window: the attached renderer's frame presented the right way up (%dx%d capture)\n", cw, ch);
    return rc;
}
