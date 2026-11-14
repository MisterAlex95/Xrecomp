/* ps1_pack.c -- see ps1_pack.h (XBA-168). */
#define _GNU_SOURCE
#include "ps1_pack.h"

#include "ps1_devices.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

void ps1_pack_sha256(const void *data, size_t n, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_final(&c, out);
}

void ps1_pack_hex(const uint8_t sum[32], char hex[65])
{
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", sum[i]);
}

int ps1_pack_sha256_file(const char *path, char hex[65])
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
    ps1_pack_hex(sum, hex);
    return 0;
}

const char *ps1_pack_host_abi(void)
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
    char console[16], title_key[64], source_sha256[72], abi[32], lib[256], lib_sha256[72], entry[64];
    unsigned have;
} pack_meta;

enum { H_FORMAT = 1, H_CONSOLE = 2, H_KEY = 4, H_SRC = 8, H_ABI = 16, H_SHELL = 32, H_LIB = 64, H_SIZE = 128,
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
        STR("source_sha256", source_sha256, H_SRC)
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

int ps1_pack_check(const char *pack_dir, const char *key, const char *source_sha256, long long shell_abi,
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
            return PS1_PACK_NONE;
        }
        snprintf(why, why_n, "pack.json cannot be read");
        return PS1_PACK_STALE;
    }
    int bad = parse_meta(text, n, &m);
    free(text);
#define STALE(...) do { snprintf(why, why_n, __VA_ARGS__); return PS1_PACK_STALE; } while (0)
    if (bad)
        STALE("pack.json is not JSON");
    if ((m.have & H_ALL) != H_ALL)
        STALE("pack.json lacks a field (format, console, title_key, source_sha256, abi, shell_abi, lib, lib_size, "
              "lib_sha256, entry)");
    if (m.format != 1)
        STALE("format %lld", m.format);
    if (strcmp(m.console, "ps1"))
        STALE("console %s", m.console);
    if (!key || strcmp(m.title_key, key))
        STALE("title_key %s, the game is %s", m.title_key, key ? key : "?");
    if (!source_sha256 || strcmp(m.source_sha256, source_sha256))
        STALE("built from another boot EXE");
    if (strcmp(m.abi, ps1_pack_host_abi()))
        STALE("abi %s, this host is %s", m.abi, ps1_pack_host_abi());
    if (shell_abi && m.shell_abi != shell_abi)
        STALE("shell_abi %lld, this shell is %lld", m.shell_abi, shell_abi);
    if (strcmp(m.entry, PS1_RECOMP_ENTRY_SYMBOL))
        STALE("entry %s", m.entry);
    if (!m.lib[0] || strchr(m.lib, '/') || !strcmp(m.lib, ".") || !strcmp(m.lib, ".."))
        STALE("lib %s is not a file name", m.lib);
    snprintf(lib_path, lib_path_n, "%s/%s", pack_dir, m.lib);
    if (stat(lib_path, &st) != 0 || !S_ISREG(st.st_mode))
        STALE("%s missing", m.lib);
    if ((long long)st.st_size != m.lib_size)
        STALE("%s is %lld bytes, pack.json says %lld", m.lib, (long long)st.st_size, m.lib_size);
    if (ps1_pack_sha256_file(lib_path, hex) != 0 || strcmp(hex, m.lib_sha256))
        STALE("%s sha256 mismatch", m.lib);
#undef STALE
    return PS1_PACK_LOADED;
}

/* ---- The library ---------------------------------------------------------------------------------------------- */

int ps1_pack_open(const char *lib, const char *source_sha256, ps1_pack *out, char *why, size_t why_n)
{
    memset(out, 0, sizeof *out);
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        snprintf(why, why_n, "dlopen: %s", e ? e : "?");
        return -1;
    }
    ps1_recomp_title_table_fn fn = (ps1_recomp_title_table_fn)dlsym(h, PS1_RECOMP_ENTRY_SYMBOL);
    const ps1_recomp_table *t = fn ? fn() : NULL;
    if (!fn)
        snprintf(why, why_n, "no %s in the pack", PS1_RECOMP_ENTRY_SYMBOL);
    else if (!t)
        snprintf(why, why_n, "%s returned no table", PS1_RECOMP_ENTRY_SYMBOL);
    else if (t->abi != PS1_RECOMP_ABI)
        snprintf(why, why_n, "table abi %u, this runtime is %u", t->abi, (unsigned)PS1_RECOMP_ABI);
    else if (t->count && !t->entries)
        snprintf(why, why_n, "table of %u entries without entries", t->count);
    else if (source_sha256 && (!t->source_sha256 || strcmp(t->source_sha256, source_sha256)))
        snprintf(why, why_n, "table built from another boot EXE");
    else {
        out->handle = h;
        out->table = t;
        return 0;
    }
    dlclose(h);
    return -1;
}

void ps1_pack_close(ps1_pack *p)
{
    if (p && p->handle)
        dlclose(p->handle);
    if (p)
        memset(p, 0, sizeof *p);
}

int ps1_pack_load(ps1_machine *m, const char *pack_lib, const char *key, const char *source_sha256, ps1_pack *out)
{
    char dir[1100], json[1200], lib[1300], why[512] = "";
    memset(out, 0, sizeof *out);
    ps1_attach_recomp(m, NULL); /* whatever was attached before goes, loaded or not */
    if (!pack_lib || !*pack_lib)
        return PS1_PACK_NONE;
    snprintf(dir, sizeof dir, "%s", pack_lib);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof dir, ".");
    snprintf(json, sizeof json, "%s/pack.json", dir);
    snprintf(lib, sizeof lib, "%s", pack_lib);
    int rc = PS1_PACK_LOADED;
    if (!source_sha256 || !*source_sha256) {
        rc = PS1_PACK_STALE;   /* nothing to check it against: never run code that may be another game's */
        snprintf(why, sizeof why, "the game's boot EXE cannot be read");
    } else if (access(json, F_OK) == 0) {   /* a published pack; a bare library (a lifter's own test) has no pack.json */
        char checked[1300];
        rc = ps1_pack_check(dir, key, source_sha256, PS1_PACK_SHELL_ABI, checked, sizeof checked, why, sizeof why);
        if (rc == PS1_PACK_LOADED && strcmp(checked, lib) != 0)
            snprintf(lib, sizeof lib, "%s", checked);   /* pack.json's lib, not the name handed over */
    } else if (access(lib, F_OK) != 0) {
        rc = PS1_PACK_STALE;
        snprintf(why, sizeof why, "no %.400s", lib);
    }
    if (rc == PS1_PACK_LOADED && ps1_pack_open(lib, source_sha256, out, why, sizeof why) != 0)
        rc = PS1_PACK_STALE;
    if (rc == PS1_PACK_LOADED && ps1_attach_recomp(m, out->table) != 0) {
        snprintf(why, sizeof why, "the core refused the table (abi %u)", out->table->abi);
        ps1_pack_close(out);
        rc = PS1_PACK_STALE;
    }
    if (rc == PS1_PACK_LOADED)
        printf("PACK_LOADED ps1 %s (%u functions)\n", key ? key : "?", out->table->count);
    else
        printf("PACK_STALE ps1 %s (%s), running interpreted\n", key ? key : "?", why);
    fflush(stdout);
    return rc == PS1_PACK_NONE ? PS1_PACK_STALE : rc;
}

/* ---- The boot EXE of a disc (consoles/ps1/tools/ps1_tools/exe.py, the same rules) ----------------------------- */

typedef struct {
    ps1_disc *d;
    uint8_t raw[2352];
} iso_rd;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The 2048 user bytes of sector lba (mode 2 form 1: after the subheader; mode 1: after the header), or NULL. */
static const uint8_t *iso_sector(iso_rd *r, uint32_t lba)
{
    if (ps1_disc_read_sector(r->d, lba, r->raw) != 0)
        return NULL;
    return r->raw[15] == 2 ? r->raw + 24 : r->raw + 16;
}

/* length bytes from lba into a fresh buffer (max bytes at most), or NULL. */
static uint8_t *iso_read(iso_rd *r, uint32_t lba, uint32_t length, uint32_t max)
{
    if (length > max)
        return NULL;
    uint8_t *out = malloc(length ? length : 1);
    for (uint32_t done = 0; out && done < length; done += 2048, lba++) {
        const uint8_t *u = iso_sector(r, lba);
        if (!u) {
            free(out);
            return NULL;
        }
        memcpy(out + done, u, length - done < 2048 ? length - done : 2048);
    }
    return out;
}

/* The entry `want` (no ";1", upper case) of the directory at (lba, size): its extent and size. 0, or -1. */
static int iso_dir_find(iso_rd *r, uint32_t lba, uint32_t size, const char *want, int dir, uint32_t *ext,
                        uint32_t *len)
{
    uint8_t *data = iso_read(r, lba, size, 1u << 20);
    int rc = -1;
    for (uint32_t pos = 0; data && pos < size && rc;) {
        uint32_t n = data[pos];
        if (n == 0) {
            pos = (pos / 2048 + 1) * 2048; /* records never cross a sector */
            continue;
        }
        if (n < 33 || pos + n > size)
            break;
        uint32_t nlen = data[pos + 32];
        if (33 + nlen > n)
            break;
        char name[256];
        memcpy(name, data + pos + 33, nlen);
        name[nlen] = 0;
        char *semi = strchr(name, ';');
        if (semi)
            *semi = 0;
        for (char *c = name; *c; c++)
            *c = (char)toupper((unsigned char)*c);
        int is_dir = (data[pos + 25] & 2) != 0;
        int dot = nlen == 1 && (data[pos + 33] == 0 || data[pos + 33] == 1); /* "." and ".." */
        if (!dot && !strcmp(name, want) && is_dir == dir) {
            *ext = le32(data + pos + 2);
            *len = le32(data + pos + 10);
            rc = 0;
        }
        pos += n;
    }
    free(data);
    return rc;
}

/* A file by path ("\\DIR\\NAME.EXT;1", '\\' or '/', case-insensitive) from the root of pvd: extent and size. */
static int iso_find(iso_rd *r, const uint8_t *pvd, const char *path, uint32_t *ext, uint32_t *len)
{
    uint32_t lba = le32(pvd + 156 + 2), size = le32(pvd + 156 + 10);
    char parts[16][256];
    int np = 0;
    for (const char *p = path; *p;) {
        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;
        if (np == 16)
            return -1;
        size_t k = 0;
        while (*p && *p != '\\' && *p != '/') {
            if (k < 255)
                parts[np][k++] = (char)toupper((unsigned char)*p);
            p++;
        }
        parts[np][k] = 0;
        char *semi = strchr(parts[np], ';');
        if (semi)
            *semi = 0;
        np++;
    }
    for (int i = 0; i < np; i++)
        if (iso_dir_find(r, lba, size, parts[i], i < np - 1, &lba, &size) != 0)
            return -1;
    *ext = lba;
    *len = size;
    return np ? 0 : -1;
}

/* SYSTEM.CNF's first `BOOT \s* = \s* \S+` (case-insensitive, as re.search), "cdrom:" or "cdrom" dropped. */
static int cnf_boot(const char *cnf, char *out, size_t n)
{
    for (const char *p = cnf; *p; p++) {
        if (strncasecmp(p, "BOOT", 4))
            continue;
        const char *q = p + 4;
        while (*q && isspace((unsigned char)*q))
            q++;
        if (*q != '=')
            continue;
        q++;
        while (*q && isspace((unsigned char)*q))
            q++;
        const char *e = q;
        while (*e && !isspace((unsigned char)*e))
            e++;
        if (e == q)
            continue;
        if (e - q >= 5 && !strncasecmp(q, "cdrom", 5)) {
            q += 5;
            if (*q == ':')
                q++;
        }
        snprintf(out, n, "%.*s", (int)(e - q), q);
        return 0;
    }
    return -1;
}

int ps1_pack_disc_boot_sha256(const char *disc, char hex[65], char *boot, size_t boot_n, char *why, size_t why_n)
{
    uint8_t *exe = NULL;
    size_t len = 0;
    hex[0] = 0;
    if (ps1_pack_disc_boot_exe(disc, &exe, &len, boot, boot_n, why, why_n) != 0)
        return -1;
    uint8_t sum[32];
    ps1_pack_sha256(exe, len, sum);
    ps1_pack_hex(sum, hex);
    free(exe);
    return 0;
}

int ps1_pack_disc_boot_exe(const char *disc, uint8_t **exe_out, size_t *exe_n, char *boot, size_t boot_n, char *why,
                           size_t why_n)
{
    char err[256] = "", path[512] = "PSX.EXE";
    iso_rd r;
    uint32_t ext, len;
    uint8_t pvd[2048];
    const uint8_t *u;
    int rc = -1;
    *exe_out = NULL;
    *exe_n = 0;
    r.d = disc && *disc ? ps1_disc_open(disc, err, sizeof err) : NULL;
    if (!r.d) {
        snprintf(why, why_n, "the disc cannot be opened: %s", err[0] ? err : "no disc");
        return -1;
    }
    if (!(u = iso_sector(&r, 16)) || u[0] != 1 || memcmp(u + 1, "CD001", 5)) {
        snprintf(why, why_n, "no ISO 9660 volume on the disc");
        goto out;
    }
    memcpy(pvd, u, sizeof pvd);
    if (iso_find(&r, pvd, "SYSTEM.CNF", &ext, &len) == 0) {
        uint8_t *cnf = iso_read(&r, ext, len, 64u << 10);
        char *text = cnf ? malloc((size_t)len + 1) : NULL;
        if (text) {
            memcpy(text, cnf, len);
            text[len] = 0;
            for (uint32_t i = 0; i < len; i++)
                if (!text[i])
                    text[i] = ' '; /* latin-1 text: a NUL does not end it */
            if (cnf_boot(text, path, sizeof path) != 0)
                snprintf(path, sizeof path, "PSX.EXE");
        }
        free(text);
        free(cnf);
    }
    if (iso_find(&r, pvd, path, &ext, &len) != 0) {
        snprintf(why, why_n, "the boot EXE %s is not on the disc", path);
        goto out;
    }
    uint8_t *exe = iso_read(&r, ext, len, PS1_PACK_EXE_MAX);
    if (!exe) {
        snprintf(why, why_n, "the boot EXE %s cannot be read (%u bytes)", path, len);
        goto out;
    }
    *exe_out = exe;
    *exe_n = len;
    if (boot && boot_n)
        snprintf(boot, boot_n, "%s", path);
    rc = 0;
out:
    ps1_disc_close(r.d);
    return rc;
}

/* ---- Discovered code ------------------------------------------------------------------------------------------ */

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

int ps1_pack_save_missed(ps1_machine *m, const char *path)
{
    if (!m || !path || !*path)
        return -1;
    size_t total = ps1_recomp_missed(m, NULL, 0);
    if (!total)
        return 0;
    mkdirs_of(path);
    int added = ps1_recomp_write_missed(m, path);
    if (added < 0) {
        fprintf(stderr, "ps1: cannot write the discovered code to %s: %s\n", path, strerror(errno));
        return -1;
    }
    printf("ps1: code at %zu address(es) the pack did not cover, %d new record(s), in %s\n", total, added, path);
    fflush(stdout);
    return added;
}
