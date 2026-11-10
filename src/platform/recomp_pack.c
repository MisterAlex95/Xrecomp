/* A title pack, the loader's side: title ID, pack.json, dlopen
 * (recomp_pack.h, docs/android/prepare.md « The loader »). */
#define _GNU_SOURCE
#include "recomp_pack.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PACK_JSON_MAX (1 << 20)

static void say(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    if (!buf || !n)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, n, fmt, ap);
    va_end(ap);
}

const char *recomp_pack_host_abi(void)
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

/* ---- Title ID ------------------------------------------------------------ */

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int recomp_pack_xbe_title_id_mem(const void *xbe, size_t len, char out[9])
{
    const unsigned char *b = xbe;
    if (!b || len < 0x11c || memcmp(b, "XBEH", 4) != 0)
        return -1;
    uint32_t base = le32(b + 0x104), cert = le32(b + 0x118);
    if (cert < base)
        return -1;
    uint64_t off = (uint64_t)(cert - base) + 0x08;
    if (off + 4 > len)
        return -1;
    snprintf(out, 9, "%08X", (unsigned)le32(b + off));
    return 0;
}

int recomp_pack_xbe_title_id(const char *path, char out[9])
{
    unsigned char hdr[0x11c], id[4];
    FILE *f = path ? fopen(path, "rb") : NULL;
    int rc = -1;

    if (!f)
        return -1;
    if (fread(hdr, 1, sizeof hdr, f) == sizeof hdr && memcmp(hdr, "XBEH", 4) == 0) {
        uint32_t base = le32(hdr + 0x104), cert = le32(hdr + 0x118);
        uint64_t off = (uint64_t)(cert - base) + 0x08;
        if (cert >= base && off <= 0x7fffffff && fseek(f, (long)off, SEEK_SET) == 0
            && fread(id, 1, 4, f) == 4) {
            snprintf(out, 9, "%08X", (unsigned)le32(id));
            rc = 0;
        }
    }
    fclose(f);
    return rc;
}

int recomp_pack_norm_title_id(const char *s, char out[9])
{
    const char *p;
    size_t n;
    if (!s)
        return -1;
    while (*s == ' ' || *s == '\t')
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    if (n == 0 || n > 8)
        return -1;
    unsigned v = 0;
    for (p = s; p < s + n; p++) {
        int c = *p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v << 4 | (unsigned)d;
    }
    snprintf(out, 9, "%08X", v);
    return 0;
}

/* ---- pack.json ------------------------------------------------------------ */

/* A tiny JSON reader: enough for a flat object of strings and integers, and
 * skips anything else (nested objects, arrays, floats, true/false/null). */
typedef struct {
    const char *p, *end;
    const char *err;
} jr;

static void ws(jr *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r'))
        r->p++;
}

static int hex4(const char *p, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        int c = p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        *v = *v << 4 | (unsigned)d;
    }
    return 0;
}

static void put_utf8(char *out, size_t n, size_t *k, unsigned cp)
{
    char b[4];
    size_t m;
    if (cp < 0x80) { b[0] = (char)cp; m = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xc0 | cp >> 6); b[1] = (char)(0x80 | (cp & 0x3f)); m = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xe0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        b[2] = (char)(0x80 | (cp & 0x3f)); m = 3;
    } else {
        b[0] = (char)(0xf0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 0x3f));
        b[2] = (char)(0x80 | (cp >> 6 & 0x3f)); b[3] = (char)(0x80 | (cp & 0x3f)); m = 4;
    }
    for (size_t i = 0; i < m; i++) {
        if (out && *k + 1 < n)
            out[*k] = b[i];
        (*k)++;
    }
}

/* A string at r->p (on its opening quote), decoded into out (NUL-terminated,
 * truncated to n). *full = 0 when it did not fit. */
static int jstring(jr *r, char *out, size_t n, int *full)
{
    size_t k = 0;
    if (r->p >= r->end || *r->p != '"') { r->err = "string expected"; return -1; }
    r->p++;
    while (r->p < r->end && *r->p != '"') {
        unsigned char c = (unsigned char)*r->p++;
        if (c < 0x20) { r->err = "control character in a string"; return -1; }
        if (c != '\\') {
            if (out && k + 1 < n)
                out[k] = (char)c;
            k++;
            continue;
        }
        if (r->p >= r->end) break;
        c = (unsigned char)*r->p++;
        unsigned cp;
        switch (c) {
        case '"': case '\\': case '/': cp = c; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (r->end - r->p < 4 || hex4(r->p, &cp)) { r->err = "bad \\u escape"; return -1; }
            r->p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u') {
                unsigned lo;
                if (!hex4(r->p + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    r->p += 6;
                }
            }
            break;
        default:
            r->err = "bad escape";
            return -1;
        }
        put_utf8(out, n, &k, cp);
    }
    if (r->p >= r->end) { r->err = "unterminated string"; return -1; }
    r->p++;
    if (out && n)
        out[k < n ? k : n - 1] = 0;
    if (full)
        *full = k < n;
    return 0;
}

/* A number; *is_int when it is a plain integer that fits a long long. */
static int jnumber(jr *r, long long *v, int *is_int)
{
    const char *s = r->p;
    char buf[64];
    if (r->p < r->end && *r->p == '-') r->p++;
    if (r->p >= r->end || *r->p < '0' || *r->p > '9') { r->err = "bad value"; return -1; }
    while (r->p < r->end && *r->p >= '0' && *r->p <= '9') r->p++;
    *is_int = 1;
    if (r->p < r->end && *r->p == '.') {
        *is_int = 0;
        r->p++;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9') r->p++;
    }
    if (r->p < r->end && (*r->p == 'e' || *r->p == 'E')) {
        *is_int = 0;
        r->p++;
        if (r->p < r->end && (*r->p == '+' || *r->p == '-')) r->p++;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9') r->p++;
    }
    if (*is_int) {
        size_t n = (size_t)(r->p - s);
        if (n >= sizeof buf) { *is_int = 0; return 0; }
        memcpy(buf, s, n);
        buf[n] = 0;
        errno = 0;
        *v = strtoll(buf, NULL, 10);
        if (errno == ERANGE) *is_int = 0;
    }
    return 0;
}

static int jskip(jr *r, int depth);

static int jskip_container(jr *r, int depth, char close)
{
    r->p++;
    ws(r);
    if (r->p < r->end && *r->p == close) { r->p++; return 0; }
    for (;;) {
        ws(r);
        if (close == '}') {
            if (jstring(r, NULL, 0, NULL)) return -1;
            ws(r);
            if (r->p >= r->end || *r->p != ':') { r->err = "':' expected"; return -1; }
            r->p++;
        }
        if (jskip(r, depth + 1)) return -1;
        ws(r);
        if (r->p < r->end && *r->p == ',') { r->p++; continue; }
        if (r->p < r->end && *r->p == close) { r->p++; return 0; }
        r->err = "',' or end expected";
        return -1;
    }
}

static int jskip(jr *r, int depth)
{
    long long v;
    int is_int;
    ws(r);
    if (depth > 32) { r->err = "nested too deep"; return -1; }
    if (r->p >= r->end) { r->err = "value expected"; return -1; }
    switch (*r->p) {
    case '"': return jstring(r, NULL, 0, NULL);
    case '{': return jskip_container(r, depth, '}');
    case '[': return jskip_container(r, depth, ']');
    case 't': if (r->end - r->p >= 4 && !memcmp(r->p, "true", 4)) { r->p += 4; return 0; } break;
    case 'f': if (r->end - r->p >= 5 && !memcmp(r->p, "false", 5)) { r->p += 5; return 0; } break;
    case 'n': if (r->end - r->p >= 4 && !memcmp(r->p, "null", 4)) { r->p += 4; return 0; } break;
    default: return jnumber(r, &v, &is_int);
    }
    r->err = "bad value";
    return -1;
}

typedef struct { const char *key; unsigned bit; size_t off; size_t size; } field;

#define STR_FIELD(k, bit, member) { k, bit, offsetof(recomp_pack_meta, member), sizeof(((recomp_pack_meta *)0)->member) }
#define INT_FIELD(k, bit, member) { k, bit, offsetof(recomp_pack_meta, member), 0 }

static const field FIELDS[] = {
    INT_FIELD("format", RECOMP_PACK_HAVE_FORMAT, format),
    STR_FIELD("title_id", RECOMP_PACK_HAVE_TITLE_ID, title_id),
    STR_FIELD("title", 0, title),
    STR_FIELD("abi", RECOMP_PACK_HAVE_ABI, abi),
    INT_FIELD("shell_abi", RECOMP_PACK_HAVE_SHELL_ABI, shell_abi),
    STR_FIELD("lib", RECOMP_PACK_HAVE_LIB, lib),
    INT_FIELD("lib_size", RECOMP_PACK_HAVE_LIB_SIZE, lib_size),
    STR_FIELD("lib_sha256", 0, lib_sha256),
    STR_FIELD("entry", 0, entry),
};

int recomp_pack_parse(const char *json, size_t len, recomp_pack_meta *m, char *err, size_t errn)
{
    jr r = { json, json + len, NULL };
    char key[64];

    memset(m, 0, sizeof *m);
    if (!json) { say(err, errn, "no text"); return -1; }
    if (len >= 3 && !memcmp(json, "\xef\xbb\xbf", 3))
        r.p += 3;
    ws(&r);
    if (r.p >= r.end || *r.p != '{') { say(err, errn, "not a JSON object"); return -1; }
    r.p++;
    ws(&r);
    if (r.p < r.end && *r.p == '}') {
        r.p++;
        goto done;
    }
    for (;;) {
        int full = 1;
        ws(&r);
        if (jstring(&r, key, sizeof key, &full)) goto bad;
        ws(&r);
        if (r.p >= r.end || *r.p != ':') { r.err = "':' expected"; goto bad; }
        r.p++;
        ws(&r);
        const field *f = NULL;
        for (size_t i = 0; full && i < sizeof FIELDS / sizeof FIELDS[0]; i++)
            if (!strcmp(key, FIELDS[i].key)) f = &FIELDS[i];
        if (!f) {
            if (jskip(&r, 1)) goto bad;
        } else if (f->size) {
            int fits = 1;
            if (r.p >= r.end || *r.p != '"') { say(err, errn, "%s: string expected", f->key); return -1; }
            if (jstring(&r, (char *)m + f->off, f->size, &fits)) goto bad;
            if (!fits) { say(err, errn, "%s: too long", f->key); return -1; }
            m->have |= f->bit;
        } else {
            long long v = 0;
            int is_int = 0;
            if (r.p >= r.end || !((*r.p >= '0' && *r.p <= '9') || *r.p == '-')) {
                say(err, errn, "%s: integer expected", f->key);
                return -1;
            }
            if (jnumber(&r, &v, &is_int)) goto bad;
            if (!is_int) { say(err, errn, "%s: integer expected", f->key); return -1; }
            *(long long *)((char *)m + f->off) = v;
            m->have |= f->bit;
        }
        ws(&r);
        if (r.p < r.end && *r.p == ',') { r.p++; continue; }
        if (r.p < r.end && *r.p == '}') { r.p++; break; }
        r.err = "',' or '}' expected";
        goto bad;
    }
done:
    ws(&r);
    if (r.p != r.end) { say(err, errn, "text after the object"); return -1; }
    if (!m->entry[0])
        snprintf(m->entry, sizeof m->entry, "recomp_title_main");
    return 0;
bad:
    say(err, errn, "bad JSON at byte %ld: %s", (long)(r.p - json), r.err ? r.err : "?");
    return -1;
}

int recomp_pack_validate(const recomp_pack_meta *m, const char *tid, long long shell_abi,
                         const char *pack_dir, char *lib_path, size_t lib_path_n,
                         char *reason, size_t reason_n)
{
    static const char *const names[] = { "format", "title_id", "abi", "shell_abi", "lib", "lib_size" };
    char a[9], b[9];
    struct stat st;

    for (unsigned i = 0; i < 6; i++)
        if (!(m->have & (1u << i))) { say(reason, reason_n, "pack.json has no %s", names[i]); return -1; }
    if (m->format != RECOMP_PACK_FORMAT) {
        say(reason, reason_n, "format %lld, this shell reads %d", m->format, RECOMP_PACK_FORMAT);
        return -1;
    }
    if (recomp_pack_norm_title_id(m->title_id, a) || recomp_pack_norm_title_id(tid, b) || strcmp(a, b)) {
        say(reason, reason_n, "title_id %s, the game is %s", m->title_id, tid ? tid : "?");
        return -1;
    }
    if (strcmp(m->abi, recomp_pack_host_abi())) {
        say(reason, reason_n, "abi %s, this shell is %s", m->abi, recomp_pack_host_abi());
        return -1;
    }
    if (m->shell_abi != shell_abi) {
        say(reason, reason_n, "shell_abi %lld, this shell is %lld", m->shell_abi, shell_abi);
        return -1;
    }
    if (!m->lib[0] || strchr(m->lib, '/') || !strcmp(m->lib, ".") || !strcmp(m->lib, "..")) {
        say(reason, reason_n, "lib \"%s\" is not a file name", m->lib);
        return -1;
    }
    if ((size_t)snprintf(lib_path, lib_path_n, "%s/%s", pack_dir, m->lib) >= lib_path_n) {
        say(reason, reason_n, "path too long");
        return -1;
    }
    if (stat(lib_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        say(reason, reason_n, "%s missing", m->lib);
        return -1;
    }
    if (m->lib_size <= 0 || (long long)st.st_size != m->lib_size) {
        say(reason, reason_n, "%s is %lld bytes, pack.json says %lld", m->lib, (long long)st.st_size,
            m->lib_size);
        return -1;
    }
    return 0;
}

int recomp_pack_find(const char *files_dir, const char *tid, long long shell_abi,
                     recomp_pack_meta *m, char *lib_path, size_t lib_path_n,
                     char *reason, size_t reason_n)
{
    char dir[RECOMP_PACK_PATH_MAX], json[RECOMP_PACK_PATH_MAX + 16], err[160];
    char id[9];
    FILE *f;

    memset(m, 0, sizeof *m);
    if (!files_dir || !files_dir[0] || recomp_pack_norm_title_id(tid, id)) {
        say(reason, reason_n, "no files dir or title ID");
        return RECOMP_PACK_INVALID;
    }
    if ((size_t)snprintf(dir, sizeof dir, "%s/packs/%s", files_dir, id) >= sizeof dir) {
        say(reason, reason_n, "path too long");
        return RECOMP_PACK_INVALID;
    }
    snprintf(json, sizeof json, "%s/pack.json", dir);
    f = fopen(json, "rb");
    if (!f) {
        if (errno == ENOENT || errno == ENOTDIR)
            return RECOMP_PACK_MISSING;
        say(reason, reason_n, "pack.json: %s", strerror(errno));
        return RECOMP_PACK_INVALID;
    }
    char *text = malloc(PACK_JSON_MAX);
    size_t n = text ? fread(text, 1, PACK_JSON_MAX, f) : 0;
    int toolong = text && n == PACK_JSON_MAX;
    fclose(f);
    if (!text || toolong) {
        free(text);
        say(reason, reason_n, toolong ? "pack.json too large" : "out of memory");
        return RECOMP_PACK_INVALID;
    }
    int rc = recomp_pack_parse(text, n, m, err, sizeof err);
    free(text);
    if (rc) {
        say(reason, reason_n, "pack.json: %s", err);
        return RECOMP_PACK_INVALID;
    }
    if (recomp_pack_validate(m, id, shell_abi, dir, lib_path, lib_path_n, reason, reason_n))
        return RECOMP_PACK_INVALID;
    return RECOMP_PACK_OK;
}

/* ---- dlopen ---------------------------------------------------------------- */

/* dlsym on a dlopen handle also searches the library's dependencies; a symbol
 * the pack does not define would come back as libmain.so's own stand-in (whose
 * forwarder would then call itself). Keep only what lives in the pack. */
static void *own_sym(void *handle, const char *name, const void *pack_base)
{
    Dl_info info;
    void *p = dlsym(handle, name);
    if (!p || !dladdr(p, &info) || info.dli_fbase != pack_base)
        return NULL;
    return p;
}

int recomp_pack_open(const char *lib_path, const char *entry, recomp_pack_syms *out,
                     char *err, size_t errn)
{
    Dl_info info;
    memset(out, 0, sizeof *out);
    if (!entry || !entry[0])
        entry = "recomp_title_main";
    dlerror();
    /* RTLD_LOCAL: the pack resolves its own references through DT_NEEDED
     * (libmain.so, libc, libm, libdl, liblog) and binds to itself
     * (-Bsymbolic); nothing loaded later needs to see its symbols. RTLD_NOW:
     * a missing symbol fails here, not in the middle of the game. */
    void *h = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        say(err, errn, "%s", e ? e : "dlopen failed");
        return RECOMP_PACK_DLOPEN;
    }
    void *main_fn = dlsym(h, entry);
    if (!main_fn || !dladdr(main_fn, &info)) {
        say(err, errn, "no %s in %s", entry, lib_path);
        dlclose(h);
        return RECOMP_PACK_DLOPEN;
    }
    /* The pack's load base, from its entry. An entry found in the shell
     * itself (the pack does not define it, a title is linked in) is no
     * pack. */
    {
        Dl_info self;
        if (!dladdr((void *)&recomp_pack_open, &self) || self.dli_fbase == info.dli_fbase) {
            say(err, errn, "%s resolves to the shell itself, not to %s", entry, lib_path);
            dlclose(h);
            return RECOMP_PACK_DLOPEN;
        }
    }
    out->handle = h;
    out->title_main = main_fn;
    out->lookup = own_sym(h, "recomp_lookup", info.dli_fbase);
    out->lookup_manual = own_sym(h, "recomp_lookup_manual", info.dli_fbase);
    out->fe_menu_get_pb_state = own_sym(h, "fe_menu_get_pb_state", info.dli_fbase);
    out->fe_cursor = own_sym(h, "g_fe_cursor", info.dli_fbase);
    return RECOMP_PACK_OK;
}
