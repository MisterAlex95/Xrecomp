/* A title pack, the loader's side: title ID, pack.json, dlopen
 * (recomp_pack.h, docs/android/prepare.md « The loader »). */
#define _GNU_SOURCE
#include "recomp_pack.h"
#include "recomp_cfg.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* ---- Other consoles (XBA-147) ---------------------------------------------- */

static const char *const CONSOLE_NAMES[] = { "xbox", "gba", "ps1", "n64" };

int recomp_pack_console_id(const char *name)
{
    if (!name || !name[0])
        return RECOMP_CONSOLE_XBOX;
    for (int i = 0; i < (int)(sizeof CONSOLE_NAMES / sizeof CONSOLE_NAMES[0]); i++)
        if (!strcmp(name, CONSOLE_NAMES[i]))
            return i;
    return RECOMP_CONSOLE_UNKNOWN;
}

const char *recomp_pack_console_name(int console)
{
    if (console < 0 || console >= (int)(sizeof CONSOLE_NAMES / sizeof CONSOLE_NAMES[0]))
        return "?";
    return CONSOLE_NAMES[console];
}

int recomp_pack_dir_name(int console, const char *key, char *out, size_t out_n)
{
    size_t n;
    if (console == RECOMP_CONSOLE_XBOX) {
        char id[9];
        if (recomp_pack_norm_title_id(key, id) || out_n < 9)
            return -1;
        memcpy(out, id, 9);
        return 0;
    }
    if (console < 0 || !key || !(n = strlen(key)) || n > 40)
        return -1;
    for (const char *p = key; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'
              || *p == '-'))
            return -1;
    return (size_t)snprintf(out, out_n, "%s-%s", recomp_pack_console_name(console), key) < out_n ? 0 : -1;
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* A game code byte for a key: [A-Z0-9] as is, anything else '_'. */
static char code_char(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? (char)c : '_';
}

/* Game Boy Advance: 0x96 at 0xB2 and the header complement check at 0xBD. */
static int gba_key(FILE *f, const unsigned char *h, size_t hn, char *key, size_t key_n)
{
    unsigned char sum = 0, buf[65536];
    uint32_t crc = 0;
    size_t n;
    if (hn < 0xC0 || h[0xB2] != 0x96)
        return -1;
    for (int i = 0xA0; i <= 0xBC; i++)
        sum = (unsigned char)(sum - h[i]);
    if ((unsigned char)(sum - 0x19) != h[0xBD])
        return -1;
    if (fseek(f, 0, SEEK_SET) != 0)
        return -1;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        crc = crc32_update(crc, buf, n);
    snprintf(key, key_n, "%c%c%c%c-%08X", code_char(h[0xAC]), code_char(h[0xAD]), code_char(h[0xAE]),
             code_char(h[0xAF]), (unsigned)crc);
    return 0;
}

/* Nintendo 64: the first word says the byte order; CRC1 at 0x10, the game
 * code (category, ID, region) at 0x3B. */
static int n64_key(const unsigned char *h, size_t hn, char *key, size_t key_n)
{
    unsigned char b[0x40];
    int order;   /* byte index of the big-endian byte 0 in each word: z64 0, v64 swaps pairs, n64 reverses */
    if (hn < 0x40)
        return -1;
    if (h[0] == 0x80 && h[1] == 0x37 && h[2] == 0x12 && h[3] == 0x40) order = 0;
    else if (h[0] == 0x37 && h[1] == 0x80 && h[2] == 0x40 && h[3] == 0x12) order = 1;
    else if (h[0] == 0x40 && h[1] == 0x12 && h[2] == 0x37 && h[3] == 0x80) order = 2;
    else return -1;
    for (int i = 0; i < 0x40; i++) {
        int w = i & ~3, k = i & 3;
        b[i] = h[w + (order == 0 ? k : order == 1 ? (k ^ 1) : 3 - k)];
    }
    snprintf(key, key_n, "%c%c%c%c-%02X%02X%02X%02X", code_char(b[0x3B]), code_char(b[0x3C]), code_char(b[0x3D]),
             code_char(b[0x3E]), b[0x10], b[0x11], b[0x12], b[0x13]);
    return 0;
}

/* An ISO 9660 track: 2048-byte sectors, or raw 2352-byte ones (mode 1 data
 * at 16, mode 2 form 1 at 24). */
typedef struct { FILE *f; long size; long off; } iso_track;

static int iso_read(const iso_track *t, uint32_t lba, unsigned char out[2048])
{
    long pos = (long)lba * t->size + t->off;
    return fseek(t->f, pos, SEEK_SET) == 0 && fread(out, 1, 2048, t->f) == 2048 ? 0 : -1;
}

static int iso_open(FILE *f, iso_track *t, unsigned char pvd[2048])
{
    static const unsigned char sync[12] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    unsigned char raw[16];
    t->f = f;
    t->size = 2048;
    t->off = 0;
    if (iso_read(t, 16, pvd) == 0 && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5))
        return 0;
    if (fseek(f, 16L * 2352, SEEK_SET) != 0 || fread(raw, 1, 16, f) != 16 || memcmp(raw, sync, 12))
        return -1;
    t->size = 2352;
    t->off = raw[15] == 2 ? 24 : 16;
    if (iso_read(t, 16, pvd) == 0 && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5))
        return 0;
    return -1;
}

/* SYSTEM.CNF in the root directory, up to n - 1 bytes, NUL-terminated. */
static int iso_system_cnf(const iso_track *t, const unsigned char pvd[2048], char *out, size_t n)
{
    unsigned char sec[2048];
    uint32_t lba = le32(pvd + 156 + 2), len = le32(pvd + 156 + 10);
    for (uint32_t done = 0; done < len && done < 64 * 2048; done += 2048, lba++) {
        if (iso_read(t, lba, sec))
            return -1;
        for (unsigned o = 0; o + 33 < 2048 && sec[o];) {
            unsigned rl = sec[o], nl = sec[o + 32];
            if (rl < 34 || o + rl > 2048)
                break;
            if (nl >= 10 && o + 33 + nl <= 2048 && !strncasecmp((const char *)sec + o + 33, "SYSTEM.CNF", 10)
                && (nl == 10 || sec[o + 33 + 10] == ';')) {
                uint32_t flba = le32(sec + o + 2), flen = le32(sec + o + 10);
                if (iso_read(t, flba, sec))
                    return -1;
                if (flen > 2048) flen = 2048;
                if (flen > n - 1) flen = (uint32_t)(n - 1);
                memcpy(out, sec, flen);
                out[flen] = 0;
                return 0;
            }
            o += rl;
        }
    }
    return -1;
}

/* "BOOT = cdrom:\SLUS_005.94;1" -> "SLUS-00594"; -1 when the boot file is not
 * serial-shaped (four letters, '_' or '-', three digits, '.', two digits). */
static int ps1_serial(const char *cnf, char *key, size_t key_n)
{
    const char *p = cnf;
    while ((p = strstr(p, "BOOT")) != NULL) {
        const char *q = p + 4;
        p = q;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=')
            continue;
        q++;
        const char *end = q;
        while (*end && *end != '\r' && *end != '\n' && *end != ';') end++;
        const char *name = q;
        for (const char *s = q; s < end; s++)
            if (*s == '\\' || *s == ':' || *s == '/') name = s + 1;
        while (end > name && (end[-1] == ' ' || end[-1] == '\t')) end--;
        char s[16];
        if (end - name != 11)
            return -1;
        for (int i = 0; i < 11; i++)
            s[i] = name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 32) : name[i];
        for (int i = 0; i < 4; i++)
            if (s[i] < 'A' || s[i] > 'Z') return -1;
        if ((s[4] != '_' && s[4] != '-') || s[8] != '.')
            return -1;
        for (int i = 5; i < 11; i++)
            if (i != 8 && (s[i] < '0' || s[i] > '9')) return -1;
        snprintf(key, key_n, "%.4s-%.3s%.2s", s, s + 5, s + 9);
        return 0;
    }
    return -1;
}

static int ps1_key(FILE *f, char *key, size_t key_n)
{
    iso_track t;
    unsigned char pvd[2048];
    char cnf[2049];
    if (iso_open(f, &t, pvd) || memcmp(pvd + 8, "PLAYSTATION", 11))
        return -1;
    if (iso_system_cnf(&t, pvd, cnf, sizeof cnf) == 0 && ps1_serial(cnf, key, key_n) == 0)
        return 0;
    snprintf(key, key_n, "PSX-%08X", (unsigned)crc32_update(0, pvd, sizeof pvd));
    return 0;
}

/* A .cue's first FILE, resolved next to it, into out. */
static int cue_bin(const char *cue, char *out, size_t n)
{
    char line[1200];
    FILE *f = fopen(cue, "r");
    int rc = -1;
    if (!f)
        return -1;
    while (rc && fgets(line, sizeof line, f)) {
        char *p = line, *name, *end;
        while (*p == ' ' || *p == '\t') p++;
        if (strncasecmp(p, "FILE", 4) || (p[4] != ' ' && p[4] != '\t'))
            continue;
        p += 5;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '"') {
            name = ++p;
            end = strchr(p, '"');
        } else {
            name = p;
            end = p + strcspn(p, " \t\r\n");
        }
        if (!end || end == name)
            break;
        *end = 0;
        const char *slash = strrchr(cue, '/');
        if (name[0] == '/' || !slash)
            rc = (size_t)snprintf(out, n, "%s", name) < n ? 0 : -1;
        else
            rc = (size_t)snprintf(out, n, "%.*s/%s", (int)(slash - cue), cue, name) < n ? 0 : -1;
        break;
    }
    fclose(f);
    return rc;
}

int recomp_pack_rom_identify(const char *path, int *console, char *key, size_t key_n)
{
    unsigned char h[0xC0];
    size_t hn;
    size_t len = path ? strlen(path) : 0;
    FILE *f;
    int c = RECOMP_CONSOLE_UNKNOWN;

    if (!path || !key || key_n < 24)
        return -1;
    if (len > 4 && !strcasecmp(path + len - 4, ".cue")) {
        char bin[RECOMP_PACK_PATH_MAX];
        if (cue_bin(path, bin, sizeof bin) || !strcasecmp(bin + (strlen(bin) > 4 ? strlen(bin) - 4 : 0), ".cue"))
            return -1;
        if (recomp_pack_rom_identify(bin, &c, key, key_n) || c != RECOMP_CONSOLE_PS1)
            return -1;
        if (console)
            *console = c;
        return 0;
    }
    if (!strncmp(path, "fd:", 3)) {
        /* A document the shell opened (XBA-157): a dup of it, read from 0; the shell's descriptor stays open. */
        char *end;
        long v = strtol(path + 3, &end, 10);
        int fd = end != path + 3 && !*end && v >= 0 ? dup((int)v) : -1;
        if (fd < 0)
            return -1;
        if (!(f = fdopen(fd, "rb"))) {
            close(fd);
            return -1;
        }
        if (fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            return -1;
        }
    } else if (!(f = fopen(path, "rb"))) {
        return -1;
    }
    hn = fread(h, 1, sizeof h, f);
    if (n64_key(h, hn, key, key_n) == 0)
        c = RECOMP_CONSOLE_N64;
    else if (gba_key(f, h, hn, key, key_n) == 0)
        c = RECOMP_CONSOLE_GBA;
    else if (ps1_key(f, key, key_n) == 0)
        c = RECOMP_CONSOLE_PS1;
    fclose(f);
    if (c == RECOMP_CONSOLE_UNKNOWN)
        return -1;
    if (console)
        *console = c;
    return 0;
}

/* ---- pack.json------------------------------------------------------------ */

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

/* "env_force": {"KEY": "VALUE", ...} into m->env_force. A value that is not
 * a string is an error; an entry that does not fit is counted and dropped. */
/* One "key": "value" pair of an object of strings; 0 kept, 1 dropped. */
typedef int (*jpair_fn)(recomp_pack_meta *m, const char *key, int kfit, const char *value, int vfit);

static int env_force_pair(recomp_pack_meta *m, const char *key, int kfit, const char *value, int vfit)
{
    if (!kfit || !vfit || m->env_force_n >= RECOMP_PACK_ENV_FORCE_MAX || strlen(key) >= sizeof m->env_force[0].key)
        return 1;
    snprintf(m->env_force[m->env_force_n].key, sizeof m->env_force[0].key, "%s", key);
    snprintf(m->env_force[m->env_force_n].value, sizeof m->env_force[0].value, "%s", value);
    m->env_force_n++;
    return 0;
}

static int image_pair(recomp_pack_meta *m, const char *key, int kfit, const char *value, int vfit)
{
    if (!kfit || !vfit || m->images_n >= RECOMP_PACK_IMAGES_MAX || strlen(value) >= sizeof m->images[0].lib)
        return 1;
    snprintf(m->images[m->images_n].path, sizeof m->images[0].path, "%s", key);
    snprintf(m->images[m->images_n].lib, sizeof m->images[0].lib, "%s", value);
    m->images_n++;
    return 0;
}

/* An object of strings ("env_force", "images"), each pair through fn. */
static int jstrmap(jr *r, const char *name, recomp_pack_meta *m, jpair_fn fn, int *dropped, char *err, size_t errn)
{
    if (r->p >= r->end || *r->p != '{') { say(err, errn, "%s: object expected", name); return -1; }
    r->p++;
    ws(r);
    if (r->p < r->end && *r->p == '}') { r->p++; return 0; }
    for (;;) {
        char key[256], value[256];
        int kfit = 1, vfit = 1;
        ws(r);
        if (jstring(r, key, sizeof key, &kfit)) goto bad;
        ws(r);
        if (r->p >= r->end || *r->p != ':') { r->err = "':' expected"; goto bad; }
        r->p++;
        ws(r);
        if (r->p >= r->end || *r->p != '"') { say(err, errn, "%s: string values expected", name); return -1; }
        if (jstring(r, value, sizeof value, &vfit)) goto bad;
        if (fn(m, key, kfit, value, vfit))
            (*dropped)++;
        ws(r);
        if (r->p < r->end && *r->p == ',') { r->p++; continue; }
        if (r->p < r->end && *r->p == '}') { r->p++; return 0; }
        r->err = "',' or '}' expected";
        goto bad;
    }
bad:
    return -2;     /* r->err says why: the caller's "bad JSON at byte N" */
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
    STR_FIELD("console", RECOMP_PACK_HAVE_CONSOLE, console),
    STR_FIELD("title_key", RECOMP_PACK_HAVE_TITLE_KEY, title_key),
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
        if (full && (!strcmp(key, "env_force") || !strcmp(key, "images"))) {
            int rc = key[0] == 'e' ? jstrmap(&r, key, m, env_force_pair, &m->env_force_dropped, err, errn)
                                   : jstrmap(&r, key, m, image_pair, &m->images_dropped, err, errn);
            if (rc == -1) return -1;
            if (rc) goto bad;
        } else if (!f) {
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
    if (!m->console[0])            /* a pack from before the field: an Xbox one */
        snprintf(m->console, sizeof m->console, "xbox");
    return 0;
bad:
    say(err, errn, "bad JSON at byte %ld: %s", (long)(r.p - json), r.err ? r.err : "?");
    return -1;
}

int recomp_pack_apply_env_force(const recomp_pack_meta *m)
{
    char tid[sizeof m->title_key];
    int n = 0;
    if (recomp_pack_console_id(m->console) != RECOMP_CONSOLE_XBOX)   /* XBA-147: the generic key */
        snprintf(tid, sizeof tid, "%s", m->title_key[0] ? m->title_key : "?");
    else if (recomp_pack_norm_title_id(m->title_id, tid))
        snprintf(tid, sizeof tid, "?");
    for (int i = 0; i < m->env_force_n; i++)
        if (recomp_cfg_force(tid, m->env_force[i].key, m->env_force[i].value) == 0)
            n++;
    if (m->env_force_dropped)
        fprintf(stderr, "[TITLE] %s: %d env_force entr%s of pack.json ignored (too many or too long)\n", tid,
                m->env_force_dropped, m->env_force_dropped == 1 ? "y" : "ies");
    return n;
}

/* The checks after the title's own: abi, shell_abi, lib and its size. */
static int validate_lib(const recomp_pack_meta *m, long long shell_abi, const char *pack_dir,
                        char *lib_path, size_t lib_path_n, char *reason, size_t reason_n)
{
    struct stat st;

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

/* Required fields: the HAVE bits of title (title_id or title_key) and the
 * five every pack has. */
static int validate_fields(const recomp_pack_meta *m, unsigned title_bit, const char *title_name,
                           char *reason, size_t reason_n)
{
    static const struct { unsigned bit; const char *name; } req[] = {
        { RECOMP_PACK_HAVE_FORMAT, "format" }, { 0, NULL }, { RECOMP_PACK_HAVE_ABI, "abi" },
        { RECOMP_PACK_HAVE_SHELL_ABI, "shell_abi" }, { RECOMP_PACK_HAVE_LIB, "lib" },
        { RECOMP_PACK_HAVE_LIB_SIZE, "lib_size" },
    };
    for (unsigned i = 0; i < sizeof req / sizeof req[0]; i++) {
        unsigned bit = req[i].bit ? req[i].bit : title_bit;
        if (!(m->have & bit)) {
            say(reason, reason_n, "pack.json has no %s", req[i].name ? req[i].name : title_name);
            return -1;
        }
    }
    if (m->format != RECOMP_PACK_FORMAT) {
        say(reason, reason_n, "format %lld, this shell reads %d", m->format, RECOMP_PACK_FORMAT);
        return -1;
    }
    return 0;
}

int recomp_pack_validate(const recomp_pack_meta *m, const char *tid, long long shell_abi,
                         const char *pack_dir, char *lib_path, size_t lib_path_n,
                         char *reason, size_t reason_n)
{
    char a[9], b[9];

    if (validate_fields(m, RECOMP_PACK_HAVE_TITLE_ID, "title_id", reason, reason_n))
        return -1;
    if (recomp_pack_console_id(m->console) != RECOMP_CONSOLE_XBOX) {
        say(reason, reason_n, "console %s, the game is an xbox title", m->console);
        return -1;
    }
    if (recomp_pack_norm_title_id(m->title_id, a) || recomp_pack_norm_title_id(tid, b) || strcmp(a, b)) {
        say(reason, reason_n, "title_id %s, the game is %s", m->title_id, tid ? tid : "?");
        return -1;
    }
    return validate_lib(m, shell_abi, pack_dir, lib_path, lib_path_n, reason, reason_n);
}

int recomp_pack_validate_key(const recomp_pack_meta *m, int console, const char *key, long long shell_abi,
                             const char *pack_dir, char *lib_path, size_t lib_path_n,
                             char *reason, size_t reason_n)
{
    if (console == RECOMP_CONSOLE_XBOX)
        return recomp_pack_validate(m, key, shell_abi, pack_dir, lib_path, lib_path_n, reason, reason_n);
    if (validate_fields(m, RECOMP_PACK_HAVE_TITLE_KEY, "title_key", reason, reason_n))
        return -1;
    if (recomp_pack_console_id(m->console) != console) {
        say(reason, reason_n, "console %s, the game is a %s title", m->console, recomp_pack_console_name(console));
        return -1;
    }
    if (!key || strcmp(m->title_key, key)) {
        say(reason, reason_n, "title_key %s, the game is %s", m->title_key, key ? key : "?");
        return -1;
    }
    return validate_lib(m, shell_abi, pack_dir, lib_path, lib_path_n, reason, reason_n);
}

/* An image path for comparison: lowercase, '/', no leading separator. */
static void image_norm(const char *p, char *out, size_t n)
{
    size_t o = 0;
    while (*p == '/' || *p == '\\')
        p++;
    for (; *p && o + 1 < n; p++) {
        char c = *p == '\\' ? '/' : *p;
        out[o++] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    out[o] = 0;
}

int recomp_pack_image_lib(const recomp_pack_meta *m, const char *pack_dir, const char *lib_path,
                          const char *path, char *out, size_t out_n, char *key, size_t key_n,
                          char *reason, size_t reason_n)
{
    char want[256], have[256];
    struct stat st;

    image_norm(path ? path : "", want, sizeof want);
    if (!want[0] || !strcmp(want, "default.xbe")) {
        snprintf(out, out_n, "%s", lib_path);
        if (key && key_n)
            snprintf(key, key_n, "default.xbe");
        return 0;
    }
    for (int i = 0; i < m->images_n; i++) {
        const char *lib = m->images[i].lib;
        image_norm(m->images[i].path, have, sizeof have);
        if (strcmp(have, want))
            continue;
        if (!lib[0] || strchr(lib, '/') || !strcmp(lib, ".") || !strcmp(lib, "..")) {
            say(reason, reason_n, "image %s: lib \"%s\" is not a file name", path, lib);
            return -1;
        }
        if ((size_t)snprintf(out, out_n, "%s/%s", pack_dir, lib) >= out_n) {
            say(reason, reason_n, "path too long");
            return -1;
        }
        if (stat(out, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            say(reason, reason_n, "image %s: %s missing", path, lib);
            return -1;
        }
        if (key && key_n)
            snprintf(key, key_n, "%s", m->images[i].path);
        return 0;
    }
    say(reason, reason_n, "image %s is not in the pack", path);
    return -1;
}

static int read_pack_json(const char *dir, recomp_pack_meta *m, char *reason, size_t reason_n);

int recomp_pack_find(const char *files_dir, const char *tid, long long shell_abi,
                     recomp_pack_meta *m, char *lib_path, size_t lib_path_n,
                     char *reason, size_t reason_n)
{
    char dir[RECOMP_PACK_PATH_MAX];
    char id[9];

    memset(m, 0, sizeof *m);
    if (!files_dir || !files_dir[0] || recomp_pack_norm_title_id(tid, id)) {
        say(reason, reason_n, "no files dir or title ID");
        return RECOMP_PACK_INVALID;
    }
    if ((size_t)snprintf(dir, sizeof dir, "%s/packs/%s", files_dir, id) >= sizeof dir) {
        say(reason, reason_n, "path too long");
        return RECOMP_PACK_INVALID;
    }
    int rc = read_pack_json(dir, m, reason, reason_n);
    if (rc != RECOMP_PACK_OK)
        return rc;
    if (recomp_pack_validate(m, id, shell_abi, dir, lib_path, lib_path_n, reason, reason_n))
        return RECOMP_PACK_INVALID;
    return RECOMP_PACK_OK;
}

int recomp_pack_find_key(const char *files_dir, int console, const char *key, long long shell_abi,
                         recomp_pack_meta *m, char *lib_path, size_t lib_path_n,
                         char *reason, size_t reason_n)
{
    char dir[RECOMP_PACK_PATH_MAX], name[64];

    if (console == RECOMP_CONSOLE_XBOX)
        return recomp_pack_find(files_dir, key, shell_abi, m, lib_path, lib_path_n, reason, reason_n);
    memset(m, 0, sizeof *m);
    if (!files_dir || !files_dir[0] || recomp_pack_dir_name(console, key, name, sizeof name)) {
        say(reason, reason_n, "no files dir or title key");
        return RECOMP_PACK_INVALID;
    }
    if ((size_t)snprintf(dir, sizeof dir, "%s/packs/%s", files_dir, name) >= sizeof dir) {
        say(reason, reason_n, "path too long");
        return RECOMP_PACK_INVALID;
    }
    int rc = read_pack_json(dir, m, reason, reason_n);
    if (rc != RECOMP_PACK_OK)
        return rc;
    if (recomp_pack_validate_key(m, console, key, shell_abi, dir, lib_path, lib_path_n, reason, reason_n))
        return RECOMP_PACK_INVALID;
    return RECOMP_PACK_OK;
}

/* <dir>/pack.json, read and parsed into m. */
static int read_pack_json(const char *dir, recomp_pack_meta *m, char *reason, size_t reason_n)
{
    char json[RECOMP_PACK_PATH_MAX + 16], err[160];
    FILE *f;

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
