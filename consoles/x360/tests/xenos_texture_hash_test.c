/* The texture check's hash (XBA-318): with the physical memory pointer (xenos_cp.mem) the hash a cached texture is
 * checked with (decode_texture's hash_only) is the one its full decode stored, changes when a byte of its storage
 * changes, and costs far less than decoding; without the pointer (read32 a dword at a time) the same holds. Prints
 * the time of both checks for a tiled 1024 x 768 k_8 texture. */
#include "../runtime/xenos_gl.c"

#include <time.h>

#define PHYS (64u << 20) /* enough for the test: the texture lives low */
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

static int check(xenos_cp *cp, const uint32_t *fc, const char *what)
{
    decoded full, quick;
    if (!decode_texture(cp, fc, 0, &full) || !decode_texture(cp, fc, 1, &quick)) {
        fprintf(stderr, "%s: not decoded\n", what);
        return 1;
    }
    free_decoded(&full);
    if (full.hash != quick.hash) {
        fprintf(stderr, "%s: hash_only %016llx, full decode %016llx\n", what, (unsigned long long)quick.hash,
                (unsigned long long)full.hash);
        return 1;
    }
    uint64_t before = quick.hash;
    g_mem[0x100000 + 777] ^= 0x5A; /* a texel of the base level */
    decode_texture(cp, fc, 1, &quick);
    g_mem[0x100000 + 777] ^= 0x5A;
    if (quick.hash == before) {
        fprintf(stderr, "%s: a changed texel left the hash as it was\n", what);
        return 1;
    }
    double t0 = now_ms();
    for (int i = 0; i < 20; i++)
        decode_texture(cp, fc, 1, &quick);
    printf("%s: check of a tiled 1024 x 768 k_8 texture %.3f ms\n", what, (now_ms() - t0) / 20);
    return 0;
}

int main(void)
{
    g_mem = calloc(PHYS, 1);
    if (!g_mem)
        return 1;
    for (uint32_t i = 0; i < PHYS; i++)
        g_mem[i] = (uint8_t)(i * 2654435761u >> 24);
    /* fetch constant: type 2 (texture), tiled, pitch 1024; format 2 (k_8) at physical 0x100000; 1024 x 768, 2D */
    uint32_t fc[6] = {2u | 1u << 31 | (1024u / 32) << 22, 0x100000u | 2u, 1023u | 767u << 13, 0, 0, 1u << 9};
    xenos_cp cp;
    memset(&cp, 0, sizeof cp);
    cp.read32 = rd;
    int bad = check(&cp, fc, "read32");
    cp.mem = g_mem;
    bad |= check(&cp, fc, "mem");
    free(g_mem);
    if (!bad)
        puts("texture hash: ok");
    return bad;
}
