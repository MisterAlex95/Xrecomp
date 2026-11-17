/* gc_disc_bad_test -- the disc readers (gc_disc.c, gc_disc_wia.c) on damaged images built in memory (no game data):
 * a main DOL whose section end wraps past 4 GB, a WIA whose table sizes exceed the file, garbage behind each magic.
 * Exit 0, or 1 on a failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_disc.h"

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

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void be64(uint8_t *p, uint64_t v)
{
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}

/* A plain image: the header (magic, the DOL at 0x3000) and a DOL header whose one section is off + len. */
static void iso_with_dol(uint8_t *img, size_t n, uint32_t off, uint32_t len)
{
    memset(img, 0, n);
    memcpy(img, "GXRE01", 6);
    be32(img + 0x1C, GC_DISC_MAGIC);
    be32(img + 0x420, 0x3000);
    be32(img + 0x3000, off);
    be32(img + 0x3000 + 0x90, len);
}

static void test_dol_size(void)
{
    static uint8_t img[0x4000];
    char why[200] = "";
    iso_with_dol(img, sizeof img, 0x100, 0x1000);
    gc_disc *d = gc_disc_open_mem(img, sizeof img, why, sizeof why);
    CHECK(d != NULL, "plain image: %s", why);
    if (d) {
        CHECK(gc_disc_dol_size(d) == 0x1100, "DOL size 0x%X", gc_disc_dol_size(d));
        gc_disc_close(d);
    }
    /* 0xFFFFFF00 + 0x200 wrapped to 0x100 in 32 bits: a 256-byte DOL instead of an unreadable one */
    iso_with_dol(img, sizeof img, 0xFFFFFF00u, 0x200);
    d = gc_disc_open_mem(img, sizeof img, why, sizeof why);
    CHECK(d != NULL, "plain image: %s", why);
    if (d) {
        CHECK(gc_disc_dol_size(d) == 0, "a wrapping section gives DOL size 0x%X, want 0", gc_disc_dol_size(d));
        gc_disc_close(d);
    }
}

/* A WIA head + disc struct for a GameCube disc, tables at raw_off / group_off of the sizes given. */
static size_t wia(uint8_t *f, size_t cap, uint32_t raw_size, uint32_t group_size)
{
    memset(f, 0, cap);
    memcpy(f, "WIA\x01", 4);
    be32(f + 0x04, 0x01000000);
    be32(f + 0x08, 0x00090000);
    be32(f + 0x0C, 0xDC);
    be64(f + 0x24, 0x100000);
    be64(f + 0x2C, cap);
    uint8_t *d = f + 0x48;
    be32(d + 0x00, 1);      /* GameCube */
    be32(d + 0x04, 0);      /* stored */
    be32(d + 0x0C, 0x8000); /* chunk */
    memcpy(d + 0x10, "GXRE01", 6);
    be32(d + 0x10 + 0x1C, GC_DISC_MAGIC);
    be32(d + 0xB4, 1); /* raw data entries */
    be64(d + 0xB8, 0x200);
    be32(d + 0xC0, raw_size);
    be32(d + 0xC4, 1); /* groups */
    be64(d + 0xC8, 0x300);
    be32(d + 0xD0, group_size);
    return cap;
}

static void test_wia_tables(void)
{
    static uint8_t f[0x400];
    char why[200] = "";
    /* a raw data table of 4 GB in a 1 KB file: refused before anything is allocated for it */
    size_t n = wia(f, sizeof f, 0xFFFFFFF0u, 8);
    gc_disc *d = gc_disc_open_mem(f, n, why, sizeof why);
    CHECK(d == NULL, "a 4 GB table opened");
    CHECK(strstr(why, "past the file's end") != NULL, "why: %s", why);
    gc_disc_close(d);
    /* the group table too */
    n = wia(f, sizeof f, 0x18, 0xFFFFFFF0u);
    d = gc_disc_open_mem(f, n, why, sizeof why);
    CHECK(d == NULL, "a 4 GB group table opened");
    CHECK(strstr(why, "past the file's end") != NULL, "why: %s", why);
    gc_disc_close(d);
}

/* Garbage behind each format's magic: refused or read, never a crash (run under the sanitizers when in doubt). */
static void test_garbage(void)
{
    static const char *const magic[] = {"CISO", "\x01\xC0\x0B\xB1", "WIA\x01", "RVZ\x01"};
    static uint8_t f[0x9000];
    uint32_t x = 12345;
    for (int k = 0; k < 4; k++)
        for (int round = 0; round < 64; round++) {
            for (size_t i = 0; i < sizeof f; i++) {
                x = x * 1103515245u + 12345u;
                f[i] = (uint8_t)(x >> 16);
            }
            memcpy(f, magic[k], 4);
            if (k == 0) /* CISO: a valid block size, so the map is read */
                memcpy(f + 4, "\x00\x80\x00\x00", 4);
            char why[200] = "";
            gc_disc *d = gc_disc_open_mem(f, sizeof f, why, sizeof why);
            if (d) {
                uint8_t buf[0x100];
                gc_disc_read(d, 0, buf, sizeof buf);
                gc_disc_read(d, 0x123456, buf, sizeof buf);
                gc_disc_close(d);
            }
        }
}

int main(void)
{
    test_dol_size();
    test_wia_tables();
    test_garbage();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gc_disc_bad_test: ok\n");
    return 0;
}
