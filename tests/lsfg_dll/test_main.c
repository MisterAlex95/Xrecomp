/* src/framegen/lsfg_dll.c and lsfg_cache.c: the PE resource walker and the shader cache, on synthetic PE files
 * built here (our own bytes: a PE32+ header, one .rsrc section, RT_RCDATA entries with made-up DXBC / SPIR-V
 * blobs). No Lossless.dll, no shader from it. Work files go to $TMPDIR (or /tmp). */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "lsfg_cache.h"
#include "lsfg_dll.h"
#include "lsfg_spv.h"
#include "../lsfg_spv/spv_build.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long a_ = (long)(a), b_ = (long)(b); if (a_ != b_) { \
    fprintf(stderr, "FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #a, a_, b_); g_fail++; } } while (0)

/* ── synthetic PE builder ─────────────────────────────────────────────────── */

typedef struct PeRes {
    uint32_t id;
    const uint8_t *data;
    uint32_t size;
} PeRes;

typedef struct Pe {
    uint8_t *b;
    size_t len;
    size_t rsrc_off;   /* file offset of the resource section */
    size_t name_dir;   /* offset (in the section) of the RCDATA name directory */
    size_t lang0;      /* offset of the first language directory */
    size_t data0;      /* offset of the first data entry */
} Pe;

static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }

#define SEC_VA 0x1000u
#define SEC_RAW 0x400u

/* PE32+ (pe32 = 0) or PE32 (pe32 = 1) with RT_VERSION (empty) and RT_RCDATA holding res[0..n). */
static Pe build_pe(const PeRes *res, size_t n, int pe32)
{
    size_t root = 0, ver_dir = 16 + 2 * 8, name_dir = ver_dir + 16, lang = name_dir + 16 + 8 * n;
    size_t data_e = lang + 24 * n, blobs = data_e + 16 * n, total = blobs;
    for (size_t i = 0; i < n; i++)
        total += (res[i].size + 3) & ~3u;
    Pe pe = { 0 };
    pe.len = SEC_RAW + total;
    pe.b = calloc(1, pe.len);
    pe.rsrc_off = SEC_RAW;
    pe.name_dir = name_dir;
    pe.lang0 = lang;
    pe.data0 = data_e;
    uint8_t *b = pe.b;
    b[0] = 'M'; b[1] = 'Z';
    w32(b + 0x3c, 0x80);
    memcpy(b + 0x80, "PE\0\0", 4);
    uint8_t *coff = b + 0x84;
    w16(coff, 0x8664);
    w16(coff + 2, 1);                                  /* one section */
    uint32_t opt_size = pe32 ? 96 + 16 * 8 : 112 + 16 * 8;
    w16(coff + 16, opt_size);
    uint8_t *opt = coff + 20;
    w16(opt, pe32 ? 0x10b : 0x20b);
    w32(opt + (pe32 ? 92 : 108), 16);
    uint8_t *dirs = opt + (pe32 ? 96 : 112);
    w32(dirs + 16, SEC_VA);                            /* resource directory */
    w32(dirs + 20, (uint32_t)total);
    uint8_t *sec = opt + opt_size;
    memcpy(sec, ".rsrc\0\0\0", 8);
    w32(sec + 8, (uint32_t)total);
    w32(sec + 12, SEC_VA);
    w32(sec + 16, (uint32_t)total);
    w32(sec + 20, SEC_RAW);
    uint8_t *r = b + SEC_RAW;
    w16(r + root + 14, 2);                             /* two id entries: RT_RCDATA (10), RT_VERSION (16) */
    w32(r + root + 16, 10);
    w32(r + root + 20, 0x80000000u | (uint32_t)name_dir);
    w32(r + root + 24, 16);
    w32(r + root + 28, 0x80000000u | (uint32_t)ver_dir);
    w16(r + name_dir + 14, (uint32_t)n);
    size_t blob = blobs;
    for (size_t i = 0; i < n; i++) {
        uint8_t *e = r + name_dir + 16 + 8 * i;
        w32(e, res[i].id);
        w32(e + 4, 0x80000000u | (uint32_t)(lang + 24 * i));
        uint8_t *l = r + lang + 24 * i;
        w16(l + 14, 1);
        w32(l + 16, 0x409);
        w32(l + 20, (uint32_t)(data_e + 16 * i));
        uint8_t *d = r + data_e + 16 * i;
        w32(d, SEC_VA + (uint32_t)blob);
        w32(d + 4, res[i].size);
        memcpy(r + blob, res[i].data, res[i].size);
        blob += (res[i].size + 3) & ~3u;
    }
    return pe;
}

/* ── walker ───────────────────────────────────────────────────────────────── */

struct seen {
    uint32_t ids[64], sizes[64];
    uint8_t first[64];
    int n;
};

static int collect_cb(const LsfgDllResource *res, const uint8_t *data, void *user)
{
    struct seen *s = user;
    if (s->n < 64) {
        s->ids[s->n] = res->id;
        s->sizes[s->n] = res->size;
        s->first[s->n] = res->size ? data[0] : 0;
        s->n++;
    }
    return 0;
}

static int stop_cb(const LsfgDllResource *res, const uint8_t *data, void *user)
{
    (void)data; (void)user;
    return res->id == 255 ? 42 : 0;
}

static void test_walk(int pe32)
{
    uint8_t a[5] = { 'a', 1, 2, 3, 4 }, dx[40] = "DXBC", other[3] = { 'z', 'z', 'z' };
    PeRes res[] = { { 101, a, 5 }, { 255, dx, 40 }, { 256, dx, 40 }, { 999, other, 3 } };
    Pe pe = build_pe(res, 4, pe32);
    struct seen s = { 0 };
    CHECK_EQ(lsfg_dll_walk(pe.b, pe.len, collect_cb, &s), 0);
    CHECK_EQ(s.n, 4);
    CHECK_EQ(s.ids[0], 101); CHECK_EQ(s.sizes[0], 5); CHECK_EQ(s.first[0], 'a');
    CHECK_EQ(s.ids[1], 255); CHECK_EQ(s.sizes[1], 40); CHECK_EQ(s.first[1], 'D');
    CHECK_EQ(s.ids[3], 999); CHECK_EQ(s.sizes[3], 3);
    CHECK_EQ(lsfg_dll_walk(pe.b, pe.len, stop_cb, NULL), 42);
    LsfgDllResource r;
    const uint8_t *d;
    CHECK_EQ(lsfg_dll_find(pe.b, pe.len, 256, &r, &d), 0);
    CHECK(r.size == 40 && memcmp(d, "DXBC", 4) == 0 && r.lang == 0x409);
    CHECK_EQ(r.file_offset, (uint32_t)(d - pe.b));
    CHECK_EQ(lsfg_dll_find(pe.b, pe.len, 257, NULL, NULL), LSFG_DLL_ENORSRC);

    /* Every truncation fails cleanly (never reads out of bounds: run under ASan). The last byte is the padding
     * after the 3-byte blob, so the walk only needs len - 1 bytes. */
    for (size_t k = 0; k < pe.len - 1; k++) {
        uint8_t *t = malloc(k ? k : 1);
        memcpy(t, pe.b, k);
        int rc = lsfg_dll_walk(t, k, collect_cb, &(struct seen){ 0 });
        CHECK(rc < 0);
        free(t);
    }
    free(pe.b);
}

static void test_malformed(void)
{
    uint8_t dx[40] = "DXBC";
    PeRes res[] = { { 255, dx, 40 }, { 256, dx, 40 } };
    Pe pe = build_pe(res, 2, 0);
    uint8_t *r = pe.b + pe.rsrc_off;
    struct seen s;
#define MUTATE(stmt, want) do { Pe c_ = pe; c_.b = malloc(pe.len); memcpy(c_.b, pe.b, pe.len); \
        uint8_t *R = c_.b + pe.rsrc_off; (void)R; uint8_t *B = c_.b; (void)B; stmt; memset(&s, 0, sizeof s); \
        CHECK_EQ(lsfg_dll_walk(c_.b, c_.len, collect_cb, &s), want); free(c_.b); } while (0)
    (void)r;
    MUTATE(B[0] = 'X', LSFG_DLL_ENOTPE);
    MUTATE(B[0x81] = 'X', LSFG_DLL_ENOTPE);
    MUTATE(w32(B + 0x3c, 0xfffffff0u), LSFG_DLL_ENOTPE);
    MUTATE(w16(B + 0x84 + 20, 0x30b), LSFG_DLL_ENOTPE);                      /* optional header magic */
    MUTATE(w32(B + 0x84 + 20 + 112 + 16, 0), LSFG_DLL_ENORSRC);              /* no resource directory */
    MUTATE(w32(R + 16, 3), LSFG_DLL_ENORSRC);                               /* no RT_RCDATA type */
    MUTATE(w32(R + 20, 0x80000000u), LSFG_DLL_ECORRUPT);                    /* RCDATA -> the root again */
    MUTATE(w32(R + 20, 40), LSFG_DLL_ECORRUPT);                             /* RCDATA is a leaf */
    MUTATE(w16(R + pe.name_dir + 14, 60000), LSFG_DLL_ECORRUPT);            /* huge entry count */
    MUTATE(w32(R + pe.name_dir + 20, (uint32_t)pe.data0), LSFG_DLL_ECORRUPT); /* name level is a leaf */
    MUTATE(w32(R + pe.lang0 + 20, 0x80000000u | (uint32_t)pe.lang0), LSFG_DLL_ECORRUPT); /* a 4th level */
    MUTATE(w32(R + pe.lang0 + 20, 0x7ffffff0u), LSFG_DLL_ECORRUPT);         /* data entry out of the section */
    MUTATE(w32(R + pe.data0 + 4, 0x10000000u), LSFG_DLL_ECORRUPT);          /* size past the end */
    MUTATE(w32(R + pe.data0, 0x9000), LSFG_DLL_ECORRUPT);                   /* RVA outside every section */
    MUTATE(w16(B + 0x84 + 2, 0), LSFG_DLL_ECORRUPT);                        /* no sections */
#undef MUTATE
    CHECK_EQ(lsfg_dll_walk(NULL, 10, collect_cb, NULL), LSFG_DLL_EINVAL);
    CHECK(strcmp(lsfg_dll_strerror(LSFG_DLL_ENOTPE), "not a Windows DLL (PE) file") == 0);
    free(pe.b);
}

/* ── files ────────────────────────────────────────────────────────────────── */

static char g_tmp[2048];

static void rm_rf(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    char p[4096];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
            rm_rf(p);
        else
            unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static int file_exists(const char *dir, const char *name)
{
    char p[4096];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return stat(p, &st) == 0;
}

static void test_read_fd(void)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/blob.bin", g_tmp);
    uint8_t data[3000];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(i * 7);
    FILE *f = fopen(path, "wb");
    fwrite(data, 1, sizeof data, f);
    fclose(f);
    int fd = open(path, O_RDONLY);
    lseek(fd, 100, SEEK_SET);   /* the position is not used */
    uint8_t *buf = NULL;
    size_t len = 0;
    CHECK_EQ(lsfg_dll_read_fd(fd, &buf, &len), 0);
    CHECK(len == sizeof data && memcmp(buf, data, len) == 0);
    free(buf);
    close(fd);
    CHECK_EQ(lsfg_dll_read_fd(-1, &buf, &len), LSFG_DLL_EINVAL);
    CHECK_EQ(lsfg_dll_read_path("/nonexistent/x.dll", &buf, &len), LSFG_DLL_EIO);
}

static void test_sha256(void)
{
    static const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    static const uint8_t two_blocks[32] = {   /* "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" */
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1 };
    static const uint8_t empty[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55 };
    uint8_t out[32];
    lsfg_sha256("abc", 3, out);
    CHECK(memcmp(out, abc, 32) == 0);
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    lsfg_sha256(m, strlen(m), out);
    CHECK(memcmp(out, two_blocks, 32) == 0);
    lsfg_sha256("", 0, out);
    CHECK(memcmp(out, empty, 32) == 0);
}

/* ── cache ────────────────────────────────────────────────────────────────── */

/* A made-up DXBC container for resource id: magic, the id, padding. The fake translator reads the id back. */
static void fake_dxbc(uint8_t *b, uint32_t id)
{
    memset(b, 0, 64);
    memcpy(b, "DXBC", 4);
    w32(b + 32, id);
}

static int g_tr_calls, g_tr_fail_id, g_tr_bad_layout_id;

/* Stands in for the DXVK translator: a module with the right layout for the id (HLSL-style bindings). */
static int fake_translate(const uint8_t *dxbc, size_t len, uint32_t id, uint8_t **spv, size_t *spv_len, char *err,
                          size_t errlen, void *user)
{
    (void)user;
    g_tr_calls++;
    if (len < 36 || memcmp(dxbc, "DXBC", 4) != 0 || (uint32_t)(dxbc[32] | dxbc[33] << 8) != id) {
        snprintf(err, errlen, "unexpected input");
        return 1;
    }
    if ((int)id == g_tr_fail_id) {
        snprintf(err, errlen, "made-up failure");
        return 1;
    }
    const LsfgSpvLayout *l = lsfg_shader_layout_for_id(id);
    SbModule *m = malloc(sizeof *m);
    size_t n = sb_build_layout(m, l->uniform_buffers, l->samplers,
                               l->sampled_images + ((int)id == g_tr_bad_layout_id), l->storage_images);
    *spv = malloc(n * 4);
    memcpy(*spv, m->w, n * 4);
    *spv_len = n * 4;
    free(m);
    return 0;
}

/* A DLL with every LSFG resource as DXBC (both variants), id 101 noise, and with_fp16: embedded SPIR-V at +49 for
 * the LSFG 3.1 IDs. */
static Pe build_lsfg_dll(int with_fp16, int perf_only)
{
    static uint8_t dx[64][64];
    static SbModule spv[32];
    PeRes res[128];
    size_t n = 0;
    uint8_t noise[16] = "not a shader";
    res[n++] = (PeRes){ 101, noise, 16 };
    for (uint32_t id = 255; id <= 302; id++) {
        if (perf_only && id >= 257 && id <= 279)
            continue;
        fake_dxbc(dx[id - 255], id);
        res[n++] = (PeRes){ id, dx[id - 255], 64 };
    }
    for (uint32_t id = 255; with_fp16 && id <= 279; id++) {
        const LsfgSpvLayout *l = lsfg_shader_layout_for_id(id);
        size_t w = sb_build_layout(&spv[id - 255], l->uniform_buffers, l->samplers, l->sampled_images,
                                   l->storage_images);
        res[n++] = (PeRes){ id + LSFG_SPV_FP16_OFFSET, (const uint8_t *)spv[id - 255].w, (uint32_t)(w * 4) };
    }
    return build_pe(res, n, 0);
}

static void test_cache(void)
{
    char base[4096], dir[4096], err[512];
    snprintf(base, sizeof base, "%s/cache", g_tmp);
    LsfgCacheInfo info;

    /* DXBC only, no translator: extracted, nothing ready yet. */
    Pe dll = build_lsfg_dll(0, 0);
    CHECK_EQ(lsfg_cache_import(dll.b, dll.len, base, NULL, NULL, NULL, &info, dir, sizeof dir, err, sizeof err), 0);
    CHECK_EQ(info.rcdata, 49);
    CHECK_EQ(info.needed[0], 25); CHECK_EQ(info.needed[1], 25);
    CHECK_EQ(info.dxbc[0], 25); CHECK_EQ(info.dxbc[1], 25);
    CHECK_EQ(info.spv[0], 0); CHECK_EQ(info.fp16[0], 0);
    CHECK(!lsfg_cache_variant_ready(&info, 0, NULL, NULL, NULL));
    CHECK(file_exists(dir, "255.dxbc") && file_exists(dir, "302.dxbc") && file_exists(dir, "manifest.txt"));
    CHECK(!file_exists(dir, "101.dxbc"));
    uint8_t digest[32];
    lsfg_sha256(dll.b, dll.len, digest);
    char key[17];
    for (int i = 0; i < 8; i++)
        snprintf(key + 2 * i, 3, "%02x", digest[i]);
    CHECK(strcmp(info.key, key) == 0);
    char cur[4096];
    CHECK_EQ(lsfg_cache_current(base, cur, sizeof cur), 0);
    CHECK(strcmp(cur, dir) == 0);
    uint8_t *spv;
    size_t len;
    CHECK_EQ(lsfg_cache_load_shader(dir, "alpha[0]", LSFG_FORM_SPV | LSFG_FORM_FP32, 0, &spv, &len, NULL, err,
                                    sizeof err), LSFG_CACHE_ENOSHADER);

    /* The translator fills the cache (one shader fails: reported, the others kept). */
    g_tr_calls = 0;
    g_tr_fail_id = 262;
    CHECK_EQ(lsfg_cache_translate(dir, "fake", fake_translate, NULL, &info, err, sizeof err), LSFG_CACHE_ETRANSLATE);
    CHECK(strstr(err, "262") != NULL);
    CHECK_EQ(g_tr_calls, 48);
    CHECK_EQ(info.spv[0], 24); CHECK_EQ(info.spv[1], 25);
    int h16, h32, hspv;
    CHECK(lsfg_cache_variant_ready(&info, 1, &h16, &h32, &hspv) && hspv && !h16 && !h32);
    CHECK(!lsfg_cache_variant_ready(&info, 0, NULL, NULL, NULL));
    CHECK(strcmp(info.translator, "fake") == 0);
    /* Only the missing one runs again. */
    g_tr_calls = 0;
    g_tr_fail_id = 0;
    CHECK_EQ(lsfg_cache_translate(dir, "fake", fake_translate, NULL, &info, err, sizeof err), 0);
    CHECK_EQ(g_tr_calls, 1);
    CHECK_EQ(info.spv[0], 25);

    /* Loading renumbers to framegen's order and checks the layout. */
    unsigned form = 0;
    CHECK_EQ(lsfg_cache_load_shader(dir, "beta[4]", LSFG_FORM_SPV, 0, &spv, &len, &form, err, sizeof err), 0);
    CHECK_EQ(form, LSFG_FORM_SPV);
    LsfgSpvLayout found;
    CHECK_EQ(lsfg_spv_inspect((const uint32_t *)(void *)spv, len / 4, &found), 0);
    CHECK(found.uniform_buffers == 1 && found.samplers == 1 && found.sampled_images == 2 && found.storage_images == 6);
    /* The first storage image (u0, declared last by spv_build) is binding 1 + 1 + 2 = 4. */
    const uint32_t *w = (const uint32_t *)(void *)spv;
    uint32_t max_binding = 0;
    for (size_t at = 5; at < len / 4; at += w[at] >> 16)
        if ((w[at] & 0xffff) == 71 && w[at + 2] == 33 && w[at + 3] > max_binding)
            max_binding = w[at + 3];
    CHECK_EQ(max_binding, 9);
    free(spv);
    CHECK_EQ(lsfg_cache_load_shader(dir, "nope", LSFG_FORM_SPV, 0, &spv, &len, NULL, err, sizeof err),
             LSFG_CACHE_EINVAL);

    /* A second import of the same DLL reuses the directory: nothing re-extracted, nothing re-translated. */
    g_tr_calls = 0;
    CHECK_EQ(lsfg_cache_import(dll.b, dll.len, base, "fake", fake_translate, NULL, &info, dir, sizeof dir, err,
                               sizeof err), 0);
    CHECK_EQ(g_tr_calls, 0);
    CHECK_EQ(info.spv[0], 25);
    free(dll.b);

    /* A DLL with embedded FP16 SPIR-V: preferred when asked for and allowed. */
    dll = build_lsfg_dll(1, 0);
    char dir2[4096];
    CHECK_EQ(lsfg_cache_import(dll.b, dll.len, base, NULL, NULL, NULL, &info, dir2, sizeof dir2, err, sizeof err), 0);
    CHECK(strcmp(dir, dir2) != 0);
    CHECK_EQ(info.fp16[0], 25); CHECK_EQ(info.fp16[1], 2);   /* 255 and 256 are shared */
    CHECK(lsfg_cache_variant_ready(&info, 0, &h16, NULL, NULL) && h16);
    CHECK_EQ(lsfg_cache_load_shader(dir2, "generate", LSFG_FORM_FP16 | LSFG_FORM_SPV, 1, &spv, &len, &form, err,
                                    sizeof err), 0);
    CHECK_EQ(form, LSFG_FORM_FP16);
    free(spv);
    CHECK_EQ(lsfg_cache_current(base, cur, sizeof cur), 0);
    CHECK(strcmp(cur, dir2) == 0);
    free(dll.b);

    /* A translator returning the wrong layout is refused. */
    dll = build_lsfg_dll(0, 0);
    dll.b[dll.len - 1] ^= 1;   /* different bytes in the last blob's padding: another key */
    g_tr_bad_layout_id = 256;
    CHECK_EQ(lsfg_cache_import(dll.b, dll.len, base, "fake", fake_translate, NULL, &info, dir2, sizeof dir2, err,
                               sizeof err), LSFG_CACHE_ETRANSLATE);
    CHECK(strstr(err, "256") && strstr(err, "layout"));
    CHECK_EQ(info.spv[0], 24);
    g_tr_bad_layout_id = 0;
    free(dll.b);

    /* A cached shader damaged on disk: refused at load. */
    char p[4096];
    snprintf(p, sizeof p, "%s/267.spv", dir);
    FILE *f = fopen(p, "r+b");
    fputc(0, f);
    fclose(f);
    CHECK_EQ(lsfg_cache_load_shader(dir, "alpha[0]", LSFG_FORM_SPV, 0, &spv, &len, NULL, err, sizeof err),
             LSFG_CACHE_EBADSHADER);

    /* No LSFG 3.1 resource: unsupported, nothing left behind. */
    uint8_t noise[8] = "noise";
    PeRes only[] = { { 101, noise, 8 }, { 290, noise, 8 } };
    dll = build_pe(only, 2, 0);
    CHECK_EQ(lsfg_cache_import(dll.b, dll.len, base, NULL, NULL, NULL, NULL, dir2, sizeof dir2, err, sizeof err),
             LSFG_CACHE_EUNSUPPORTED);
    CHECK(strstr(err, "not supported") != NULL);
    struct stat st;
    CHECK(stat(dir2, &st) != 0);
    char partial[4200];
    snprintf(partial, sizeof partial, "%s.partial", dir2);
    CHECK(stat(partial, &st) != 0);
    free(dll.b);

    /* Not a PE: the DLL error comes through. */
    uint8_t junk[100] = { 0 };
    CHECK_EQ(lsfg_cache_import(junk, sizeof junk, base, NULL, NULL, NULL, NULL, NULL, 0, err, sizeof err),
             LSFG_CACHE_DLL_BASE + LSFG_DLL_ENOTPE);
    CHECK(strcmp(lsfg_cache_strerror(LSFG_CACHE_DLL_BASE + LSFG_DLL_ENOTPE), lsfg_dll_strerror(LSFG_DLL_ENOTPE)) == 0);

    /* Import from an fd. */
    dll = build_lsfg_dll(0, 1);
    snprintf(p, sizeof p, "%s/perf.dll", g_tmp);
    f = fopen(p, "wb");
    fwrite(dll.b, 1, dll.len, f);
    fclose(f);
    int fd = open(p, O_RDONLY);
    /* Only the performance set (255, 256, 280..302): supported, LSFG 3.1 incomplete. */
    CHECK_EQ(lsfg_cache_import_fd(fd, base, NULL, NULL, NULL, &info, NULL, 0, err, sizeof err), 0);
    CHECK_EQ(info.dxbc[0], 2);
    CHECK_EQ(info.dxbc[1], 25);
    close(fd);
    free(dll.b);

    /* Manifest garbage. */
    CHECK_EQ(lsfg_cache_read_manifest(g_tmp, &info), LSFG_CACHE_ENOMANIFEST);
}

int main(void)
{
    const char *t = getenv("TMPDIR");
    snprintf(g_tmp, sizeof g_tmp, "%s/lsfg_dll_test.%d", t && *t ? t : "/tmp", (int)getpid());
    mkdir(g_tmp, 0700);
    test_walk(0);
    test_walk(1);
    test_malformed();
    test_read_fd();
    test_sha256();
    test_cache();
    rm_rf(g_tmp);
    if (g_fail) {
        fprintf(stderr, "lsfg_dll: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("lsfg_dll: ok\n");
    return 0;
}
