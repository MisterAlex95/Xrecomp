/* console_pack.c -- see console_pack.h. */
#define _GNU_SOURCE
#include "console_pack.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *console_pack_host_abi(void)
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

static int parse_meta(const char *s, size_t len, const char *source_field, pack_meta *m)
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
        STR(source_field, source_sha256, H_SRC)
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

char *console_pack_read_text(const char *path, size_t max, size_t *n)
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

int console_pack_check(const console_pack_desc *d, const char *pack_dir, const char *key, const char *source_sha256,
                       long long shell_abi, char *lib_path, size_t lib_path_n, char *why, size_t why_n)
{
    char path[1200], hex[65];
    pack_meta m;
    size_t n = 0;
    struct stat st;
    if (why && why_n)
        why[0] = 0;
    snprintf(path, sizeof path, "%s/pack.json", pack_dir ? pack_dir : ".");
    char *text = console_pack_read_text(path, 1 << 20, &n);
    if (!text) {
        if (errno == ENOENT || access(path, F_OK) != 0) {
            snprintf(why, why_n, "no %s", path);
            return CONSOLE_PACK_NONE;
        }
        snprintf(why, why_n, "pack.json cannot be read");
        return CONSOLE_PACK_STALE;
    }
    int bad = parse_meta(text, n, d->source_field, &m);
    free(text);
#define STALE(...) do { snprintf(why, why_n, __VA_ARGS__); return CONSOLE_PACK_STALE; } while (0)
    if (bad)
        STALE("pack.json is not JSON");
    if ((m.have & H_ALL) != H_ALL)
        STALE("pack.json lacks a field (format, console, title_key, %s, abi, shell_abi, lib, lib_size, "
              "lib_sha256, entry)", d->source_field);
    if (m.format != 1)
        STALE("format %lld", m.format);
    if (strcmp(m.console, d->console))
        STALE("console %s", m.console);
    if (!key || strcmp(m.title_key, key))
        STALE("title_key %s, the game is %s", m.title_key, key ? key : "?");
    if (!source_sha256 || strcmp(m.source_sha256, source_sha256))
        STALE("built from another %s", d->source_noun);
    if (strcmp(m.abi, console_pack_host_abi()))
        STALE("abi %s, this host is %s", m.abi, console_pack_host_abi());
    if (shell_abi && m.shell_abi != shell_abi)
        STALE("shell_abi %lld, this shell is %lld", m.shell_abi, shell_abi);
    if (strcmp(m.entry, d->entry_symbol))
        STALE("entry %s", m.entry);
    if (!m.lib[0] || strchr(m.lib, '/') || !strcmp(m.lib, ".") || !strcmp(m.lib, ".."))
        STALE("lib %s is not a file name", m.lib);
    snprintf(lib_path, lib_path_n, "%s/%s", pack_dir, m.lib);
    if (stat(lib_path, &st) != 0 || !S_ISREG(st.st_mode))
        STALE("%s missing", m.lib);
    if ((long long)st.st_size != m.lib_size)
        STALE("%s is %lld bytes, pack.json says %lld", m.lib, (long long)st.st_size, m.lib_size);
    if (recomp_sha256_file(lib_path, hex, d->sha) != 0 || strcmp(hex, m.lib_sha256))
        STALE("%s sha256 mismatch", m.lib);
#undef STALE
    return CONSOLE_PACK_LOADED;
}

/* ---- The library ---------------------------------------------------------------------------------------------- */

int console_pack_open(const console_pack_desc *d, const char *lib, const char *source_sha256, void **handle,
                      const void **table, char *why, size_t why_n)
{
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        snprintf(why, why_n, "dlopen: %s", e ? e : "?");
        return -1;
    }
    void *entry = dlsym(h, d->entry_symbol);
    console_pack_table t = {0};
    if (entry)
        d->read_table(entry, &t);
    if (!entry)
        snprintf(why, why_n, "no %s in the pack", d->entry_symbol);
    else if (!t.table)
        snprintf(why, why_n, "%s returned no table", d->entry_symbol);
    else if (d->abi_range && (t.abi < d->abi_min || t.abi > d->abi_max))
        snprintf(why, why_n, "table abi %u, this runtime takes %u to %u", t.abi, d->abi_min, d->abi_max);
    else if (!d->abi_range && t.abi != d->abi_max)
        snprintf(why, why_n, "table abi %u, this runtime is %u", t.abi, d->abi_max);
    else if (t.count && !t.entries)
        snprintf(why, why_n, "table of %u entries without entries", t.count);
    else if (source_sha256 && (!t.source_sha256 || strcmp(t.source_sha256, source_sha256)))
        snprintf(why, why_n, "table built from another %s", d->source_noun);
    else {
        *handle = h;
        *table = t.table;
        return 0;
    }
    dlclose(h);
    return -1;
}

void console_pack_close(void *handle)
{
    if (handle)
        dlclose(handle);
}
