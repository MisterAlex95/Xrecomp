/* resolve_write's direct way (WP-1) writes the bytes its generic way writes: for every pair it takes (RGBA8 to
 * 8_8_8_8 and its variants 6 / 14 / 50 / 62, RGB10_A2 to 2_10_10_10 7 / 54, floats to 16_16_16_16_FLOAT 32 and to
 * 32_FLOAT 36), each swap mode, with and without the red / blue exchange, rectangles that start and end inside a
 * 16-byte group, a destination that is not 16-byte aligned, and stores both into the writable memory pointer
 * (xenos_cp.wmem) and through write32. Every byte value of RGBA8 and every 10-bit value go through, and floats with
 * infinities, quiet NaNs, denormals, negative zero and rounding cases. Depth resolves (depth_write) against the
 * put_bytes loop they replaced. Prints the time of both ways for a 1920 x 1080 8_8_8_8 resolve. */
#include "../runtime/xenos_gl.c"

#include <time.h>

#define PHYS (8u << 20)
static uint8_t *g_mem, *g_noise; /* memory; the noise it starts each case with */

static uint32_t rd(void *ctx, uint32_t phys)
{
    (void)ctx;
    uint32_t v;
    memcpy(&v, g_mem + (phys % PHYS), 4);
    return __builtin_bswap32(v); /* the big-endian value the CPU sees */
}

static void wr(void *ctx, uint32_t phys, uint32_t v)
{
    (void)ctx;
    v = __builtin_bswap32(v);
    memcpy(g_mem + (phys % PHYS), &v, 4);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
    return rng;
}

static void fill_noise(uint8_t *p, size_t n)
{
    uint32_t s = 99;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        p[i] = (uint8_t)(s >> 24);
    }
}

/* a host picture of n texels in layout lay */
static void make_picture(uint8_t *px, int lay, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (lay == PIX_U8X4) {
            for (int c = 0; c < 4; c++) /* the first 256 texels: every value in every channel */
                px[4 * i + c] = (uint8_t)(i < 256 ? (i + 64 * (size_t)c) & 255 : rnd());
        } else if (lay == PIX_RGB10A2) {
            uint32_t v = i < 1024 ? (uint32_t)i | (uint32_t)(1023 - i) << 10 | (uint32_t)((i * 7) & 1023) << 20 |
                                        (uint32_t)(i & 3) << 30
                                  : rnd();
            memcpy(px + 4 * i, &v, 4);
        } else {
            static const uint32_t special[] = {0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC12345u, 0x00000001u,
                                               0x80000000u, 0x00000000u, 0x3F800000u, 0x477FE000u, 0x477FF000u,
                                               0x47800000u, 0x38800000u, 0x387FC000u, 0x33800000u, 0x33000000u,
                                               0x3F801000u, 0x3F800FFFu, 0xC0490FDBu, 0x00800000u, 0x7F7FFFFFu};
            for (int c = 0; c < 4; c++) {
                uint32_t b;
                size_t k = 4 * i + (size_t)c;
                if (k < sizeof special / sizeof special[0]) {
                    b = special[k];
                } else if (rnd() & 1) { /* within the half range */
                    float f = ((float)(int32_t)rnd() / 2147483648.0f) * (float)(1u << (rnd() % 17));
                    memcpy(&b, &f, 4);
                } else {
                    b = rnd();
                    if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu))
                        b |= 0x400000u; /* quiet NaNs only: a signalling one may come out quieted of either way */
                }
                memcpy(px + 16 * i + 4 * (size_t)c, &b, 4);
            }
        }
    }
}

static int fails;

static void one(int lay, uint32_t fmt, uint32_t endian, int swap_rb, int use_wmem, uint32_t base, int x0, int y0, int w,
                int h, uint8_t *ref, const uint8_t *px)
{
    xenos_cp cp;
    memset(&cp, 0, sizeof cp);
    cp.read32 = rd, cp.write32 = wr;
    if (use_wmem)
        cp.mem = g_mem, cp.wmem = g_mem;
    resolve_dest d = {base, 1280, fmt, endian, swap_rb, 0};
    memcpy(g_mem, g_noise, PHYS);
    if (!resolve_write_float(&cp, &d, px, lay, x0, y0, w, h)) {
        fprintf(stderr, "format %u: the generic way refused it\n", fmt);
        fails++;
        return;
    }
    memcpy(ref, g_mem, PHYS);
    memcpy(g_mem, g_noise, PHYS);
    if (!resolve_write_direct(&cp, &d, px, lay, x0, y0, w, h)) {
        fprintf(stderr, "layout %d format %u: no direct way\n", lay, fmt);
        fails++;
        return;
    }
    if (memcmp(ref, g_mem, PHYS)) {
        size_t at = 0;
        while (ref[at] == g_mem[at])
            at++;
        fprintf(stderr,
                "layout %d format %u endian %u swap_rb %d %s base %08X rect %d,%d %dx%d: first difference at %08zX "
                "(direct %02X, generic %02X)\n",
                lay, fmt, endian, swap_rb, use_wmem ? "wmem" : "write32", base, x0, y0, w, h, at, g_mem[at], ref[at]);
        fails++;
    }
}

int main(void)
{
    g_mem = malloc(PHYS), g_noise = malloc(PHYS);
    uint8_t *ref = malloc(PHYS);
    if (!g_noise)
        return 1;
    fill_noise(g_noise, PHYS);
    size_t npx = 1920 * 1080;
    uint8_t *px = malloc(npx * 16);
    if (!g_mem || !ref || !px)
        return 1;
    static const struct { int lay; uint32_t fmt; } pairs[] = {{PIX_U8X4, 6},     {PIX_U8X4, 14},    {PIX_U8X4, 50},
                                                              {PIX_U8X4, 62},    {PIX_RGB10A2, 7},  {PIX_RGB10A2, 54},
                                                              {PIX_F32X4, 32},   {PIX_F32X4, 36}};
    static const struct { int x0, y0, w, h; } rects[] = {{0, 0, 64, 64}, {3, 5, 61, 37}, {1, 30, 2, 3}, {0, 0, 1280, 6}};
    static const uint32_t bases[] = {0x200000u, 0x300004u, 0x400008u};
    int cases = 0;
    for (size_t p = 0; p < sizeof pairs / sizeof pairs[0]; p++) {
        make_picture(px, pairs[p].lay, 1280 * 64);
        for (uint32_t e = 0; e < 4; e++)
            for (int sw = 0; sw < 2; sw++)
                for (int wm = 0; wm < 2; wm++)
                    for (size_t r = 0; r < sizeof rects / sizeof rects[0]; r++)
                        for (size_t b = 0; b < sizeof bases / sizeof bases[0]; b++, cases++)
                            one(pairs[p].lay, pairs[p].fmt, e, sw, wm, bases[b], rects[r].x0, rects[r].y0, rects[r].w,
                                rects[r].h, ref, px);
    }
    /* what the direct way leaves to the generic one */
    {
        xenos_cp cp;
        memset(&cp, 0, sizeof cp);
        cp.read32 = rd, cp.write32 = wr, cp.mem = g_mem, cp.wmem = g_mem;
        static const struct { int lay; uint32_t fmt, base; int bias; } no[] = {
            {PIX_U8X4, 6, 0x200000u, 1},  {PIX_U8X4, 6, 0x200002u, 0}, {PIX_U8X4, 4, 0x200000u, 0},
            {PIX_U8X4, 32, 0x200000u, 0}, {PIX_RGB10A2, 6, 0x200000u, 0}, {PIX_F32X4, 6, 0x200000u, 0}};
        for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) {
            resolve_dest d = {no[i].base, 64, no[i].fmt, 0, 0, no[i].bias};
            if (resolve_write_direct(&cp, &d, px, no[i].lay, 0, 0, 8, 8)) {
                fprintf(stderr, "layout %d format %u base %08X bias %d: taken by the direct way\n", no[i].lay,
                        no[i].fmt, no[i].base, no[i].bias);
                fails++;
            }
        }
    }
    /* depth: depth_write against the put_bytes loop it replaced */
    {
        uint32_t *dv = malloc(64 * 64 * 4);
        uint8_t *sv = malloc(64 * 64);
        for (int i = 0; i < 64 * 64; i++)
            dv[i] = rnd() & 0xFFFFFFu, sv[i] = (uint8_t)rnd();
        for (int wm = 0; wm < 2; wm++)
            for (uint32_t e = 0; e < 4; e++)
                for (int st = 0; st < 2; st++)
                    for (uint32_t base = 0x200000u; base <= 0x200002u; base += 2, cases++) {
                        xenos_cp cp;
                        memset(&cp, 0, sizeof cp);
                        cp.read32 = rd, cp.write32 = wr;
                        if (wm)
                            cp.mem = g_mem, cp.wmem = g_mem;
                        const uint8_t *s = st ? sv : NULL;
                        memcpy(g_mem, g_noise, PHYS);
                        xenos_cp old = cp;
                        old.wmem = NULL;
                        for (int y = 2; y < 50; y++)
                            for (int x = 3; x < 64; x++) {
                                size_t at = (size_t)(y - 2) * 61 + (x - 3);
                                uint32_t v = dv[at] << 8 | (s ? s[at] : 0);
                                uint8_t b4[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
                                put_bytes(&old, base + (uint32_t)tiled2d(x, y, 64, 2), b4, 4, e);
                            }
                        memcpy(ref, g_mem, PHYS);
                        memcpy(g_mem, g_noise, PHYS);
                        depth_write(&cp, base, 64, e, dv, s, 3, 2, 61, 48);
                        if (memcmp(ref, g_mem, PHYS)) {
                            fprintf(stderr, "depth endian %u stencil %d %s base %08X: differs\n", e, st,
                                    wm ? "wmem" : "write32", base);
                            fails++;
                        }
                    }
        free(dv);
        free(sv);
    }
    /* time: a 1920 x 1080 RGBA8 resolve to 8_8_8_8, 8-in-32, red / blue exchanged (Direct3D's A8R8G8B8) */
    {
        free(g_mem);
        g_mem = calloc(1, 32u << 20);
        xenos_cp cp;
        memset(&cp, 0, sizeof cp);
        cp.read32 = rd, cp.write32 = wr, cp.mem = g_mem, cp.wmem = g_mem;
        make_picture(px, PIX_U8X4, npx);
        resolve_dest d = {0x100000u, 1920, 6, 2, 1, 0};
        double t0 = now_ms();
        resolve_write_float(&cp, &d, px, PIX_U8X4, 0, 0, 1920, 1080);
        double t1 = now_ms();
        for (int i = 0; i < 10; i++)
            resolve_write_direct(&cp, &d, px, PIX_U8X4, 0, 0, 1920, 1080);
        double t2 = now_ms();
        printf("1920 x 1080 8_8_8_8 resolve write: generic %.2f ms, direct %.2f ms\n", t1 - t0, (t2 - t1) / 10);
    }
    free(px);
    free(ref);
    free(g_mem);
    free(g_noise);
    if (fails) {
        fprintf(stderr, "%d of %d cases failed\n", fails, cases);
        return 1;
    }
    printf("resolve write: ok (%d cases)\n", cases);
    return 0;
}
