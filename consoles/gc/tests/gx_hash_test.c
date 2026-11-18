/* gx_hash_test -- the content hash behind the GX texture cache (gx_tex.c). A texture is reused when this value matches,
 * so a byte the hash misses is a stale picture on screen: every bit of every position must change it, at any length and
 * alignment, as must moving data between the lanes, the blocks, the length and the seed. No GL. Exit 0, or 1. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx/gx_internal.h"

#define MAX_LEN 300u

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static uint64_t rng_state = 0x0123456789ABCDEFull;

static uint8_t rng8(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint8_t)(rng_state >> 32);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(void)
{
    static uint8_t store[MAX_LEN + 64], copy[MAX_LEN + 64];

    /* Each bit of each byte, at every length up to several blocks and at the eight alignments of a word. */
    for (unsigned align = 0; align < 8; align++) {
        uint8_t *p = store + align;
        for (unsigned n = 0; n <= MAX_LEN; n++) {
            for (unsigned i = 0; i < n; i++)
                p[i] = rng8();
            uint64_t h = gx_hash(p, n, 7);
            CHECK(h == gx_hash(p, n, 7), "not deterministic at length %u", n);
            memcpy(copy + 3, p, n);
            CHECK(h == gx_hash(copy + 3, n, 7), "depends on the address at length %u, alignment %u", n, align);
            for (unsigned i = 0; i < n; i++)
                for (unsigned b = 0; b < 8; b++) {
                    p[i] ^= (uint8_t)(1u << b);
                    CHECK(gx_hash(p, n, 7) != h, "bit %u of byte %u missed at length %u, alignment %u", b, i, n, align);
                    p[i] ^= (uint8_t)(1u << b);
                }
        }
    }

    /* Words exchanged inside a block (between lanes) and whole blocks exchanged (between rounds). */
    for (unsigned i = 0; i < 256; i++)
        store[i] = rng8();
    uint64_t base = gx_hash(store, 256, 0);
    for (unsigned a = 0; a < 32; a++)
        for (unsigned b = a + 1; b < 32; b++) {
            uint8_t wa[8], wb[8];
            memcpy(wa, store + a * 8, 8);
            memcpy(wb, store + b * 8, 8);
            if (!memcmp(wa, wb, 8))
                continue;
            memcpy(store + a * 8, wb, 8);
            memcpy(store + b * 8, wa, 8);
            CHECK(gx_hash(store, 256, 0) != base, "words %u and %u exchanged", a, b);
            memcpy(store + a * 8, wa, 8);
            memcpy(store + b * 8, wb, 8);
        }
    for (unsigned a = 0; a < 8; a++)
        for (unsigned b = a + 1; b < 8; b++) {
            uint8_t ba[32], bb[32];
            memcpy(ba, store + a * 32, 32);
            memcpy(bb, store + b * 32, 32);
            memcpy(store + a * 32, bb, 32);
            memcpy(store + b * 32, ba, 32);
            CHECK(gx_hash(store, 256, 0) != base, "blocks %u and %u exchanged", a, b);
            memcpy(store + a * 32, ba, 32);
            memcpy(store + b * 32, bb, 32);
        }

    /* All-zero data: only the length and the seed tell these apart (cleared textures of different sizes). */
    memset(store, 0, sizeof store);
    static uint64_t seen[MAX_LEN + 1];
    for (unsigned n = 0; n <= MAX_LEN; n++)
        seen[n] = gx_hash(store, n, 0);
    qsort(seen, MAX_LEN + 1, sizeof seen[0], cmp_u64);
    for (unsigned n = 0; n < MAX_LEN; n++)
        CHECK(seen[n] != seen[n + 1], "two lengths of zeros hash alike");
    for (unsigned n = 0; n <= MAX_LEN; n += 13)
        CHECK(gx_hash(store, n, 0) != gx_hash(store, n, 1), "the seed is ignored at length %u", n);

    /* Many similar textures: a counter in an otherwise constant 4 KB block must never collide. */
    enum { N = 200000 };
    static uint8_t tex[4096];
    uint64_t *all = malloc(N * sizeof *all);
    if (!all) {
        printf("FAIL out of memory\n");
        return 1;
    }
    for (uint32_t i = 0; i < N; i++) {
        memcpy(tex + (i % 16) * 256, &i, sizeof i);
        all[i] = gx_hash(tex, sizeof tex, 0);
        memset(tex + (i % 16) * 256, 0, sizeof i);
    }
    qsort(all, N, sizeof *all, cmp_u64);
    unsigned dup = 0;
    for (uint32_t i = 0; i + 1 < N; i++)
        dup += all[i] == all[i + 1];
    CHECK(dup == 0, "%u collisions among %u near-identical blocks", dup, (unsigned)N);
    free(all);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gx_hash_test: ok\n");
    return 0;
}
