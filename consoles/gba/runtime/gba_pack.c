/* gba_pack.c -- see gba_pack.h (XBA-157 / XBA-158). */
#define _GNU_SOURCE
#include "gba_pack.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- SHA-256 -------------------------------------------------------------------------------------------------- */

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    size_t len;
    uint64_t total;
} sha256_ctx;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, k;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

static void sha256_init(sha256_ctx *c)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->total = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->total += n;
    while (n) {
        size_t k = 64 - c->len < n ? 64 - c->len : n;
        memcpy(c->buf + c->len, p, k);
        c->len += k, p += k, n -= k;
        if (c->len == 64) {
            sha256_block(c->h, c->buf);
            c->len = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    sha256_update(c, &pad, 1);
    while (c->len != 56)
        sha256_update(c, &zero, 1);
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void gba_pack_sha256(const void *data, size_t n, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_final(&c, out);
}

void gba_pack_hex(const uint8_t sum[32], char hex[65])
{
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", sum[i]);
}

int gba_pack_sha256_file(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536], sum[32];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0)
        sha256_update(&c, buf, r);
    int err = ferror(f);
    fclose(f);
    if (err)
        return -1;
    sha256_final(&c, sum);
    gba_pack_hex(sum, hex);
    return 0;
}

const char *gba_pack_host_abi(void)
{
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86";
#elif defined(__arm__)
    return "armeabi-v7a";
#else
    return "unknown";
#endif
}

/* ---- pack.json: the top-level fields of a flat object (nested values are skipped) ----------------------------- */

typedef struct {
    long long format, shell_abi, lib_size;
    char console[16], title_key[64], rom_sha256[72], abi[32], lib[256], lib_sha256[72], entry[64];
    unsigned have;
} pack_meta;

enum { H_FORMAT = 1, H_CONSOLE = 2, H_KEY = 4, H_ROM = 8, H_ABI = 16, H_SHELL = 32, H_LIB = 64, H_SIZE = 128,
       H_LIBSHA = 256, H_ENTRY = 512, H_ALL = 1023 };

static const char *ws(const char *p, const char *e)
{
    while (p < e && isspace((unsigned char)*p))
        p++;
    return p;
}

/* A JSON string at p into out (escapes: \" \\ \/ and \uXXXX as '?'). Returns past it, or NULL. */
static const char *jstring(const char *p, const char *e, char *out, size_t n)
{
    size_t k = 0;
    if (p >= e || *p != '"')
        return NULL;
    for (p++; p < e && *p != '"'; p++) {
        char ch = *p;
        if (ch == '\\') {
            if (++p >= e)
                return NULL;
            ch = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
            if (*p == 'u') {
                if (e - p < 5)
                    return NULL;
                p += 4;
                ch = '?';
            }
        }
        if (out && k + 1 < n)
            out[k++] = ch;
    }
    if (out && n)
        out[k] = 0;
    return p < e ? p + 1 : NULL;
}

/* Skips any JSON value. */
static const char *jskip(const char *p, const char *e)
{
    p = ws(p, e);
    if (p >= e)
        return NULL;
    if (*p == '"')
        return jstring(p, e, NULL, 0);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; p < e; p++) {
            if (*p == '"') {
                p = jstring(p, e, NULL, 0);
                if (!p)
                    return NULL;
                p--;
            } else if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0)
                    return p + 1;
            }
        }
        return NULL;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p))
        p++;
    return p;
}

static void copy_str(char *dst, size_t n, const char *src)
{
    size_t k = strlen(src);
    if (k >= n)
        k = n - 1;
    memcpy(dst, src, k);
    dst[k] = 0;
}

static int parse_meta(const char *s, size_t len, pack_meta *m)
{
    const char *p = s, *e = s + len;
    memset(m, 0, sizeof *m);
    p = ws(p, e);
    if (p >= e || *p++ != '{')
        return -1;
    for (;;) {
        char key[64], val[256];
        p = ws(p, e);
        if (p < e && *p == '}')
            return 0;
        if (!(p = jstring(p, e, key, sizeof key)))
            return -1;
        p = ws(p, e);
        if (p >= e || *p++ != ':')
            return -1;
        p = ws(p, e);
        const char *start = p;
        if (!(p = jskip(p, e)))
            return -1;
        int is_str = *start == '"';
        if (is_str)
            jstring(start, e, val, sizeof val);
        else {
            size_t n = (size_t)(p - start) < sizeof val - 1 ? (size_t)(p - start) : sizeof val - 1;
            memcpy(val, start, n);
            val[n] = 0;
        }
#define STR(name, field, bit) else if (!strcmp(key, name) && is_str) { copy_str(m->field, sizeof m->field, val); m->have |= bit; }
#define NUM(name, field, bit) else if (!strcmp(key, name) && !is_str && (isdigit((unsigned char)val[0]) || val[0] == '-')) { m->field = strtoll(val, NULL, 10); m->have |= bit; }
        if (0) {
        }
        NUM("format", format, H_FORMAT)
        STR("console", console, H_CONSOLE)
        STR("title_key", title_key, H_KEY)
        STR("rom_sha256", rom_sha256, H_ROM)
        STR("abi", abi, H_ABI)
        NUM("shell_abi", shell_abi, H_SHELL)
        STR("lib", lib, H_LIB)
        NUM("lib_size", lib_size, H_SIZE)
        STR("lib_sha256", lib_sha256, H_LIBSHA)
        STR("entry", entry, H_ENTRY)
#undef STR
#undef NUM
        p = ws(p, e);
        if (p < e && *p == ',') {
            p++;
            continue;
        }
        return p < e && *p == '}' ? 0 : -1;
    }
}

static char *read_text(const char *path, size_t max, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(max + 1);
    size_t r = buf ? fread(buf, 1, max + 1, f) : 0;
    fclose(f);
    if (!buf || r > max) {
        free(buf);
        return NULL;
    }
    buf[r] = 0;
    *n = r;
    return buf;
}

int gba_pack_check(const char *pack_dir, const char *key, const char *rom_sha256, long long shell_abi,
                   char *lib_path, size_t lib_path_n, char *why, size_t why_n)
{
    char path[1200], hex[65];
    pack_meta m;
    size_t n = 0;
    struct stat st;
    if (why && why_n)
        why[0] = 0;
    snprintf(path, sizeof path, "%s/pack.json", pack_dir ? pack_dir : ".");
    char *text = read_text(path, 1 << 20, &n);
    if (!text) {
        if (errno == ENOENT || access(path, F_OK) != 0) {
            snprintf(why, why_n, "no %s", path);
            return GBA_PACK_NONE;
        }
        snprintf(why, why_n, "pack.json cannot be read");
        return GBA_PACK_STALE;
    }
    int bad = parse_meta(text, n, &m);
    free(text);
#define STALE(...) do { snprintf(why, why_n, __VA_ARGS__); return GBA_PACK_STALE; } while (0)
    if (bad)
        STALE("pack.json is not JSON");
    if ((m.have & H_ALL) != H_ALL)
        STALE("pack.json lacks a field (format, console, title_key, rom_sha256, abi, shell_abi, lib, lib_size, "
              "lib_sha256, entry)");
    if (m.format != 1)
        STALE("format %lld", m.format);
    if (strcmp(m.console, "gba"))
        STALE("console %s", m.console);
    if (!key || strcmp(m.title_key, key))
        STALE("title_key %s, the game is %s", m.title_key, key ? key : "?");
    if (!rom_sha256 || strcmp(m.rom_sha256, rom_sha256))
        STALE("built from another ROM");
    if (strcmp(m.abi, gba_pack_host_abi()))
        STALE("abi %s, this host is %s", m.abi, gba_pack_host_abi());
    if (shell_abi && m.shell_abi != shell_abi)
        STALE("shell_abi %lld, this shell is %lld", m.shell_abi, shell_abi);
    if (strcmp(m.entry, GBA_RECOMP_ENTRY_SYMBOL))
        STALE("entry %s", m.entry);
    if (!m.lib[0] || strchr(m.lib, '/') || !strcmp(m.lib, ".") || !strcmp(m.lib, ".."))
        STALE("lib %s is not a file name", m.lib);
    snprintf(lib_path, lib_path_n, "%s/%s", pack_dir, m.lib);
    if (stat(lib_path, &st) != 0 || !S_ISREG(st.st_mode))
        STALE("%s missing", m.lib);
    if ((long long)st.st_size != m.lib_size)
        STALE("%s is %lld bytes, pack.json says %lld", m.lib, (long long)st.st_size, m.lib_size);
    if (gba_pack_sha256_file(lib_path, hex) != 0 || strcmp(hex, m.lib_sha256))
        STALE("%s sha256 mismatch", m.lib);
#undef STALE
    return GBA_PACK_LOADED;
}

/* ---- The library ---------------------------------------------------------------------------------------------- */

int gba_pack_open(const char *lib, const char *rom_sha256, gba_pack *out, char *why, size_t why_n)
{
    memset(out, 0, sizeof *out);
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        snprintf(why, why_n, "dlopen: %s", e ? e : "?");
        return -1;
    }
    gba_recomp_title_table_fn fn = (gba_recomp_title_table_fn)dlsym(h, GBA_RECOMP_ENTRY_SYMBOL);
    const gba_recomp_table *t = fn ? fn() : NULL;
    if (!fn)
        snprintf(why, why_n, "no %s in the pack", GBA_RECOMP_ENTRY_SYMBOL);
    else if (!t)
        snprintf(why, why_n, "%s returned no table", GBA_RECOMP_ENTRY_SYMBOL);
    else if (t->abi < GBA_RECOMP_ABI_MIN || t->abi > GBA_RECOMP_ABI)
        snprintf(why, why_n, "table abi %u, this runtime takes %u to %u", t->abi, (unsigned)GBA_RECOMP_ABI_MIN,
                 (unsigned)GBA_RECOMP_ABI);
    else if (t->count && !t->entries)
        snprintf(why, why_n, "table of %u entries without entries", t->count);
    else if (rom_sha256 && (!t->rom_sha256 || strcmp(t->rom_sha256, rom_sha256)))
        snprintf(why, why_n, "table built from another ROM");
    else {
        out->handle = h;
        out->table = t;
        return 0;
    }
    dlclose(h);
    return -1;
}

void gba_pack_close(gba_pack *p)
{
    if (p && p->handle)
        dlclose(p->handle);
    if (p)
        memset(p, 0, sizeof *p);
}

int gba_pack_load(gba_machine *m, const char *pack_lib, const char *key, const char *rom_sha256, gba_pack *out)
{
    char dir[1100], json[1200], lib[1300], why[512] = "";
    memset(out, 0, sizeof *out);
    gba_attach_recomp(m, NULL); /* whatever was attached before goes, loaded or not */
    if (!pack_lib || !*pack_lib)
        return GBA_PACK_NONE;
    snprintf(dir, sizeof dir, "%s", pack_lib);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof dir, ".");
    snprintf(json, sizeof json, "%s/pack.json", dir);
    snprintf(lib, sizeof lib, "%s", pack_lib);
    int rc = GBA_PACK_LOADED;
    if (access(json, F_OK) == 0) {   /* a published pack; a bare library (a lifter's own test) has no pack.json */
        char checked[1300];
        rc = gba_pack_check(dir, key, rom_sha256, GBA_PACK_SHELL_ABI, checked, sizeof checked, why, sizeof why);
        if (rc == GBA_PACK_LOADED && strcmp(checked, lib) != 0)
            snprintf(lib, sizeof lib, "%s", checked);   /* pack.json's lib, not the name handed over */
    } else if (access(lib, F_OK) != 0) {
        rc = GBA_PACK_STALE;
        snprintf(why, sizeof why, "no %.400s", lib);
    }
    if (rc == GBA_PACK_LOADED && gba_pack_open(lib, rom_sha256, out, why, sizeof why) != 0)
        rc = GBA_PACK_STALE;
    if (rc == GBA_PACK_LOADED && gba_attach_recomp(m, out->table) != 0) {
        snprintf(why, sizeof why, "the core refused the table (abi %u)", out->table->abi);
        gba_pack_close(out);
        rc = GBA_PACK_STALE;
    }
    if (rc == GBA_PACK_LOADED)
        printf("PACK_LOADED gba %s (%u functions)\n", key ? key : "?", out->table->count);
    else
        printf("PACK_STALE gba %s (%s), running interpreted\n", key ? key : "?", why);
    fflush(stdout);
    return rc == GBA_PACK_NONE ? GBA_PACK_STALE : rc;
}

/* ---- Discovered addresses ------------------------------------------------------------------------------------- */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void mkdirs_of(const char *path)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return;
    *slash = 0;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

static size_t uniq(uint32_t *v, size_t n)
{
    size_t k = 0;
    qsort(v, n, sizeof *v, cmp_u32);
    for (size_t i = 0; i < n; i++)
        if (k == 0 || v[k - 1] != v[i])
            v[k++] = v[i];
    return k;
}

int gba_pack_write_discovered(const char *path, const uint32_t *addrs, size_t n, size_t *added)
{
    size_t cap = GBA_PACK_DISCOVERED_MAX + n, have = 0, len = 0;
    if (added)
        *added = 0;
    if (!path || !*path)
        return -1;
    uint32_t *v = malloc((cap ? cap : 1) * sizeof *v);
    if (!v)
        return -1;
    char *text = read_text(path, 16u << 20, &len);
    if (text) {
        for (char *line = strtok(text, "\n"); line && have < GBA_PACK_DISCOVERED_MAX; line = strtok(NULL, "\n")) {
            char *hash = strchr(line, '#');
            if (hash)
                *hash = 0;
            char *end;
            while (isspace((unsigned char)*line))
                line++;
            if (!*line)
                continue;
            unsigned long long x = strtoull(line, &end, 16);
            if (end != line && x <= 0xFFFFFFFFull)
                v[have++] = (uint32_t)x;
        }
        free(text);
    }
    have = uniq(v, have);
    size_t before = have;
    memcpy(v + have, addrs, n * sizeof *v);
    size_t total = uniq(v, have + n);
    if (total > GBA_PACK_DISCOVERED_MAX)
        total = GBA_PACK_DISCOVERED_MAX;
    size_t fresh = total > before ? total - before : 0;
    if (added)
        *added = fresh;
    if (!fresh) {
        free(v);
        return 0;
    }
    char tmp[1200];
    mkdirs_of(path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    int ok = f != NULL;
    for (size_t i = 0; ok && i < total; i++)
        ok = fprintf(f, "0x%08X\n", (unsigned)v[i]) > 0;
    if (f) {
        ok &= fflush(f) == 0;
        ok &= fsync(fileno(f)) == 0;
        ok &= fclose(f) == 0;
    }
    free(v);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int gba_pack_save_missed(gba_machine *m, const char *path)
{
    if (!m || !path || !*path)
        return -1;
    size_t total = gba_recomp_missed(m, NULL, 0);
    if (!total)
        return 0;
    if (total > GBA_PACK_DISCOVERED_MAX)
        total = GBA_PACK_DISCOVERED_MAX;
    uint32_t *v = malloc(total * sizeof *v);
    if (!v)
        return -1;
    size_t got = gba_recomp_missed(m, v, total), added = 0;
    if (got > total)
        got = total;
    int rc = gba_pack_write_discovered(path, v, got, &added);
    free(v);
    if (rc != 0) {
        fprintf(stderr, "gba: cannot write the discovered addresses to %s: %s\n", path, strerror(errno));
        return -1;
    }
    printf("gba: %zu address(es) the pack did not cover, %zu new, in %s\n", got, added, path);
    fflush(stdout);
    return (int)added;
}
