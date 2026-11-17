/* gx_progcache.c -- the GX program binary cache file (gx_progcache.h), XBA-264. Plain C, no GL. */
#include "gx_progcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_shader.h"

static const char MAGIC[8] = {'X', 'R', 'G', 'X', 'P', 'B', '1', '\n'};

static uint64_t fnv1a(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
#define FNV_SEED 0xCBF29CE484222325ull

static uint64_t hash_str(uint64_t h, const char *s)
{
    s = s ? s : "";
    return fnv1a(fnv1a(h, s, strlen(s)), "\0", 1);
}

uint64_t gx_progcache_key(const char *vendor, const char *renderer, const char *version, int desktop)
{
    uint64_t h = FNV_SEED;
    uint32_t v[3] = {GX_PROGCACHE_VERSION, (uint32_t)sizeof(gx_shader_key), desktop ? 1u : 0u};
    h = fnv1a(h, v, sizeof v);
    h = hash_str(h, vendor);
    h = hash_str(h, renderer);
    h = hash_str(h, version);
    h = hash_str(h, getenv("GC_GX_DRIVER_KEY"));
    h = hash_str(h, gx_shader_vs_source());
    char *uber = gx_shader_fs_source(NULL);
    h = hash_str(h, uber);
    free(uber);
    return h;
}

uint64_t gx_progcache_src_hash(const gx_shader_key *k)
{
    char *s = gx_shader_fs_source(k);
    if (!s)
        return 0;
    uint64_t h = hash_str(FNV_SEED, s);
    free(s);
    return h ? h : 1;
}

void gx_progcache_init(gx_progcache *c, uint64_t key, const char *driver)
{
    memset(c, 0, sizeof *c);
    c->key = key;
    snprintf(c->driver, sizeof c->driver, "%s", driver ? driver : "");
}

void gx_progcache_free(gx_progcache *c)
{
    for (size_t i = 0; i < c->n; i++)
        free(c->e[i].data);
    free(c->e);
    c->e = NULL;
    c->n = c->cap = 0;
}

gx_progbin *gx_progcache_find(gx_progcache *c, const gx_shader_key *k)
{
    for (size_t i = 0; i < c->n; i++)
        if (!memcmp(&c->e[i].key, k, sizeof *k))
            return &c->e[i];
    return NULL;
}

void gx_progcache_drop(gx_progcache *c, size_t i)
{
    if (i >= c->n)
        return;
    free(c->e[i].data);
    c->e[i] = c->e[--c->n];
}

int gx_progcache_put(gx_progcache *c, const gx_shader_key *k, uint64_t src_hash, uint32_t format, const void *data,
                     uint32_t len)
{
    if (!len || len > GX_PROGCACHE_MAX_BINARY)
        return -1;
    uint8_t *copy = malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, data, len);
    gx_progbin *e = gx_progcache_find(c, k);
    if (!e) {
        if (c->n == c->cap) {
            size_t cap = c->cap ? c->cap * 2 : 64;
            gx_progbin *ne = realloc(c->e, cap * sizeof *ne);
            if (!ne) {
                free(copy);
                return -1;
            }
            c->e = ne;
            c->cap = cap;
        }
        e = &c->e[c->n++];
        e->key = *k;
    } else {
        free(e->data);
    }
    e->src_hash = src_hash;
    e->format = format;
    e->len = len;
    e->data = copy;
    return 0;
}

/* ---- file ---- */

typedef struct reader {
    const uint8_t *p;
    size_t n, at;
} reader;

static int take(reader *r, void *out, size_t n)
{
    if (r->n - r->at < n)
        return -1;
    if (out)
        memcpy(out, r->p + r->at, n);
    r->at += n;
    return 0;
}

int gx_progcache_load(const char *path, uint64_t key, const char *driver, gx_progcache *c, const char **why,
                      char *driver_was, size_t driver_was_len)
{
    gx_progcache_init(c, key, driver);
    if (driver_was && driver_was_len)
        driver_was[0] = 0;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) {
        *why = "none";
        return -1;
    }
    uint8_t *buf = NULL;
    size_t n = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0 && fseek(f, 0, SEEK_SET) == 0) {
            buf = malloc((size_t)sz);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
                n = (size_t)sz;
        }
    }
    fclose(f);
    if (!buf) {
        *why = n ? "out of memory" : "truncated";
        return -1;
    }
    reader r = {buf, n, 0};
    char magic[8];
    uint64_t fkey, sum;
    uint32_t dlen, count;
    *why = "truncated";
    if (n < 8 + 8 + 4 + 4 + 8 || take(&r, magic, 8))
        goto bad;
    if (memcmp(magic, MAGIC, 8)) {
        *why = "not a program cache";
        goto bad;
    }
    memcpy(&sum, buf + n - 8, 8);
    if (fnv1a(FNV_SEED, buf, n - 8) != sum) {
        *why = "checksum";
        goto bad;
    }
    r.n = n - 8;
    if (take(&r, &fkey, 8) || take(&r, &dlen, 4) || take(&r, &count, 4) || dlen > r.n - r.at)
        goto bad;
    if (driver_was && driver_was_len) {
        size_t m = dlen < driver_was_len - 1 ? dlen : driver_was_len - 1;
        memcpy(driver_was, buf + r.at, m);
        driver_was[m] = 0;
    }
    r.at += dlen;
    if (fkey != key) {
        *why = "another driver or shader generator";
        goto bad;
    }
    for (uint32_t i = 0; i < count; i++) {
        gx_shader_key k;
        uint64_t sh;
        uint32_t fmt, len;
        if (take(&r, &k, sizeof k) || take(&r, &sh, 8) || take(&r, &fmt, 4) || take(&r, &len, 4) ||
            len > r.n - r.at || len > GX_PROGCACHE_MAX_BINARY)
            goto bad;
        if (gx_progcache_put(c, &k, sh, fmt, buf + r.at, len)) {
            *why = "out of memory";
            goto bad;
        }
        r.at += len;
    }
    if (r.at != r.n)
        goto bad;
    free(buf);
    *why = NULL;
    return 0;
bad:
    free(buf);
    gx_progcache_free(c);
    return -1;
}

typedef struct writer {
    FILE *f;
    uint64_t sum;
    int err;
} writer;

static void put(writer *w, const void *p, size_t n)
{
    if (!n)
        return;
    w->sum = fnv1a(w->sum, p, n);
    if (fwrite(p, 1, n, w->f) != n)
        w->err = 1;
}

int gx_progcache_save(const char *path, const gx_progcache *c)
{
    char tmp[1200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return -1;
    writer w = {fopen(tmp, "wb"), FNV_SEED, 0};
    if (!w.f)
        return -1;
    uint32_t dlen = (uint32_t)strlen(c->driver), count = (uint32_t)c->n;
    put(&w, MAGIC, 8);
    put(&w, &c->key, 8);
    put(&w, &dlen, 4);
    put(&w, &count, 4);
    put(&w, c->driver, dlen);
    for (size_t i = 0; i < c->n; i++) {
        const gx_progbin *e = &c->e[i];
        put(&w, &e->key, sizeof e->key);
        put(&w, &e->src_hash, 8);
        put(&w, &e->format, 4);
        put(&w, &e->len, 4);
        put(&w, e->data, e->len);
    }
    uint64_t sum = w.sum;
    if (fwrite(&sum, 1, 8, w.f) != 8)
        w.err = 1;
    if (fclose(w.f) || w.err) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path)) {
        remove(tmp);
        return -1;
    }
    return 0;
}
