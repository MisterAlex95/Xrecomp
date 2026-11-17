/* gx_progcache_test -- the GX program binary cache file (consoles/gc/runtime/gx/gx_progcache.h, XBA-264), no GL:
 * save then load gives the entries back; a file of another key (driver or shader generator), truncated, with a byte
 * changed, of another format or missing is ignored with the right reason; replace / drop / find; the key follows the
 * driver strings, the GL flavour and GC_GX_DRIVER_KEY; the source hash follows the TEV state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx/gx_progcache.h"

static int fails;
#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                                   \
            fails++;                                                                                                  \
        }                                                                                                             \
    } while (0)

static gx_shader_key key_n(unsigned n)
{
    gx_shader_key k;
    memset(&k, 0, sizeof k);
    k.nst = 1 + n % 16;
    k.st_c[0] = 0x8FFFF0u ^ n;
    k.st_a[0] = 0x08FFF0u + n;
    return k;
}

static long file_size(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void flip_byte(const char *p, long at)
{
    FILE *f = fopen(p, "r+b");
    fseek(f, at, SEEK_SET);
    int c = fgetc(f);
    fseek(f, at, SEEK_SET);
    fputc(c ^ 0x40, f);
    fclose(f);
}

static void truncate_to(const char *p, long n)
{
    FILE *f = fopen(p, "rb");
    char *buf = malloc((size_t)n);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    f = fopen(p, "wb");
    fwrite(buf, 1, got, f);
    fclose(f);
    free(buf);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024], tmp[1100];
    snprintf(path, sizeof path, "%s/gx_programs_test.bin", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    remove(path);
    unsetenv("GC_GX_DRIVER_KEY");

    /* the key */
    uint64_t k1 = gx_progcache_key("Mesa", "llvmpipe", "OpenGL ES 3.2 Mesa 24", 0);
    CHECK(k1 == gx_progcache_key("Mesa", "llvmpipe", "OpenGL ES 3.2 Mesa 24", 0));
    CHECK(k1 != gx_progcache_key("Mesa", "llvmpipe", "OpenGL ES 3.2 Mesa 25", 0));
    CHECK(k1 != gx_progcache_key("Qualcomm", "llvmpipe", "OpenGL ES 3.2 Mesa 24", 0));
    CHECK(k1 != gx_progcache_key("Mesa", "Adreno (TM) 740", "OpenGL ES 3.2 Mesa 24", 0));
    CHECK(k1 != gx_progcache_key("Mesa", "llvmpipe", "OpenGL ES 3.2 Mesa 24", 1));
    CHECK(k1 != gx_progcache_key("Mesa", "llvmpipeOpenGL ES 3.2", " Mesa 24", 0)); /* fields apart */
    setenv("GC_GX_DRIVER_KEY", "update", 1);
    CHECK(k1 != gx_progcache_key("Mesa", "llvmpipe", "OpenGL ES 3.2 Mesa 24", 0));
    unsetenv("GC_GX_DRIVER_KEY");
    CHECK(k1 == gx_progcache_key("Mesa", "llvmpipe", "OpenGL ES 3.2 Mesa 24", 0));

    /* the source hash */
    gx_shader_key a = key_n(1), b = key_n(2);
    CHECK(gx_progcache_src_hash(&a) != 0);
    CHECK(gx_progcache_src_hash(&a) == gx_progcache_src_hash(&a));
    CHECK(gx_progcache_src_hash(&a) != gx_progcache_src_hash(&b));

    /* missing */
    gx_progcache c;
    const char *why = NULL;
    char was[256];
    CHECK(gx_progcache_load(path, k1, "drv", &c, &why, was, sizeof was) == -1 && !strcmp(why, "none"));
    CHECK(c.n == 0 && c.key == k1);
    gx_progcache_free(&c);

    /* save, load */
    gx_progcache_init(&c, k1, "Mesa | llvmpipe | OpenGL ES 3.2 Mesa 24");
    uint8_t blob[300];
    for (unsigned i = 0; i < 20; i++) {
        gx_shader_key k = key_n(i);
        for (unsigned j = 0; j < sizeof blob; j++)
            blob[j] = (uint8_t)(i * 7 + j);
        CHECK(gx_progcache_put(&c, &k, 1000 + i, 0x8E21u + i, blob, 100 + i * 10) == 0);
    }
    CHECK(c.n == 20);
    gx_shader_key k3 = key_n(3);
    CHECK(gx_progcache_put(&c, &k3, 77, 5, blob, 8) == 0); /* replaced, not added */
    CHECK(c.n == 20);
    CHECK(gx_progcache_find(&c, &k3) && gx_progcache_find(&c, &k3)->len == 8 && gx_progcache_find(&c, &k3)->format == 5);
    CHECK(gx_progcache_put(&c, &k3, 77, 5, blob, 0) == -1); /* empty binaries are refused */
    CHECK(gx_progcache_save(path, &c) == 0);
    CHECK(file_size(tmp) == -1);
    gx_progcache d;
    CHECK(gx_progcache_load(path, k1, "now", &d, &why, was, sizeof was) == 0 && why == NULL);
    CHECK(!strcmp(was, "Mesa | llvmpipe | OpenGL ES 3.2 Mesa 24"));
    CHECK(d.n == 20);
    for (unsigned i = 0; i < 20; i++) {
        gx_shader_key k = key_n(i);
        gx_progbin *e = gx_progcache_find(&d, &k), *o = gx_progcache_find(&c, &k);
        CHECK(e && o && e->len == o->len && e->format == o->format && e->src_hash == o->src_hash &&
              !memcmp(e->data, o->data, e->len));
    }
    /* drop */
    gx_progcache_drop(&d, 0);
    CHECK(d.n == 19);
    gx_progcache_free(&d);

    /* another key: ignored, the file's driver told */
    uint64_t k2 = gx_progcache_key("Mesa", "llvmpipe", "OpenGL ES 3.2 Mesa 25", 0);
    CHECK(gx_progcache_load(path, k2, "new", &d, &why, was, sizeof was) == -1);
    CHECK(why && !strcmp(why, "another driver or shader generator"));
    CHECK(!strcmp(was, "Mesa | llvmpipe | OpenGL ES 3.2 Mesa 24"));
    CHECK(d.n == 0 && d.key == k2 && !strcmp(d.driver, "new"));
    gx_progcache_free(&d);

    /* a changed byte: checksum */
    long size = file_size(path);
    flip_byte(path, size / 2);
    CHECK(gx_progcache_load(path, k1, "", &d, &why, NULL, 0) == -1 && !strcmp(why, "checksum") && d.n == 0);
    gx_progcache_free(&d);

    /* truncated (at every few sizes) */
    CHECK(gx_progcache_save(path, &c) == 0);
    for (long n = size - 1; n > 0; n -= size / 7 + 1) {
        CHECK(gx_progcache_save(path, &c) == 0);
        truncate_to(path, n);
        int rc = gx_progcache_load(path, k1, "", &d, &why, NULL, 0);
        CHECK(rc == -1 && d.n == 0);
        CHECK(!strcmp(why, "truncated") || !strcmp(why, "checksum"));
        gx_progcache_free(&d);
    }
    /* empty file, other format */
    FILE *f = fopen(path, "wb");
    fclose(f);
    CHECK(gx_progcache_load(path, k1, "", &d, &why, NULL, 0) == -1 && !strcmp(why, "truncated"));
    gx_progcache_free(&d);
    f = fopen(path, "wb");
    fputs("# xrecomp gx shader list v1\nT 1 0 0 0 0 0 0 0\n", f);
    fclose(f);
    CHECK(gx_progcache_load(path, k1, "", &d, &why, NULL, 0) == -1 && !strcmp(why, "not a program cache"));
    gx_progcache_free(&d);

    /* an empty cache round-trips */
    gx_progcache e;
    gx_progcache_init(&e, k2, "x");
    CHECK(gx_progcache_save(path, &e) == 0);
    CHECK(gx_progcache_load(path, k2, "", &d, &why, NULL, 0) == 0 && d.n == 0);
    gx_progcache_free(&d);
    gx_progcache_free(&e);
    gx_progcache_free(&c);
    /* an unwritable place: -1, nothing left behind */
    CHECK(gx_progcache_save("/nonexistent-dir/gx_programs.bin", &c) == -1);
    remove(path);
    if (fails) {
        fprintf(stderr, "gx_progcache: %d check(s) failed\n", fails);
        return 1;
    }
    printf("gx_progcache: all checks passed\n");
    return 0;
}
