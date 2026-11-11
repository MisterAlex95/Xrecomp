/* The LSFG shader cache (lsfg_cache.h). libc only. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "lsfg_cache.h"
#include "lsfg_dll.h"
#include "lsfg_spv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_CAP 4096
#define ID_MIN 255u
#define ID_MAX 302u
#define NIDS (ID_MAX - ID_MIN + 1)

/* ── SHA-256 (FIPS 180-4) ─────────────────────────────────────────────────────── */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void lsfg_sha256(const void *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *p = data;
    size_t n = len;
    for (; n >= 64; n -= 64, p += 64)
        sha256_block(h, p);
    uint8_t tail[128] = { 0 };
    memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t tlen = n < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[tlen - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_block(h, tail);
    if (tlen == 128)
        sha256_block(h, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

/* ── small helpers ─────────────────────────────────────────────────────────────── */

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int join(char *out, size_t cap, const char *a, const char *b)
{
    int n = snprintf(out, cap, "%s/%s", a, b);
    return n > 0 && (size_t)n < cap ? 0 : LSFG_CACHE_EINVAL;
}

static int mkdir_p(const char *path)
{
    char tmp[PATH_CAP];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof tmp)
        return LSFG_CACHE_EINVAL;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\0')
            continue;
        char c = tmp[i];
        tmp[i] = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return LSFG_CACHE_EIO;
        tmp[i] = c;
    }
    return 0;
}

/* Removes a directory and the plain files in it (the cache directories are flat). */
static void rm_flat(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    char p[PATH_CAP];
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (join(p, sizeof p, dir, e->d_name) == 0)
            unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static int write_file(const char *path, const void *data, size_t len)
{
    char tmp[PATH_CAP];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return LSFG_CACHE_EINVAL;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return LSFG_CACHE_EIO;
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, p + done, len - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            close(fd);
            unlink(tmp);
            return LSFG_CACHE_EIO;
        }
        done += (size_t)w;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return LSFG_CACHE_EIO;
    }
    return 0;
}

static int read_file(const char *path, uint8_t **buf, size_t *len)
{
    int rc = lsfg_dll_read_path(path, buf, len);
    if (rc == LSFG_DLL_ENOMEM)
        return LSFG_CACHE_ENOMEM;
    return rc ? LSFG_CACHE_EIO : 0;
}

static int exists(const char *dir, const char *name)
{
    char p[PATH_CAP];
    struct stat st;
    return join(p, sizeof p, dir, name) == 0 && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_needed_id(uint32_t id)
{
    return id >= ID_MIN && id <= ID_MAX && lsfg_shader_layout_for_id(id) != NULL;
}

/* ── manifest ─────────────────────────────────────────────────────────────────── */

static const char *const k_suffix[4] = { ".dxbc", ".fp16.spv", ".fp32.spv", ".spv" };
static const char *const k_listkey[4] = { "dxbc", "fp16", "fp32", "spv" };

static int write_manifest(const char *dir, const char *sha_hex, uint64_t size, uint32_t rcdata, const char *translator)
{
    char buf[8192];
    size_t n = 0;
#define PUT(...) do { int k_ = snprintf(buf + n, sizeof buf - n, __VA_ARGS__); \
                      if (k_ < 0 || (size_t)k_ >= sizeof buf - n) return LSFG_CACHE_EINVAL; n += (size_t)k_; } while (0)
    PUT("lsfg-shader-cache 1\nextractor %d\ndll_sha256 %s\ndll_size %llu\nrcdata %u\n", LSFG_CACHE_EXTRACTOR, sha_hex,
        (unsigned long long)size, rcdata);
    for (int f = 0; f < 4; f++) {
        PUT("%s", k_listkey[f]);
        for (uint32_t id = ID_MIN; id <= ID_MAX; id++) {
            char name[32];
            snprintf(name, sizeof name, "%u%s", id, k_suffix[f]);
            if (is_needed_id(id) && exists(dir, name))
                PUT(" %u", id);
        }
        PUT("\n");
    }
    if (translator && translator[0])
        PUT("translator %s\n", translator);
#undef PUT
    char path[PATH_CAP];
    if (join(path, sizeof path, dir, "manifest.txt"))
        return LSFG_CACHE_EINVAL;
    return write_file(path, buf, n);
}

int lsfg_cache_read_manifest(const char *dir, LsfgCacheInfo *info)
{
    if (!dir || !info)
        return LSFG_CACHE_EINVAL;
    memset(info, 0, sizeof *info);
    char path[PATH_CAP];
    if (join(path, sizeof path, dir, "manifest.txt"))
        return LSFG_CACHE_EINVAL;
    FILE *f = fopen(path, "r");
    if (!f)
        return LSFG_CACHE_ENOMANIFEST;
    char line[4096];
    int version = 0;
    uint8_t have[4][NIDS] = { { 0 } };
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *val = strchr(line, ' ');
        if (!val)
            continue;
        *val++ = '\0';
        if (strcmp(line, "lsfg-shader-cache") == 0) {
            version = atoi(val);
        } else if (strcmp(line, "extractor") == 0) {
            info->extractor = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(line, "dll_sha256") == 0) {
            snprintf(info->dll_sha256, sizeof info->dll_sha256, "%s", val);
        } else if (strcmp(line, "dll_size") == 0) {
            info->dll_size = strtoull(val, NULL, 10);
        } else if (strcmp(line, "rcdata") == 0) {
            info->rcdata = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(line, "translator") == 0) {
            snprintf(info->translator, sizeof info->translator, "%s", val);
        } else {
            for (int k = 0; k < 4; k++) {
                if (strcmp(line, k_listkey[k]) != 0)
                    continue;
                for (char *s = val, *end; *s; s = end) {
                    unsigned long id = strtoul(s, &end, 10);
                    if (end == s)
                        break;
                    if (is_needed_id((uint32_t)id))
                        have[k][id - ID_MIN] = 1;
                }
            }
        }
    }
    fclose(f);
    if (version != 1 || strlen(info->dll_sha256) != 64)
        return LSFG_CACHE_ENOMANIFEST;
    memcpy(info->key, info->dll_sha256, 16);
    info->key[16] = '\0';
    for (int perf = 0; perf < 2; perf++) {
        uint16_t ids[64];
        size_t n = lsfg_shader_ids(perf, ids, 64);
        info->needed[perf] = (uint32_t)n;
        for (size_t i = 0; i < n; i++) {
            uint32_t at = ids[i] - ID_MIN;
            info->dxbc[perf] += have[0][at];
            info->fp16[perf] += have[1][at];
            info->fp32[perf] += have[2][at];
            info->spv[perf] += have[3][at];
        }
    }
    return 0;
}

int lsfg_cache_variant_ready(const LsfgCacheInfo *info, int performance, int *has_fp16, int *has_fp32, int *has_spv)
{
    int v = performance ? 1 : 0;
    int a = info->needed[v] && info->fp16[v] == info->needed[v];
    int b = info->needed[v] && info->fp32[v] == info->needed[v];
    int c = info->needed[v] && info->spv[v] == info->needed[v];
    if (has_fp16) *has_fp16 = a;
    if (has_fp32) *has_fp32 = b;
    if (has_spv) *has_spv = c;
    return a || b || c;
}

/* ── translation ──────────────────────────────────────────────────────────────── */

/* Runs tr on one DXBC blob and checks the result; *out is malloc'd. */
static int translate_one(const uint8_t *dxbc, size_t len, uint32_t id, lsfg_translate_fn tr, void *user,
                         uint8_t **out, size_t *out_len, char *err, size_t errlen)
{
    uint8_t *spv = NULL;
    size_t n = 0;
    char why[256] = "";
    if (tr(dxbc, len, id, &spv, &n, why, sizeof why, user) != 0 || !spv) {
        free(spv);
        set_err(err, errlen, "shader %u: translation failed: %s", id, why[0] ? why : "?");
        return LSFG_CACHE_ETRANSLATE;
    }
    if (!lsfg_spv_is_spirv(spv, n)) {
        free(spv);
        set_err(err, errlen, "shader %u: the translator did not return SPIR-V", id);
        return LSFG_CACHE_ETRANSLATE;
    }
    LsfgSpvLayout found;
    int rc = lsfg_spv_renumber((uint32_t *)(void *)spv, n / 4, lsfg_shader_layout_for_id(id), &found);
    if (rc) {
        set_err(err, errlen, "shader %u: %s (found %u ubo, %u samplers, %u sampled, %u storage)", id,
                lsfg_spv_strerror(rc), found.uniform_buffers, found.samplers, found.sampled_images,
                found.storage_images);
        free(spv);
        return LSFG_CACHE_ETRANSLATE;
    }
    *out = spv;
    *out_len = n;
    return 0;
}

int lsfg_cache_translate(const char *dir, const char *translator_name, lsfg_translate_fn tr, void *user,
                         LsfgCacheInfo *info, char *err, size_t errlen)
{
    if (!dir || !tr)
        return LSFG_CACHE_EINVAL;
    LsfgCacheInfo cur;
    int rc = lsfg_cache_read_manifest(dir, &cur);
    if (rc)
        return rc;
    int result = 0, failed = 0;
    for (uint32_t id = ID_MIN; id <= ID_MAX; id++) {
        char name[32], path[PATH_CAP];
        snprintf(name, sizeof name, "%u.dxbc", id);
        if (!is_needed_id(id) || !exists(dir, name))
            continue;
        snprintf(name, sizeof name, "%u.spv", id);
        if (exists(dir, name))
            continue;
        snprintf(name, sizeof name, "%u.dxbc", id);
        uint8_t *dxbc = NULL, *spv = NULL;
        size_t dlen = 0, slen = 0;
        if (join(path, sizeof path, dir, name) || (rc = read_file(path, &dxbc, &dlen)) != 0) {
            set_err(err, errlen, "shader %u: cannot read %s", id, name);
            return rc ? rc : LSFG_CACHE_EINVAL;
        }
        char why[512];
        rc = translate_one(dxbc, dlen, id, tr, user, &spv, &slen, why, sizeof why);
        free(dxbc);
        if (rc) {
            if (!failed++)
                set_err(err, errlen, "%s", why);
            result = LSFG_CACHE_ETRANSLATE;
            continue;
        }
        snprintf(name, sizeof name, "%u.spv", id);
        rc = join(path, sizeof path, dir, name) ? LSFG_CACHE_EINVAL : write_file(path, spv, slen);
        free(spv);
        if (rc) {
            set_err(err, errlen, "cannot write %s", name);
            return rc;
        }
    }
    if (failed > 1 && err && errlen) {
        size_t l = strlen(err);
        snprintf(err + l, errlen - l, " (and %d more)", failed - 1);
    }
    rc = write_manifest(dir, cur.dll_sha256, cur.dll_size, cur.rcdata,
                        translator_name && translator_name[0] ? translator_name : cur.translator);
    if (rc)
        return rc;
    if (info && (rc = lsfg_cache_read_manifest(dir, info)) != 0)
        return rc;
    return result;
}

/* ── import ───────────────────────────────────────────────────────────────────── */

struct extract_ctx {
    const char *dir;
    uint32_t rcdata;
    int rc;
};

static int extract_cb(const LsfgDllResource *res, const uint8_t *data, void *user)
{
    struct extract_ctx *c = user;
    c->rcdata++;
    uint32_t base = 0;
    const char *suffix = NULL;
    if (is_needed_id(res->id) && lsfg_spv_is_dxbc(data, res->size)) {
        base = res->id;
        suffix = ".dxbc";
    } else if (res->id >= LSFG_SPV_FP16_OFFSET && is_needed_id(res->id - LSFG_SPV_FP16_OFFSET) &&
               lsfg_spv_is_spirv(data, res->size)) {
        base = res->id - LSFG_SPV_FP16_OFFSET;
        suffix = ".fp16.spv";
    } else if (res->id >= LSFG_SPV_FP32_OFFSET && is_needed_id(res->id - LSFG_SPV_FP32_OFFSET) &&
               lsfg_spv_is_spirv(data, res->size)) {
        base = res->id - LSFG_SPV_FP32_OFFSET;
        suffix = ".fp32.spv";
    }
    if (!suffix)
        return 0;
    char name[32], path[PATH_CAP];
    snprintf(name, sizeof name, "%u%s", base, suffix);
    if (join(path, sizeof path, c->dir, name) || (c->rc = write_file(path, data, res->size)) != 0) {
        if (!c->rc)
            c->rc = LSFG_CACHE_EINVAL;
        return 1;
    }
    return 0;
}

static void hex(const uint8_t *b, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = '\0';
}

static int write_current(const char *base, const char *key)
{
    char path[PATH_CAP], line[32];
    if (join(path, sizeof path, base, "current"))
        return LSFG_CACHE_EINVAL;
    int n = snprintf(line, sizeof line, "%s\n", key);
    return write_file(path, line, (size_t)n);
}

int lsfg_cache_import(const uint8_t *dll, size_t len, const char *base, const char *translator_name,
                      lsfg_translate_fn tr, void *user, LsfgCacheInfo *info, char *dir_out, size_t dir_cap,
                      char *err, size_t errlen)
{
    if (!dll || !base || !base[0])
        return LSFG_CACHE_EINVAL;
    if (err && errlen)
        err[0] = '\0';
    uint8_t digest[32];
    char sha[65], key[17], dir[PATH_CAP], partial[PATH_CAP];
    lsfg_sha256(dll, len, digest);
    hex(digest, 32, sha);
    memcpy(key, sha, 16);
    key[16] = '\0';
    int rc = mkdir_p(base);
    if (rc) {
        set_err(err, errlen, "cannot create %s", base);
        return rc;
    }
    if (join(dir, sizeof dir, base, key) || snprintf(partial, sizeof partial, "%s.partial", dir) >= (int)sizeof partial)
        return LSFG_CACHE_EINVAL;
    if (dir_out && dir_cap)
        snprintf(dir_out, dir_cap, "%s", dir);

    LsfgCacheInfo cur;
    int reuse = lsfg_cache_read_manifest(dir, &cur) == 0 && strcmp(cur.dll_sha256, sha) == 0 &&
                cur.extractor == LSFG_CACHE_EXTRACTOR;
    if (!reuse) {
        rm_flat(partial);
        if (mkdir(partial, 0700) != 0) {
            set_err(err, errlen, "cannot create %s", partial);
            return LSFG_CACHE_EIO;
        }
        struct extract_ctx c = { .dir = partial };
        rc = lsfg_dll_walk(dll, len, extract_cb, &c);
        if (rc < 0 && c.rc == 0) {
            rm_flat(partial);
            set_err(err, errlen, "%s", lsfg_dll_strerror(rc));
            return LSFG_CACHE_DLL_BASE + rc;
        }
        if (c.rc) {
            rm_flat(partial);
            set_err(err, errlen, "cannot write the shader cache in %s", partial);
            return c.rc;
        }
        if ((rc = write_manifest(partial, sha, (uint64_t)len, c.rcdata, NULL)) != 0 ||
            (rc = lsfg_cache_read_manifest(partial, &cur)) != 0) {
            rm_flat(partial);
            set_err(err, errlen, "cannot write the manifest");
            return rc;
        }
        /* The version check: one variant complete in one form (DXBC to translate, or embedded SPIR-V). */
        int complete = 0;
        for (int v = 0; v < 2; v++)
            complete |= cur.dxbc[v] == cur.needed[v] || cur.fp16[v] == cur.needed[v] || cur.fp32[v] == cur.needed[v];
        if (!complete) {
            rm_flat(partial);
            set_err(err, errlen, "this Lossless.dll version is not supported (LSFG 3.1: %u/%u shaders, performance: "
                    "%u/%u, among %u resources)", cur.dxbc[0], cur.needed[0], cur.dxbc[1], cur.needed[1], c.rcdata);
            return LSFG_CACHE_EUNSUPPORTED;
        }
        rm_flat(dir);
        if (rename(partial, dir) != 0) {
            rm_flat(partial);
            set_err(err, errlen, "cannot rename %s", partial);
            return LSFG_CACHE_EIO;
        }
    }
    int result = 0;
    if (tr)
        result = lsfg_cache_translate(dir, translator_name, tr, user, NULL, err, errlen);
    if ((rc = write_current(base, key)) != 0) {
        set_err(err, errlen, "cannot write %s/current", base);
        return rc;
    }
    if (info && (rc = lsfg_cache_read_manifest(dir, info)) != 0)
        return rc;
    return result;
}

int lsfg_cache_import_fd(int fd, const char *base, const char *translator_name, lsfg_translate_fn tr, void *user,
                         LsfgCacheInfo *info, char *dir_out, size_t dir_cap, char *err, size_t errlen)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    int rc = lsfg_dll_read_fd(fd, &buf, &len);
    if (rc) {
        set_err(err, errlen, "%s", lsfg_dll_strerror(rc));
        return LSFG_CACHE_DLL_BASE + rc;
    }
    rc = lsfg_cache_import(buf, len, base, translator_name, tr, user, info, dir_out, dir_cap, err, errlen);
    free(buf);
    return rc;
}

int lsfg_cache_import_path(const char *dll_path, const char *base, const char *translator_name, lsfg_translate_fn tr,
                           void *user, LsfgCacheInfo *info, char *dir_out, size_t dir_cap, char *err, size_t errlen)
{
    if (!dll_path)
        return LSFG_CACHE_EINVAL;
    int fd = open(dll_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_err(err, errlen, "cannot open the DLL");
        return LSFG_CACHE_DLL_BASE + LSFG_DLL_EIO;
    }
    int rc = lsfg_cache_import_fd(fd, base, translator_name, tr, user, info, dir_out, dir_cap, err, errlen);
    close(fd);
    return rc;
}

/* ── lookup ───────────────────────────────────────────────────────────────────── */

int lsfg_cache_default_base(char *out, size_t cap)
{
    const char *env = getenv("RECOMP_LSFG_CACHE");
    int n;
    if (env && env[0]) {
        n = snprintf(out, cap, "%s", env);
    } else {
        const char *home = getenv("HOME");
#ifdef __APPLE__
        if (!home || !home[0])
            return LSFG_CACHE_EINVAL;
        n = snprintf(out, cap, "%s/Library/Caches/xboxrecomp/lsfg", home);
#else
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && xdg[0])
            n = snprintf(out, cap, "%s/xboxrecomp/lsfg", xdg);
        else if (home && home[0])
            n = snprintf(out, cap, "%s/.cache/xboxrecomp/lsfg", home);
        else
            return LSFG_CACHE_EINVAL;
#endif
    }
    return n > 0 && (size_t)n < cap ? 0 : LSFG_CACHE_EINVAL;
}

int lsfg_cache_current(const char *base, char *dir_out, size_t cap)
{
    char path[PATH_CAP], key[64] = "";
    if (!base || join(path, sizeof path, base, "current"))
        return LSFG_CACHE_EINVAL;
    FILE *f = fopen(path, "r");
    if (!f)
        return LSFG_CACHE_ENOMANIFEST;
    if (!fgets(key, sizeof key, f))
        key[0] = '\0';
    fclose(f);
    key[strcspn(key, "\r\n")] = '\0';
    if (strlen(key) != 16 || strspn(key, "0123456789abcdef") != 16)
        return LSFG_CACHE_ENOMANIFEST;
    return join(dir_out, cap, base, key) ? LSFG_CACHE_EINVAL : 0;
}

int lsfg_cache_load_shader(const char *dir, const char *name, unsigned allowed, int prefer_fp16, uint8_t **spv,
                           size_t *len, unsigned *form_out, char *err, size_t errlen)
{
    if (!dir || !name || !spv || !len)
        return LSFG_CACHE_EINVAL;
    *spv = NULL;
    *len = 0;
    const LsfgShaderInfo *si = lsfg_shader_find(name);
    if (!si) {
        set_err(err, errlen, "unknown shader %s", name);
        return LSFG_CACHE_EINVAL;
    }
    static const unsigned order_fp16[3] = { LSFG_FORM_FP16, LSFG_FORM_SPV, LSFG_FORM_FP32 };
    static const unsigned order_full[3] = { LSFG_FORM_SPV, LSFG_FORM_FP32, LSFG_FORM_FP16 };
    const unsigned *order = prefer_fp16 ? order_fp16 : order_full;
    for (int i = 0; i < 3; i++) {
        unsigned form = order[i];
        if (!(allowed & form))
            continue;
        const char *suffix = form == LSFG_FORM_FP16 ? ".fp16.spv" : form == LSFG_FORM_FP32 ? ".fp32.spv" : ".spv";
        char file[32], path[PATH_CAP];
        snprintf(file, sizeof file, "%u%s", si->base_id, suffix);
        if (!exists(dir, file) || join(path, sizeof path, dir, file))
            continue;
        uint8_t *buf = NULL;
        size_t n = 0;
        int rc = read_file(path, &buf, &n);
        if (rc) {
            set_err(err, errlen, "%s (%s): cannot read %s", name, file, path);
            return rc;
        }
        if (!lsfg_spv_is_spirv(buf, n)) {
            free(buf);
            set_err(err, errlen, "%s (%s): not SPIR-V", name, file);
            return LSFG_CACHE_EBADSHADER;
        }
        LsfgSpvLayout found;
        rc = lsfg_spv_renumber((uint32_t *)(void *)buf, n / 4, &si->layout, &found);
        if (rc) {
            free(buf);
            set_err(err, errlen, "%s (%s): %s (found %u/%u/%u/%u, want %u/%u/%u/%u ubo/sampler/sampled/storage)",
                    name, file, lsfg_spv_strerror(rc), found.uniform_buffers, found.samplers, found.sampled_images,
                    found.storage_images, si->layout.uniform_buffers, si->layout.samplers, si->layout.sampled_images,
                    si->layout.storage_images);
            return LSFG_CACHE_EBADSHADER;
        }
        *spv = buf;
        *len = n;
        if (form_out)
            *form_out = form;
        return 0;
    }
    set_err(err, errlen, "%s (resource %u): not in the shader cache %s", name, si->base_id, dir);
    return LSFG_CACHE_ENOSHADER;
}

const char *lsfg_cache_strerror(int err)
{
    if (err <= LSFG_CACHE_DLL_BASE + LSFG_DLL_EIO && err >= LSFG_CACHE_DLL_BASE + LSFG_DLL_ETOOBIG)
        return lsfg_dll_strerror(err - LSFG_CACHE_DLL_BASE);
    switch (err) {
    case 0: return "ok";
    case LSFG_CACHE_EIO: return "cannot write the shader cache";
    case LSFG_CACHE_ENOMANIFEST: return "no shader cache (choose Lossless.dll again)";
    case LSFG_CACHE_ENOSHADER: return "a shader is missing from the cache";
    case LSFG_CACHE_EBADSHADER: return "a cached shader is invalid";
    case LSFG_CACHE_EUNSUPPORTED: return "this Lossless.dll version is not supported";
    case LSFG_CACHE_ENOMEM: return "out of memory";
    case LSFG_CACHE_EINVAL: return "invalid argument";
    case LSFG_CACHE_ETRANSLATE: return "shader translation failed";
    default: return "unknown error";
    }
}
