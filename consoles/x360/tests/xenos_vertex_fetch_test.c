/* fetch_vertices with physical memory at hand (cp->mem: the float formats' and, WP-36, the packed formats' loops per
 * swap mode) gives the floats its generic way (gpu_read through read32) gives, bit for bit: every vertex format it
 * takes, each swap mode, signed / unsigned, normalised / integer, with and without an exponent adjustment, strides
 * and offsets that are not multiples of the format's size, words of noise (every 16-bit pattern, half floats among
 * them). read_bytes (texture rows) likewise, at any address and length. Prints the time of both ways for 8_8_8_8. */
#include "../runtime/xenos_gl.c"

#include <time.h>

#define PHYS (1u << 20)
static uint8_t *g_mem;

static uint32_t rd(void *ctx, uint32_t phys)
{
    (void)ctx;
    uint32_t v;
    memcpy(&v, g_mem + (phys % PHYS), 4);
    return __builtin_bswap32(v); /* the big-endian value the CPU sees */
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(void)
{
    g_mem = malloc(PHYS);
    if (!g_mem)
        return 1;
    uint32_t s = 7;
    for (uint32_t i = 0; i < PHYS; i++) {
        s = s * 1664525u + 1013904223u;
        g_mem[i] = (uint8_t)(s >> 24);
    }
    for (uint32_t i = 0; i < 65536; i++) /* every 16-bit pattern at the start (half floats, 16-bit fields) */
        g_mem[2 * i] = (uint8_t)i, g_mem[2 * i + 1] = (uint8_t)(i >> 8);
    xenos_cp gen, mem;
    memset(&gen, 0, sizeof gen);
    gen.read32 = rd;
    mem = gen;
    mem.mem = g_mem;
    static const uint32_t formats[] = {6, 7, 16, 17, 25, 26, 31, 32, 33, 34, 35, 36, 37, 38, 57};
    static const struct { uint32_t stride, offset; } layouts[] = {{1, 0}, {2, 0}, {3, 1}, {8, 2}, {7, 5}};
    const uint32_t count = 16384;
    const size_t nf = (size_t)count * 12; /* floats: two attributes interleaved (ostride 8) and room */
    float *a_out = malloc(nf * sizeof(float)), *b_out = malloc(nf * sizeof(float));
    if (!a_out || !b_out)
        return 1;
    int fails = 0, cases = 0;
    for (size_t f = 0; f < sizeof formats / sizeof formats[0]; f++)
        for (uint32_t e = 0; e < 4; e++)
            for (int sg = 0; sg < 2; sg++)
                for (int in = 0; in < 2; in++)
                    for (int ex = 0; ex < 3; ex++)
                        for (size_t l = 0; l < sizeof layouts / sizeof layouts[0]; l++, cases++) {
                            attr at = {0, layouts[l].offset, formats[f], layouts[l].stride, (uint32_t)sg, (uint32_t)in,
                                       ex == 0 ? 0 : ex == 1 ? -5 : 3};
                            uint32_t first = (uint32_t)l * 3, n = count / (layouts[l].stride + 1);
                            memset(a_out, 0xA5, nf * sizeof(float));
                            memset(b_out, 0xA5, nf * sizeof(float));
                            /* the second of two interleaved attributes, as the draw lays them out */
                            int ra = fetch_vertices(&gen, 0x40u * (uint32_t)l, e, &at, first, n, a_out + 4, 8);
                            int rb = fetch_vertices(&mem, 0x40u * (uint32_t)l, e, &at, first, n, b_out + 4, 8);
                            if (ra != 1 || rb != 1 || memcmp(a_out, b_out, nf * sizeof(float))) {
                                size_t k = 0;
                                while (k < nf && !memcmp(&a_out[k], &b_out[k], 4))
                                    k++;
                                fprintf(stderr,
                                        "format %u endian %u signed %d integer %d exp %d stride %u offset %u: %d / %d, "
                                        "first difference at float %zu\n",
                                        formats[f], e, sg, in, at.exp_adjust, at.stride, at.offset, ra, rb, k);
                                fails++;
                            }
                        }
    /* read_bytes (texture rows) with memory at hand reads whole dwords at a time (WP-36): the same bytes, any start,
     * length and swap mode */
    {
        uint8_t ra[300], rb[300];
        uint32_t r = 1;
        for (int k = 0; k < 20000; k++, cases++) {
            r ^= r << 13, r ^= r >> 17, r ^= r << 5;
            uint32_t addr = r % (PHYS - 512), n = (r >> 20) % 290, e = (r >> 11) & 3;
            memset(ra, 0x5A, sizeof ra), memset(rb, 0x5A, sizeof rb);
            read_bytes(&gen, addr, n, e, ra);
            read_bytes(&mem, addr, n, e, rb);
            if (memcmp(ra, rb, sizeof ra)) {
                fprintf(stderr, "read_bytes at %08X, %u bytes, endian %u: memory and read32 differ\n", addr, n, e);
                fails++;
                break;
            }
        }
    }
    attr at = {0, 0, 6, 1, 0, 0, 0};
    double t0 = now_ms();
    for (int r = 0; r < 20; r++)
        fetch_vertices(&gen, 0, 2, &at, 0, count * 2, a_out, 4);
    double t1 = now_ms();
    for (int r = 0; r < 20; r++)
        fetch_vertices(&mem, 0, 2, &at, 0, count * 2, b_out, 4);
    double t2 = now_ms();
    printf("%u vertices of 8_8_8_8 x 20: generic %.2f ms, from memory %.2f ms\n", count * 2, t1 - t0, t2 - t1);
    if (fails)
        return 1;
    printf("vertex fetch: ok (%d cases)\n", cases);
    return 0;
}
