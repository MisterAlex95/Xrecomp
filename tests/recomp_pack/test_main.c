/* A title pack, the loader's side (recomp_pack.h, pack_loader.h): title ID,
 * pack.json, checks, and the routing of a dlopened pack's symbols through the
 * shell's weak forwarders. See CMakeLists.txt. */
#define _GNU_SOURCE
#include "recomp_pack.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char root[512];

static void put32(unsigned char *b, size_t o, uint32_t v)
{
    b[o] = (unsigned char)v; b[o + 1] = (unsigned char)(v >> 8);
    b[o + 2] = (unsigned char)(v >> 16); b[o + 3] = (unsigned char)(v >> 24);
}

/* A minimal XBE: "XBEH", base 0x10000, certificate at 0x10200. */
static void make_xbe(unsigned char b[0x400], uint32_t tid)
{
    memset(b, 0, 0x400);
    memcpy(b, "XBEH", 4);
    put32(b, 0x104, 0x10000);
    put32(b, 0x118, 0x10200);
    put32(b, 0x208, tid);
}

static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(data, 1, n, f);
    fclose(f);
}

static void mkdirs(const char *path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

static long copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    char buf[65536];
    size_t n;
    long total = 0;
    if (!a || !b) { perror(a ? to : from); exit(1); }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) { fwrite(buf, 1, n, b); total += (long)n; }
    fclose(a);
    fclose(b);
    return total;
}

static void write_pack_json(const char *dir, const char *tid, long long shell_abi, long long lib_size)
{
    char path[1024], text[2048];
    int n = snprintf(text, sizeof text,
        "{\n \"format\": 1,\n \"title_id\": \"%s\",\n \"title\": \"Test \\\"pack\\\" \\u00e9\",\n"
        " \"xbe_sha256\": \"00\",\n \"abi\": \"%s\",\n \"shell_abi\": %lld,\n \"toolkit\": \"test\",\n"
        " \"lib\": \"libtitle.so\",\n \"lib_sha256\": \"ab\",\n \"lib_size\": %lld,\n"
        " \"entry\": \"recomp_title_main\",\n \"cc\": \"clang 19 -O2\",\n \"built_at\": \"2026-11-10T00:00:00Z\"\n}\n",
        tid, recomp_pack_host_abi(), shell_abi, lib_size);
    snprintf(path, sizeof path, "%s/pack.json", dir);
    write_file(path, text, (size_t)n);
}

static void test_title_id(void)
{
    unsigned char x[0x400];
    char id[9], path[1024];

    make_xbe(x, 0x4D530004);
    CHECK(recomp_pack_xbe_title_id_mem(x, sizeof x, id) == 0 && !strcmp(id, "4D530004"));
    CHECK(recomp_pack_xbe_title_id_mem(x, 0x200, id) == -1);           /* certificate past the end */
    snprintf(path, sizeof path, "%s/default.xbe", root);
    write_file(path, x, sizeof x);
    CHECK(recomp_pack_xbe_title_id(path, id) == 0 && !strcmp(id, "4D530004"));
    x[0] = 'Y';
    CHECK(recomp_pack_xbe_title_id_mem(x, sizeof x, id) == -1);
    make_xbe(x, 1);
    put32(x, 0x118, 0x100);                                            /* certificate below base */
    CHECK(recomp_pack_xbe_title_id_mem(x, sizeof x, id) == -1);
    CHECK(recomp_pack_xbe_title_id("/nonexistent/default.xbe", id) == -1);

    CHECK(recomp_pack_norm_title_id("0x4d530004", id) == 0 && !strcmp(id, "4D530004"));
    CHECK(recomp_pack_norm_title_id(" 12ab ", id) == 0 && !strcmp(id, "000012AB"));
    CHECK(recomp_pack_norm_title_id("123456789", id) == -1);
    CHECK(recomp_pack_norm_title_id("12G4", id) == -1);
    CHECK(recomp_pack_norm_title_id("", id) == -1);
}

static void test_parse(void)
{
    recomp_pack_meta m;
    char err[160];
    const char *full =
        "\xef\xbb\xbf{\"format\":1,\"title_id\":\"4D530004\",\"title\":\"A \\\"b\\\" \\u00e9\\ud83d\\ude00\","
        "\"future\":{\"x\":[1,2.5e3,{\"y\":null}],\"z\":true},\"abi\":\"arm64-v8a\",\"shell_abi\":1,"
        "\"lib\":\"libtitle.so\",\"lib_sha256\":\"ff\",\"lib_size\":123456789012,\"neg\":-3}";

    CHECK(recomp_pack_parse(full, strlen(full), &m, err, sizeof err) == 0);
    CHECK(m.have == RECOMP_PACK_REQUIRED);
    CHECK(m.format == 1 && m.shell_abi == 1 && m.lib_size == 123456789012LL);
    CHECK(!strcmp(m.title_id, "4D530004") && !strcmp(m.abi, "arm64-v8a") && !strcmp(m.lib, "libtitle.so"));
    CHECK(!strcmp(m.title, "A \"b\" \xc3\xa9\xf0\x9f\x98\x80"));
    CHECK(!strcmp(m.entry, "recomp_title_main"));                       /* default */

    CHECK(recomp_pack_parse("{}", 2, &m, err, sizeof err) == 0 && m.have == 0);
    CHECK(recomp_pack_parse("[]", 2, &m, err, sizeof err) == -1);
    CHECK(recomp_pack_parse("{\"shell_abi\":\"1\"}", 17, &m, err, sizeof err) == -1);
    CHECK(strstr(err, "shell_abi") != NULL);
    CHECK(recomp_pack_parse("{\"format\":1.0}", 14, &m, err, sizeof err) == -1);
    CHECK(recomp_pack_parse("{\"format\":1} x", 14, &m, err, sizeof err) == -1);
    CHECK(recomp_pack_parse("{\"format\":1,}", 13, &m, err, sizeof err) == -1);
    CHECK(recomp_pack_parse("{\"title\":\"abc", 13, &m, err, sizeof err) == -1);
    CHECK(recomp_pack_parse("{\"format\":1", 11, &m, err, sizeof err) == -1);
}

/* "env_force" (title.json, copied by tools/prepare/pack.py): parsed, then set over the environment. */
static void test_env_force(void)
{
    recomp_pack_meta m;
    char err[160];
    const char *j = "{\"title_id\":\"4541005b\",\"env_force\":{\"RECOMP_LSFG\":\"0\",\"RECOMP_GPU_FPS_CAP\":\"60\","
                    "\"LD_PRELOAD\":\"x\"},\"format\":1}";
    char big[4096];

    CHECK(recomp_pack_parse(j, strlen(j), &m, err, sizeof err) == 0);
    CHECK(m.format == 1 && m.env_force_n == 3 && m.env_force_dropped == 0);
    CHECK(!strcmp(m.env_force[0].key, "RECOMP_LSFG") && !strcmp(m.env_force[0].value, "0"));
    CHECK(!strcmp(m.env_force[1].key, "RECOMP_GPU_FPS_CAP") && !strcmp(m.env_force[1].value, "60"));
    setenv("RECOMP_LSFG", "1", 1);            /* as recomp.cfg would have set it */
    unsetenv("RECOMP_GPU_FPS_CAP");
    unsetenv("LD_PRELOAD_X");
    CHECK(recomp_pack_apply_env_force(&m) == 2);   /* LD_PRELOAD refused */
    CHECK(getenv("RECOMP_LSFG") && !strcmp(getenv("RECOMP_LSFG"), "0"));
    CHECK(getenv("RECOMP_GPU_FPS_CAP") && !strcmp(getenv("RECOMP_GPU_FPS_CAP"), "60"));
    unsetenv("RECOMP_LSFG");
    unsetenv("RECOMP_GPU_FPS_CAP");

    /* No env_force, an empty one: nothing to set. */
    CHECK(recomp_pack_parse("{\"env_force\":{}}", strlen("{\"env_force\":{}}"), &m, err, sizeof err) == 0 && m.env_force_n == 0);
    CHECK(recomp_pack_apply_env_force(&m) == 0);
    /* Wrong types fail like any known field. */
    CHECK(recomp_pack_parse("{\"env_force\":[]}", strlen("{\"env_force\":[]}"), &m, err, sizeof err) == -1 && strstr(err, "env_force"));
    CHECK(recomp_pack_parse("{\"env_force\":{\"RECOMP_A\":1}}", strlen("{\"env_force\":{\"RECOMP_A\":1}}"), &m, err, sizeof err) == -1
          && strstr(err, "env_force"));
    CHECK(recomp_pack_parse("{\"env_force\":{\"RECOMP_A\":\"1\"", strlen("{\"env_force\":{\"RECOMP_A\":\"1\""), &m, err, sizeof err) == -1);
    /* Too many entries: the first RECOMP_PACK_ENV_FORCE_MAX kept, the others counted. */
    {
        size_t k = (size_t)snprintf(big, sizeof big, "{\"env_force\":{");
        for (int i = 0; i < RECOMP_PACK_ENV_FORCE_MAX + 2; i++)
            k += (size_t)snprintf(big + k, sizeof big - k, "%s\"RECOMP_T%d\":\"%d\"", i ? "," : "", i, i);
        snprintf(big + k, sizeof big - k, "}}");
        CHECK(recomp_pack_parse(big, strlen(big), &m, err, sizeof err) == 0);
        CHECK(m.env_force_n == RECOMP_PACK_ENV_FORCE_MAX && m.env_force_dropped == 2);
    }
}

/* "images" (XBA-82): the disc's other images, each one's library in the pack folder. */
static void test_images(void)
{
    recomp_pack_meta m;
    char err[160], dir[1024], lib[1200], out[1200], key[256];
    const char *j = "{\"format\":1,\"lib\":\"libtitle.so\",\"images\":{\"Sub/Game.xbe\":\"libimage-1.so\","
                    "\"bad.xbe\":\"../x.so\",\"gone.xbe\":\"libimage-3.so\",\"lazy.xbe\":\"\"}}";

    CHECK(recomp_pack_parse(j, strlen(j), &m, err, sizeof err) == 0);
    CHECK(m.images_n == 4 && m.images_dropped == 0);
    CHECK(!strcmp(m.images[0].path, "Sub/Game.xbe") && !strcmp(m.images[0].lib, "libimage-1.so"));
    snprintf(dir, sizeof dir, "%s/images", root);
    mkdirs(dir);
    snprintf(lib, sizeof lib, "%s/libtitle.so", dir);
    write_file(lib, "x", 1);
    snprintf(out, sizeof out, "%s/libimage-1.so", dir);
    write_file(out, "y", 1);
    CHECK(recomp_pack_image_lib(&m, dir, lib, "default.xbe", out, sizeof out, key, sizeof key, err, sizeof err) == 0
          && !strcmp(out, lib) && !strcmp(key, "default.xbe"));
    CHECK(recomp_pack_image_lib(&m, dir, lib, "", out, sizeof out, NULL, 0, err, sizeof err) == 0 && !strcmp(out, lib));
    CHECK(recomp_pack_image_lib(&m, dir, lib, "\\SUB\\game.XBE", out, sizeof out, key, sizeof key, err, sizeof err) == 0
          && strstr(out, "/libimage-1.so") && !strcmp(key, "Sub/Game.xbe"));
    CHECK(recomp_pack_image_lib(&m, dir, lib, "bad.xbe", out, sizeof out, NULL, 0, err, sizeof err) == -1
          && strstr(err, "not a file name"));
    CHECK(recomp_pack_image_lib(&m, dir, lib, "gone.xbe", out, sizeof out, NULL, 0, err, sizeof err) == -1
          && strstr(err, "missing"));
    CHECK(recomp_pack_image_lib(&m, dir, lib, "other.xbe", out, sizeof out, NULL, 0, err, sizeof err) == -1
          && strstr(err, "not in the pack"));
    /* Named, not prepared yet (lazy): its own result, the key as pack.json spells it. */
    CHECK(recomp_pack_image_lib(&m, dir, lib, "\\LAZY.xbe", out, sizeof out, key, sizeof key, err, sizeof err)
              == RECOMP_PACK_IMAGE_LAZY
          && !strcmp(key, "lazy.xbe") && strstr(err, "not prepared yet"));
    CHECK(recomp_pack_parse("{\"images\":[]}", 13, &m, err, sizeof err) == -1 && strstr(err, "images"));
}

static void test_validate_and_find(void)
{
    recomp_pack_meta m;
    char dir[1024], lib[1024], path[1200], reason[256];
    unsigned char so[100] = { 0 };

    snprintf(dir, sizeof dir, "%s/files/packs/4D530004", root);
    mkdirs(dir);
    snprintf(path, sizeof path, "%s/libtitle.so", dir);
    write_file(path, so, sizeof so);

    char files[1024];
    snprintf(files, sizeof files, "%s/files", root);
    CHECK(recomp_pack_find(files, "12345678", 1, &m, lib, sizeof lib, reason, sizeof reason) == RECOMP_PACK_MISSING);

    write_pack_json(dir, "4D530004", 1, 100);
    CHECK(recomp_pack_find(files, "4d530004", 1, &m, lib, sizeof lib, reason, sizeof reason) == RECOMP_PACK_OK);
    CHECK(!strcmp(lib, path));
    CHECK(!strcmp(m.title, "Test \"pack\" \xc3\xa9"));

    CHECK(recomp_pack_find(files, "4D530004", 2, &m, lib, sizeof lib, reason, sizeof reason) == RECOMP_PACK_INVALID);
    CHECK(strstr(reason, "shell_abi 1, this shell is 2") != NULL);

    write_pack_json(dir, "4D530004", 1, 99);
    CHECK(recomp_pack_find(files, "4D530004", 1, &m, lib, sizeof lib, reason, sizeof reason) == RECOMP_PACK_INVALID);
    CHECK(strstr(reason, "100 bytes") != NULL);

    write_pack_json(dir, "4D530005", 1, 100);
    CHECK(recomp_pack_find(files, "4D530004", 1, &m, lib, sizeof lib, reason, sizeof reason) == RECOMP_PACK_INVALID);
    CHECK(strstr(reason, "title_id") != NULL);

    snprintf(path, sizeof path, "%s/pack.json", dir);
    write_file(path, "{\"format\":", 10);
    CHECK(recomp_pack_find(files, "4D530004", 1, &m, lib, sizeof lib, reason, sizeof reason) == RECOMP_PACK_INVALID);
    CHECK(strstr(reason, "pack.json") != NULL);

    /* validate on its own */
    write_pack_json(dir, "4D530004", 1, 100);
    CHECK(recomp_pack_find(files, "4D530004", 1, &m, lib, sizeof lib, reason, sizeof reason) == RECOMP_PACK_OK);
    recomp_pack_meta b = m;
    b.format = 2;
    CHECK(recomp_pack_validate(&b, "4D530004", 1, dir, lib, sizeof lib, reason, sizeof reason) == -1);
    CHECK(strstr(reason, "format 2") != NULL);
    b = m;
    snprintf(b.abi, sizeof b.abi, "mips");
    CHECK(recomp_pack_validate(&b, "4D530004", 1, dir, lib, sizeof lib, reason, sizeof reason) == -1);
    b = m;
    snprintf(b.lib, sizeof b.lib, "../libtitle.so");
    CHECK(recomp_pack_validate(&b, "4D530004", 1, dir, lib, sizeof lib, reason, sizeof reason) == -1);
    b = m;
    snprintf(b.lib, sizeof b.lib, "libother.so");
    CHECK(recomp_pack_validate(&b, "4D530004", 1, dir, lib, sizeof lib, reason, sizeof reason) == -1);
    CHECK(strstr(reason, "missing") != NULL);
    b = m;
    b.have &= ~RECOMP_PACK_HAVE_LIB_SIZE;
    CHECK(recomp_pack_validate(&b, "4D530004", 1, dir, lib, sizeof lib, reason, sizeof reason) == -1);
    CHECK(strstr(reason, "lib_size") != NULL);

    /* XBA-147: a pack.json from before the console field is an Xbox pack and
     * loads as before; "console": "xbox" said aloud too; any other console is
     * not this Xbox title's pack. */
    CHECK(recomp_pack_find(files, "4D530004", 1, &m, lib, sizeof lib, reason, sizeof reason) == RECOMP_PACK_OK);
    CHECK(!strcmp(m.console, "xbox") && !(m.have & RECOMP_PACK_HAVE_CONSOLE) && !m.title_key[0]);
    CHECK(recomp_pack_find_key(files, RECOMP_CONSOLE_XBOX, "4d530004", 1, &m, lib, sizeof lib, reason,
                               sizeof reason) == RECOMP_PACK_OK);
    b = m;
    snprintf(b.console, sizeof b.console, "xbox");
    b.have |= RECOMP_PACK_HAVE_CONSOLE;
    CHECK(recomp_pack_validate(&b, "4D530004", 1, dir, lib, sizeof lib, reason, sizeof reason) == 0);
    snprintf(b.console, sizeof b.console, "gba");
    CHECK(recomp_pack_validate(&b, "4D530004", 1, dir, lib, sizeof lib, reason, sizeof reason) == -1);
    CHECK(strstr(reason, "console gba") != NULL);
}

/* ---- Other consoles (XBA-147) -------------------------------------------- */

static uint32_t crc32_ref(const unsigned char *p, size_t n)   /* bit by bit, not the library's table */
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* A homebrew-shaped GBA ROM: game code, 0x96 at 0xB2, the complement check. */
static void make_gba(unsigned char *b, size_t n, const char code[4])
{
    unsigned char sum = 0;
    memset(b, 0, n);
    for (size_t i = 0xC0; i < n; i++)
        b[i] = (unsigned char)(i * 7);
    memcpy(b + 0xA0, "XRECOMPTEST ", 12);
    memcpy(b + 0xAC, code, 4);
    memcpy(b + 0xB0, "01", 2);
    b[0xB2] = 0x96;
    for (int i = 0xA0; i <= 0xBC; i++)
        sum = (unsigned char)(sum - b[i]);
    b[0xBD] = (unsigned char)(sum - 0x19);
}

/* A PS1 data track: ISO 9660, system ID PLAYSTATION, SYSTEM.CNF holding cnf
 * (none when NULL); 2048-byte sectors, or raw 2352-byte mode 2 ones. */
static void make_ps1(const char *path, int raw, const char *cnf)
{
    enum { SECTORS = 24 };
    static unsigned char s[SECTORS][2048];
    unsigned char *pvd = s[16], *root = s[20], *r;
    memset(s, 0, sizeof s);
    pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    memset(pvd + 8, ' ', 32); memcpy(pvd + 8, "PLAYSTATION", 11);
    memset(pvd + 40, ' ', 32); memcpy(pvd + 40, "XRECOMP_TEST", 12);
    r = pvd + 156;                         /* the root directory's record */
    r[0] = 34; put32(r, 2, 20); put32(r, 10, 2048); r[25] = 2; r[32] = 1;
    s[17][0] = 0xff; memcpy(s[17] + 1, "CD001", 5);
    r = root;                              /* ".", "..", SYSTEM.CNF;1 */
    r[0] = 34; put32(r, 2, 20); put32(r, 10, 2048); r[25] = 2; r[32] = 1; r[33] = 0; r += 34;
    r[0] = 34; put32(r, 2, 20); put32(r, 10, 2048); r[25] = 2; r[32] = 1; r[33] = 1; r += 34;
    r[0] = 33 + 12 + 1; put32(r, 2, 21); put32(r, 10, cnf ? (uint32_t)strlen(cnf) : 0); r[32] = 12;
    memcpy(r + 33, cnf ? "SYSTEM.CNF;1" : "OTHER.TXT;1 ", 12);
    if (cnf)
        memcpy(s[21], cnf, strlen(cnf));
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    for (int i = 0; i < SECTORS; i++) {
        if (raw) {
            unsigned char head[24] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
            unsigned char tail[2352 - 24 - 2048] = { 0 };
            head[15] = 2;
            fwrite(head, 1, sizeof head, f);
            fwrite(s[i], 1, 2048, f);
            fwrite(tail, 1, sizeof tail, f);
        } else {
            fwrite(s[i], 1, 2048, f);
        }
    }
    fclose(f);
}

/* A minimal DOL: text 0 at file 0x100, loaded at 0x80003100 (0x100 bytes), data 0 after it, a BSS, the entry at the
 * text's start. */
static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void make_dol(unsigned char *d, size_t n)
{
    memset(d, 0, n);
    put_be32(d + 0x00, 0x100);        /* text 0 offset */
    put_be32(d + 0x48, 0x80003100u);  /* text 0 address */
    put_be32(d + 0x90, 0x100);        /* text 0 size */
    put_be32(d + 0x1C, 0x200);        /* data 0 offset */
    put_be32(d + 0x64, 0x80003200u);  /* data 0 address */
    put_be32(d + 0xAC, 0x100);        /* data 0 size */
    put_be32(d + 0xD8, 0x80003300u);  /* BSS */
    put_be32(d + 0xDC, 0x1000);
    put_be32(d + 0xE0, 0x80003100u);  /* entry */
    for (size_t i = 0x100; i < n; i++)
        d[i] = (unsigned char)(i * 7);
}

/* ---- Xbox 360 (XBA-280) ---- */

static void be32_put(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24), p[1] = (unsigned char)(v >> 16), p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void le32_put(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* an XEX2 header whose execution info (at 0x20) has title id tid */
static void make_xex(unsigned char *x, size_t n, uint32_t tid)
{
    memset(x, 0, n);
    memcpy(x, "XEX2", 4);
    be32_put(x + 20, 1);
    be32_put(x + 24, 0x00040006u);
    be32_put(x + 28, 0x20);
    be32_put(x + 0x20, 0x11223344u);
    be32_put(x + 0x20 + 12, tid);
}

/* an XDVDFS volume at base in the file at path: the volume descriptor at sector 32, a root directory (sector 40)
 * with "readme.txt" (left) and `name` (the root node), the file's bytes at sector 48 */
static void make_xdvdfs(const char *path, long long base, const char *name, const unsigned char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    unsigned char vd[0x800], dir[0x800];
    memset(vd, 0, sizeof vd);
    memcpy(vd, "MICROSOFT*XBOX*MEDIA", 20);
    le32_put(vd + 20, 40);
    le32_put(vd + 24, 0x800);
    memcpy(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    memset(dir, 0xFF, sizeof dir);
    size_t len = strlen(name);
    /* node 0: name, left = the entry at dword 8 */
    dir[0] = 8, dir[1] = 0, dir[2] = 0, dir[3] = 0;
    le32_put(dir + 4, 48);
    le32_put(dir + 8, (uint32_t)n);
    dir[12] = 0x20, dir[13] = (unsigned char)len;
    memcpy(dir + 14, name, len);
    /* node at 32: readme.txt, no children */
    memset(dir + 32, 0, 14);
    le32_put(dir + 32 + 4, 49);
    le32_put(dir + 32 + 8, 7);
    dir[32 + 12] = 0x20, dir[32 + 13] = 10;
    memcpy(dir + 32 + 14, "readme.txt", 10);
    fseeko(f, (off_t)(base + 0x10000), SEEK_SET);
    fwrite(vd, 1, sizeof vd, f);
    fseeko(f, (off_t)(base + 40 * 2048), SEEK_SET);
    fwrite(dir, 1, sizeof dir, f);
    fseeko(f, (off_t)(base + 48 * 2048), SEEK_SET);
    fwrite(data, 1, n, f);
    fclose(f);
}

static void test_x360(void)
{
    char path[1024], key[64], want[64];
    unsigned char x[0x200];
    int c;
    CHECK(recomp_pack_console_id("x360") == RECOMP_CONSOLE_X360 &&
          !strcmp(recomp_pack_console_name(RECOMP_CONSOLE_X360), "x360"));
    make_xex(x, sizeof x, 0x4D5307E6u);
    snprintf(want, sizeof want, "X360-4D5307E6-%08X", (unsigned)crc32_ref(x + 0x20, 24));
    snprintf(path, sizeof path, "%s/default.xex", root);
    write_file(path, x, sizeof x);
    CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_X360 && !strcmp(key, want));
    CHECK(recomp_pack_dir_name(RECOMP_CONSOLE_X360, key, want + 32, 32) == 0 && !strncmp(want + 32, "x360-X360-", 10));
    /* an XGD3 image (the game partition at 0x02080000, sparse here) and an extracted one: default.xex's key */
    snprintf(path, sizeof path, "%s/xgd3.iso", root);
    make_xdvdfs(path, 0x02080000LL, "default.xex", x, sizeof x);
    c = -1;
    CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_X360 &&
          !strncmp(key, "X360-4D5307E6-", 14));
    snprintf(path, sizeof path, "%s/x360.xiso", root);
    make_xdvdfs(path, 0, "DEFAULT.XEX", x, sizeof x);
    c = -1;
    CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_X360);
    /* an Xbox image (default.xbe at 0) stays the Xbox path's: not identified here */
    snprintf(path, sizeof path, "%s/xbox.iso", root);
    make_xdvdfs(path, 0, "default.xbe", (const unsigned char *)"XBEH", 4);
    CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == -1);
    x[0x20 + 4] = 1; /* another version: another key */
    snprintf(path, sizeof path, "%s/default.xex", root);
    write_file(path, x, sizeof x);
    CHECK(recomp_pack_rom_identify(path, &c, want, sizeof want) == 0 && strcmp(key, want) != 0);
}

static void test_consoles(void)
{
    char path[1024], path2[1024], key[64], name[64];
    unsigned char rom[0x400], n64[0x1000], v[0x1000];
    int c;

    CHECK(recomp_pack_console_id(NULL) == RECOMP_CONSOLE_XBOX && recomp_pack_console_id("") == RECOMP_CONSOLE_XBOX);
    CHECK(recomp_pack_console_id("gba") == RECOMP_CONSOLE_GBA && recomp_pack_console_id("ps1") == RECOMP_CONSOLE_PS1);
    CHECK(recomp_pack_console_id("n64") == RECOMP_CONSOLE_N64 && recomp_pack_console_id("xbox") == RECOMP_CONSOLE_XBOX);
    CHECK(recomp_pack_console_id("gc") == RECOMP_CONSOLE_GC && !strcmp(recomp_pack_console_name(RECOMP_CONSOLE_GC), "gc"));
    CHECK(recomp_pack_console_id("GBA") == RECOMP_CONSOLE_UNKNOWN && recomp_pack_console_id("snes") == RECOMP_CONSOLE_UNKNOWN);
    CHECK(!strcmp(recomp_pack_console_name(RECOMP_CONSOLE_PS1), "ps1") && !strcmp(recomp_pack_console_name(9), "?"));

    CHECK(recomp_pack_dir_name(RECOMP_CONSOLE_GBA, "AXVE-1A2B3C4D", name, sizeof name) == 0
          && !strcmp(name, "gba-AXVE-1A2B3C4D"));
    CHECK(recomp_pack_dir_name(RECOMP_CONSOLE_XBOX, "0x4d530004", name, sizeof name) == 0 && !strcmp(name, "4D530004"));
    CHECK(recomp_pack_dir_name(RECOMP_CONSOLE_PS1, "../x", name, sizeof name) == -1);
    CHECK(recomp_pack_dir_name(RECOMP_CONSOLE_PS1, "", name, sizeof name) == -1);
    CHECK(recomp_pack_dir_name(RECOMP_CONSOLE_UNKNOWN, "A", name, sizeof name) == -1);

    /* GBA: game code + CRC32 of the whole file; a code byte that is no
     * [A-Z0-9] becomes '_'; a broken header is no GBA ROM. */
    make_gba(rom, sizeof rom, "AXVE");
    snprintf(path, sizeof path, "%s/test.gba", root);
    write_file(path, rom, sizeof rom);
    {
        char want[32];
        snprintf(want, sizeof want, "AXVE-%08X", (unsigned)crc32_ref(rom, sizeof rom));
        CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_GBA && !strcmp(key, want));
        /* "fd:N", a document the app opened (XBA-157): the same key, read from 0, the descriptor left open. */
        int fd = open(path, O_RDONLY);
        char spec[32];
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(lseek(fd, 100, SEEK_SET) == 100);
            snprintf(spec, sizeof spec, "fd:%d", fd);
            c = -1;
            key[0] = 0;
            CHECK(recomp_pack_rom_identify(spec, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_GBA
                  && !strcmp(key, want));
            CHECK(fcntl(fd, F_GETFD) != -1);
            close(fd);
        }
        CHECK(recomp_pack_rom_identify("fd:-1", &c, key, sizeof key) == -1);
        CHECK(recomp_pack_rom_identify("fd:x", &c, key, sizeof key) == -1);
    }
    make_gba(rom, sizeof rom, "a\0 1");
    write_file(path, rom, sizeof rom);
    CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && !strncmp(key, "___1-", 5));
    rom[0xBD] ^= 1;
    write_file(path, rom, sizeof rom);
    CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == -1);

    /* N64: the same key from each byte order. */
    memset(n64, 0, sizeof n64);
    n64[0] = 0x80; n64[1] = 0x37; n64[2] = 0x12; n64[3] = 0x40;
    n64[0x10] = 0x63; n64[0x11] = 0x5A; n64[0x12] = 0x2B; n64[0x13] = 0xFF;
    memcpy(n64 + 0x3B, "NSME", 4);
    for (int order = 0; order < 3; order++) {
        for (size_t i = 0; i < sizeof n64; i++)
            v[i] = order == 0 ? n64[i] : order == 1 ? n64[i ^ 1] : n64[(i & ~(size_t)3) + 3 - (i & 3)];
        snprintf(path, sizeof path, "%s/test.%s", root, order == 0 ? "z64" : order == 1 ? "v64" : "n64");
        write_file(path, v, sizeof v);
        CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_N64
              && !strcmp(key, "NSME-635A2BFF"));
    }

    /* GameCube DOL (XBA-259): "DOL-" + CRC32 of the file, when every header
     * field makes sense; one that does not is no DOL. */
    {
        unsigned char dol[0x400];
        char want[32];
        make_dol(dol, sizeof dol);
        snprintf(path, sizeof path, "%s/test.dol", root);
        write_file(path, dol, sizeof dol);
        snprintf(want, sizeof want, "DOL-%08X", (unsigned)crc32_ref(dol, sizeof dol));
        CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_GC && !strcmp(key, want));
        dol[0xE3] = 0x00; /* entry 0x80003100 -> 0x80003100 & ~0xFF: before the text section */
        dol[0xE2] = 0x30;
        write_file(path, dol, sizeof dol);
        CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == -1);
        make_dol(dol, sizeof dol);
        dol[0x93] = 0xFF; /* text 0 longer than the file */
        dol[0x92] = 0x10;
        write_file(path, dol, sizeof dol);
        CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == -1);
        make_dol(dol, sizeof dol);
        dol[0xF0] = 1; /* the header's padding */
        write_file(path, dol, sizeof dol);
        CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == -1);
        memset(dol, 0, sizeof dol); /* all zero: no text section */
        write_file(path, dol, sizeof dol);
        CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == -1);
    }

    /* PS1: the serial from SYSTEM.CNF, 2048- and 2352-byte sectors, through a
     * .cue; without a serial-shaped boot file, the volume descriptor's CRC. */
    snprintf(path, sizeof path, "%s/test.iso", root);
    make_ps1(path, 0, "BOOT = cdrom:\\SLUS_005.94;1\r\nTCB = 4\r\n");
    CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_PS1 && !strcmp(key, "SLUS-00594"));
    snprintf(path, sizeof path, "%s/Test Game (Track 1).bin", root);
    make_ps1(path, 1, "BOOT=cdrom:\\scus_944.26;1\n");
    CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_PS1 && !strcmp(key, "SCUS-94426"));
    snprintf(path2, sizeof path2, "%s/test.cue", root);
    {
        const char *cue = "FILE \"Test Game (Track 1).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
        write_file(path2, cue, strlen(cue));
    }
    CHECK(recomp_pack_rom_identify(path2, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_PS1 && !strcmp(key, "SCUS-94426"));
    make_ps1(path, 1, "BOOT = cdrom:\\PSX.EXE;1\n");
    CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && !strncmp(key, "PSX-", 4) && strlen(key) == 12);
    make_ps1(path, 0, NULL);
    CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == 0 && !strncmp(key, "PSX-", 4));

    /* An Xbox image (XISO magic at 0x10000, no PLAYSTATION volume), an XBE,
     * a missing file: none is another console's game. */
    {
        static unsigned char x[0x10800];
        memset(x, 0, sizeof x);
        memcpy(x + 0x10000, "MICROSOFT*XBOX*MEDIA", 20);
        snprintf(path, sizeof path, "%s/xbox.iso", root);
        write_file(path, x, sizeof x);
        CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == -1);
        make_xbe(x, 0x4D530004);
        snprintf(path, sizeof path, "%s/default.xbe", root);
        write_file(path, x, 0x400);
        CHECK(recomp_pack_rom_identify(path, &c, key, sizeof key) == -1);
        CHECK(recomp_pack_rom_identify("/nonexistent/x.gba", &c, key, sizeof key) == -1);
    }

    /* A gba pack: found under packs/gba-<key>, checked on console and title_key. */
    {
        char files[1024], dir[1100], lib[1300], reason[256], so[1300], text[1024], jpath[1200];
        recomp_pack_meta m;
        unsigned char bytes[64] = { 1 };
        snprintf(files, sizeof files, "%s/consoles/files", root);
        snprintf(dir, sizeof dir, "%s/packs/gba-AXVE-00000001", files);
        CHECK(recomp_pack_find_key(files, RECOMP_CONSOLE_GBA, "AXVE-00000001", 1, &m, lib, sizeof lib, reason,
                                   sizeof reason) == RECOMP_PACK_MISSING);
        mkdirs(dir);
        snprintf(so, sizeof so, "%s/libtitle.so", dir);
        write_file(so, bytes, sizeof bytes);
        snprintf(text, sizeof text, "{\"format\":1,\"console\":\"gba\",\"title_key\":\"AXVE-00000001\",\"abi\":\"%s\","
                 "\"shell_abi\":1,\"lib\":\"libtitle.so\",\"lib_size\":64}", recomp_pack_host_abi());
        snprintf(jpath, sizeof jpath, "%s/pack.json", dir);
        write_file(jpath, text, strlen(text));
        CHECK(recomp_pack_find_key(files, RECOMP_CONSOLE_GBA, "AXVE-00000001", 1, &m, lib, sizeof lib, reason,
                                   sizeof reason) == RECOMP_PACK_OK);
        CHECK(!strcmp(lib, so) && !strcmp(m.console, "gba") && !strcmp(m.title_key, "AXVE-00000001"));
        recomp_pack_meta b = m;
        CHECK(recomp_pack_validate_key(&b, RECOMP_CONSOLE_GBA, "AXVE-00000002", 1, dir, lib, sizeof lib, reason,
                                       sizeof reason) == -1 && strstr(reason, "title_key"));
        CHECK(recomp_pack_validate_key(&b, RECOMP_CONSOLE_N64, "AXVE-00000001", 1, dir, lib, sizeof lib, reason,
                                       sizeof reason) == -1 && strstr(reason, "console gba"));
        CHECK(recomp_pack_validate(&b, "4D530004", 1, dir, lib, sizeof lib, reason, sizeof reason) == -1);
        b.have &= ~RECOMP_PACK_HAVE_TITLE_KEY;
        CHECK(recomp_pack_validate_key(&b, RECOMP_CONSOLE_GBA, "AXVE-00000001", 1, dir, lib, sizeof lib, reason,
                                       sizeof reason) == -1 && strstr(reason, "no title_key"));
        b = m;
        snprintf(b.console, sizeof b.console, "xbox");      /* no console field: an Xbox pack, not this one */
        CHECK(recomp_pack_validate_key(&b, RECOMP_CONSOLE_GBA, "AXVE-00000001", 1, dir, lib, sizeof lib, reason,
                                       sizeof reason) == -1);
        b = m;
        b.shell_abi = 9;
        CHECK(recomp_pack_validate_key(&b, RECOMP_CONSOLE_GBA, "AXVE-00000001", 1, dir, lib, sizeof lib, reason,
                                       sizeof reason) == -1 && strstr(reason, "shell_abi 9"));
        CHECK(recomp_pack_find_key(files, RECOMP_CONSOLE_GBA, "../x", 1, &m, lib, sizeof lib, reason,
                                   sizeof reason) == RECOMP_PACK_INVALID);
    }
}

/* ---- Routing ------------------------------------------------------------- */

typedef int (*title_main_fn)(const void *store);
typedef int (*load_fn)(const char *files_dir, const char *xbe, title_main_fn *main_out, char *reason, size_t n);

/* A game folder with default.xbe for tid and, if lib is given, a pack built
 * from it under <files>/packs/<tid>. Returns the xbe path in xbe. */
static void setup_game(const char *name, uint32_t tid, const char *lib, long long shell_abi,
                       char *files, char *xbe)
{
    char dir[1024], so[1200], id[9];
    unsigned char x[0x400];

    snprintf(id, sizeof id, "%08X", tid);
    sprintf(files, "%s/%s/files", root, name);
    snprintf(dir, sizeof dir, "%s/%s/game", root, name);
    mkdirs(dir);
    sprintf(xbe, "%s/default.xbe", dir);
    make_xbe(x, tid);
    write_file(xbe, x, sizeof x);
    if (!lib)
        return;
    snprintf(dir, sizeof dir, "%s/packs/%s", files, id);
    mkdirs(dir);
    snprintf(so, sizeof so, "%s/libtitle.so", dir);
    long n = copy_file(lib, so);
    write_pack_json(dir, id, shell_abi, n);
}

/* xba_pack_load_rom with stdout captured into log (through the file out): the runtime's lines are the checks. */
static int load_rom_logged(int (*load_rom)(const char *, const char *, int *, char *, size_t), const char *files,
                           const char *rom, int *runtime_rc, char *reason, size_t reason_n, const char *out,
                           char *log, size_t log_n)
{
    fflush(stdout);
    int saved = dup(1);
    FILE *cap = fopen(out, "w");
    dup2(fileno(cap), 1);
    int rc = load_rom(files, rom, runtime_rc, reason, reason_n);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    fclose(cap);
    cap = fopen(out, "r");
    size_t n = fread(log, 1, log_n - 1, cap);
    log[n] = 0;
    fclose(cap);
    fputs(log, stdout);
    return rc;
}

static void test_routing(const char *shell_path, const char *title_path, const char *title_min_path,
                         const char *static_path)
{
    char files[1024], xbe[1024], reason[512];
    title_main_fn main_fn;
    int cursor = -1;

    /* As SDLActivity loads libmain.so (System.loadLibrary: RTLD_NOW, local). */
    void *shell = dlopen(shell_path, RTLD_NOW | RTLD_LOCAL);
    if (!shell) { fprintf(stderr, "FAIL dlopen shell: %s\n", dlerror()); failures++; return; }
    load_fn load = (load_fn)dlsym(shell, "xba_pack_load");
    uintptr_t (*resolve)(uint32_t) = (uintptr_t (*)(uint32_t))dlsym(shell, "toolkit_resolve");
    int (*run)(uint32_t) = (int (*)(uint32_t))dlsym(shell, "toolkit_run");
    int (*frame)(int *) = (int (*)(int *))dlsym(shell, "toolkit_pb_frame");
    const int *abi = dlsym(shell, "xba_shell_abi");
    CHECK(load && resolve && run && frame && abi);
    if (!load || !resolve || !run || !frame || !abi)
        return;
    CHECK(*abi == 1);

    /* No pack loaded: the forwarders stand in. */
    CHECK(resolve(0x11000) == 0);
    CHECK(frame(&cursor) == 0 && cursor == 0);

    /* No pack for this title. */
    setup_game("missing", 0x12345678, NULL, 1, files, xbe);
    CHECK(load(files, xbe, &main_fn, reason, sizeof reason) == RECOMP_PACK_MISSING);
    CHECK(main_fn == NULL && !strcmp(reason, "12345678"));

    /* Wrong shell ABI, then a lib that is not one. */
    setup_game("abi", 0x12345678, title_path, 7, files, xbe);
    CHECK(load(files, xbe, &main_fn, reason, sizeof reason) == RECOMP_PACK_INVALID);
    CHECK(strstr(reason, "shell_abi 7") != NULL);
    {
        char so[1200], dir[1100];
        setup_game("garbage", 0x12345678, NULL, 1, files, xbe);
        snprintf(dir, sizeof dir, "%s/packs/12345678", files);
        mkdirs(dir);
        snprintf(so, sizeof so, "%s/libtitle.so", dir);
        write_file(so, "not an ELF file", 15);
        write_pack_json(dir, "12345678", 1, 15);
        CHECK(load(files, xbe, &main_fn, reason, sizeof reason) == RECOMP_PACK_DLOPEN);
        CHECK(main_fn == NULL && reason[0]);
    }

    /* A pack with its entry only: the other dlsyms find the shell's own
     * forwarders, which must not be installed (they would call themselves). */
    setup_game("min", 0x0000ABCD, title_min_path, 1, files, xbe);
    CHECK(load(files, xbe, &main_fn, reason, sizeof reason) == RECOMP_PACK_OK);
    CHECK(main_fn && main_fn(NULL) == 7);
    CHECK(resolve(0x11000) == 0);
    CHECK(frame(&cursor) == 0 && cursor == 0);

    /* The full pack: every toolkit call reaches it. */
    setup_game("full", 0x4D530004, title_path, 1, files, xbe);
    CHECK(load(files, xbe, &main_fn, reason, sizeof reason) == RECOMP_PACK_OK);
    CHECK(main_fn != NULL);
    if (main_fn) {
        CHECK(run(0x11000) == 0x11000);       /* recomp_lookup */
        CHECK(run(0x22000) == 0x22000);       /* recomp_lookup_manual */
        CHECK(run(0x33000) == -1);
        CHECK(frame(&cursor) == 5 && cursor == 3);
        unsigned char store = 0x10;
        CHECK(main_fn(&store) == 101 + 0x10);
        CHECK(frame(&cursor) == 5 && cursor == 4);
    }

    /* XBA-82: the last run handed over to another image of the disc
     * (work/<tid>/image.next): that image's library runs, with its XBE. */
    {
        const char *(*image_xbe)(void) = (const char *(*)(void))dlsym(shell, "xba_pack_image_xbe");
        int (*set_next)(const char *) = (int (*)(const char *))dlsym(shell, "xba_pack_set_next_image");
        char dir[1100], path[1300], text[2600], next[1300];
        long n;
        FILE *f;

        CHECK(image_xbe && set_next);
        setup_game("image", 0x4D530005, title_path, 1, files, xbe);
        snprintf(dir, sizeof dir, "%s/packs/4D530005", files);
        snprintf(path, sizeof path, "%s/libimage-1.so", dir);
        copy_file(title_min_path, path);
        snprintf(path, sizeof path, "%s/libtitle.so", dir);
        f = fopen(path, "rb");
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fclose(f);
        snprintf(text, sizeof text,
                 "{\"format\":1,\"title_id\":\"4D530005\",\"abi\":\"%s\",\"shell_abi\":1,\"lib\":\"libtitle.so\","
                 "\"lib_size\":%ld,\"images\":{\"Game2.xbe\":\"libimage-1.so\",\"Intro.xbe\":\"\"}}",
                 recomp_pack_host_abi(), n);
        snprintf(path, sizeof path, "%s/pack.json", dir);
        write_file(path, text, strlen(text));
        snprintf(dir, sizeof dir, "%s/work/4D530005", files);
        mkdirs(dir);
        snprintf(next, sizeof next, "%s/image.next", dir);
        write_file(next, "game2.XBE\n", 10);
        unsetenv("RECOMP_IMAGE");
        unsetenv("RECOMP_IMAGE_MAP");
        CHECK(load(files, xbe, &main_fn, reason, sizeof reason) == RECOMP_PACK_OK);
        CHECK(main_fn && main_fn(NULL) == 7);                /* the image's library */
        CHECK(access(next, F_OK) != 0);                      /* taken once */
        CHECK(getenv("RECOMP_IMAGE") && !strcmp(getenv("RECOMP_IMAGE"), "Game2.xbe"));
        if (image_xbe) {
            const char *ix = image_xbe();
            CHECK(ix && strstr(ix, "/image/game/Game2.xbe"));
        }
        CHECK(getenv("RECOMP_IMAGE_MAP") && strstr(getenv("RECOMP_IMAGE_MAP"), "/work/4D530005/images.map"));
        if (getenv("RECOMP_IMAGE_MAP")) {
            f = fopen(getenv("RECOMP_IMAGE_MAP"), "r");
            CHECK(f != NULL);
            if (f) {
                n = (long)fread(text, 1, sizeof text - 1, f);
                text[n] = 0;
                fclose(f);
                CHECK(!strcmp(text, "default.xbe\tdefault.xbe\nGame2.xbe\tGame2.xbe\nIntro.xbe\tIntro.xbe\n"));
            }
        }
        CHECK(set_next && set_next("default.xbe") == 0 && access(next, F_OK) == 0);
        /* Lazy images: the image handler asks the pack (built, named only, unknown) and writes image.pending. */
        {
            int (*state)(const char *) = (int (*)(const char *))dlsym(shell, "xba_pack_image_state");
            int (*pending)(const char *) = (int (*)(const char *))dlsym(shell, "xba_pack_set_pending_image");
            char pend[1300];
            CHECK(state && pending);
            if (state && pending) {
                CHECK(state("game2.xbe") == 0 && state("default.xbe") == 0);
                CHECK(state("\\intro.XBE") == RECOMP_PACK_IMAGE_LAZY);
                CHECK(state("other.xbe") == -1 && state("") == -1);
                snprintf(pend, sizeof pend, "%s/image.pending", dir);
                CHECK(pending("Intro.xbe") == 0);
                f = fopen(pend, "r");
                CHECK(f && fgets(text, sizeof text, f) && !strcmp(text, "Intro.xbe\n"));
                if (f)
                    fclose(f);
            }
        }
        unsetenv("RECOMP_IMAGE");
        unsetenv("RECOMP_IMAGE_MAP");
    }

    /* XBA-147: a game of another console goes to that console's runtime. A
     * gba pack the shell accepts is handed to gba_runtime_start (a stand-in
     * here, fake_gba_runtime.c: the real one checks rom_sha256, lib_sha256
     * and the table, consoles/gba/tests test_pack.c); without a pack, or with
     * one the shell refuses, the GBA runs interpreted anyway (XBA-158). */
    {
        typedef int (*load_rom_fn)(const char *, const char *, int *, char *, size_t);
        load_rom_fn load_rom = (load_rom_fn)dlsym(shell, "xba_pack_load_rom");
        int (*rom_console)(const char *) = (int (*)(const char *))dlsym(shell, "xba_pack_rom_console");
        char gba[1100], key[64], dir[1200], so[1300], text[1024], out[1300], log[4096];
        unsigned char rom[0x400], bytes[64] = { 0 };
        int c = -1, runtime_rc = -1, rc;

        CHECK(load_rom && rom_console);
        if (!load_rom || !rom_console)
            return;
        setup_game("gba", 0x4D530006, NULL, 1, files, xbe);
        snprintf(gba, sizeof gba, "%s/gba/Game.gba", root);
        make_gba(rom, sizeof rom, "XRCT");
        write_file(gba, rom, sizeof rom);
        CHECK(recomp_pack_rom_identify(gba, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_GBA);
        CHECK(rom_console(gba) == RECOMP_CONSOLE_GBA && rom_console(xbe) == RECOMP_CONSOLE_UNKNOWN);

        /* No pack: the GBA runtime is an interpreter (XBA-157), it runs the game anyway (PACK_NONE). */
        snprintf(out, sizeof out, "%s/gba/stdout-none.txt", root);
        fflush(stdout);
        {
            int saved0 = dup(1);
            FILE *cap0 = fopen(out, "w");
            dup2(fileno(cap0), 1);
            rc = load_rom(files, gba, &runtime_rc, reason, sizeof reason);
            fflush(stdout);
            dup2(saved0, 1);
            close(saved0);
            fclose(cap0);
            cap0 = fopen(out, "r");
            size_t n0 = fread(log, 1, sizeof log - 1, cap0);
            log[n0] = 0;
            fclose(cap0);
            fputs(log, stdout);
        }
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0 && reason[0] == 0);
        CHECK(strstr(log, "PACK_NONE gba XRCT-") != NULL);
        CHECK(strstr(log, "gba runtime ready") != NULL && strstr(log, "pack none") != NULL);
        CHECK(strstr(log, "gba work dir ") != NULL && strstr(log, "/work\n") != NULL);

        snprintf(dir, sizeof dir, "%s/packs/gba-%s", files, key);
        mkdirs(dir);
        snprintf(so, sizeof so, "%s/libtitle.so", dir);
        write_file(so, bytes, sizeof bytes);
        snprintf(text, sizeof text, "{\"format\":1,\"console\":\"gba\",\"title_key\":\"%s\",\"title\":\"Fake GBA\","
                 "\"abi\":\"%s\",\"shell_abi\":1,\"lib\":\"libtitle.so\",\"lib_size\":64}", key, recomp_pack_host_abi());
        snprintf(out, sizeof out, "%s/pack.json", dir);
        write_file(out, text, strlen(text));

        /* stdout to a file around the call: the runtime's line is the check. */
        snprintf(out, sizeof out, "%s/gba/stdout.txt", root);
        fflush(stdout);
        int saved = dup(1);
        FILE *cap = fopen(out, "w");
        dup2(fileno(cap), 1);
        rc = load_rom(files, gba, &runtime_rc, reason, sizeof reason);
        fflush(stdout);
        dup2(saved, 1);
        close(saved);
        fclose(cap);
        cap = fopen(out, "r");
        size_t n = fread(log, 1, sizeof log - 1, cap);
        log[n] = 0;
        fclose(cap);
        fputs(log, stdout);
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0);
        CHECK(strstr(log, "PACK_NONE") == NULL && strstr(log, "PACK_STALE") == NULL);
        CHECK(strstr(log, "gba runtime ready") != NULL && strstr(log, so) != NULL);   /* the pack handed over */
        CHECK(strstr(log, key) != NULL);

        /* A pack the shell refuses (another console, another shell ABI): PACK_STALE, the game runs interpreted. */
        const char *bad[2] = { "\"console\":\"n64\",\"shell_abi\":1", "\"console\":\"gba\",\"shell_abi\":2" };
        for (int i = 0; i < 2; i++) {
            snprintf(text, sizeof text, "{\"format\":1,%s,\"title_key\":\"%s\",\"abi\":\"%s\","
                     "\"lib\":\"libtitle.so\",\"lib_size\":64}", bad[i], key, recomp_pack_host_abi());
            snprintf(out, sizeof out, "%s/pack.json", dir);
            write_file(out, text, strlen(text));
            runtime_rc = 5;
            snprintf(out, sizeof out, "%s/gba/stdout-stale.txt", root);
            fflush(stdout);
            saved = dup(1);
            cap = fopen(out, "w");
            dup2(fileno(cap), 1);
            rc = load_rom(files, gba, &runtime_rc, reason, sizeof reason);
            fflush(stdout);
            dup2(saved, 1);
            close(saved);
            fclose(cap);
            cap = fopen(out, "r");
            n = fread(log, 1, sizeof log - 1, cap);
            log[n] = 0;
            fclose(cap);
            fputs(log, stdout);
            CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0);
            CHECK(strstr(log, "PACK_STALE gba ") != NULL && strstr(log, "running interpreted") != NULL);
            CHECK(strstr(log, i == 0 ? "console n64" : "shell_abi 2") != NULL);
            CHECK(strstr(log, "pack none") != NULL);
        }

        /* An Xbox game is not one: the Xbox loader's. */
        CHECK(load_rom(files, xbe, &runtime_rc, reason, sizeof reason) == RECOMP_PACK_INVALID);
    }

    /* XBA-168: a PS1 disc the same way. Its pack, when the shell accepts it, is handed to ps1_runtime_start (a
     * stand-in here, fake_ps1_runtime.c: the real one checks source_sha256 = the boot EXE's, lib_sha256 and the
     * table, consoles/ps1/tests test_pack.c); without a pack, or with one the shell refuses, it runs interpreted. */
    {
        int (*load_rom)(const char *, const char *, int *, char *, size_t) =
            (int (*)(const char *, const char *, int *, char *, size_t))dlsym(shell, "xba_pack_load_rom");
        char disc[1100], key[64], dir[1200], so[1300], text[1024], out[1300], log[4096];
        unsigned char bytes[64] = { 0 };
        int c = -1, runtime_rc = -1, rc;

        CHECK(load_rom != NULL);
        if (!load_rom)
            return;
        snprintf(disc, sizeof disc, "%s/ps1", root);
        mkdirs(disc);
        snprintf(disc, sizeof disc, "%s/ps1/Game.bin", root);
        make_ps1(disc, 1, "BOOT = cdrom:\\SLUS_005.94;1\r\n");
        CHECK(recomp_pack_rom_identify(disc, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_PS1);
        CHECK(!strcmp(key, "SLUS-00594"));

        snprintf(out, sizeof out, "%s/ps1/stdout-none.txt", root);
        rc = load_rom_logged(load_rom, files, disc, &runtime_rc, reason, sizeof reason, out, log, sizeof log);
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0 && reason[0] == 0);
        CHECK(strstr(log, "PACK_NONE ps1 SLUS-00594") != NULL);
        CHECK(strstr(log, "ps1 runtime ready") != NULL && strstr(log, "pack none") != NULL);
        CHECK(strstr(log, "ps1 work dir ") != NULL && strstr(log, "/work\n") != NULL);

        snprintf(dir, sizeof dir, "%s/packs/ps1-%s", files, key);
        mkdirs(dir);
        snprintf(so, sizeof so, "%s/libtitle.so", dir);
        write_file(so, bytes, sizeof bytes);
        snprintf(text, sizeof text, "{\"format\":1,\"console\":\"ps1\",\"title_key\":\"%s\",\"title\":\"Fake PS1\","
                 "\"abi\":\"%s\",\"shell_abi\":1,\"lib\":\"libtitle.so\",\"lib_size\":64}", key, recomp_pack_host_abi());
        snprintf(out, sizeof out, "%s/pack.json", dir);
        write_file(out, text, strlen(text));
        snprintf(out, sizeof out, "%s/ps1/stdout.txt", root);
        rc = load_rom_logged(load_rom, files, disc, &runtime_rc, reason, sizeof reason, out, log, sizeof log);
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0);
        CHECK(strstr(log, "PACK_NONE") == NULL && strstr(log, "PACK_STALE") == NULL && strstr(log, "IGNORED") == NULL);
        CHECK(strstr(log, "ps1 runtime ready") != NULL && strstr(log, so) != NULL);   /* the pack handed over */

        snprintf(text, sizeof text, "{\"format\":1,\"console\":\"ps1\",\"title_key\":\"%s\",\"abi\":\"%s\","
                 "\"shell_abi\":2,\"lib\":\"libtitle.so\",\"lib_size\":64}", key, recomp_pack_host_abi());
        snprintf(out, sizeof out, "%s/pack.json", dir);
        write_file(out, text, strlen(text));
        runtime_rc = 5;
        snprintf(out, sizeof out, "%s/ps1/stdout-stale.txt", root);
        rc = load_rom_logged(load_rom, files, disc, &runtime_rc, reason, sizeof reason, out, log, sizeof log);
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0);
        CHECK(strstr(log, "PACK_STALE ps1 SLUS-00594 (") != NULL && strstr(log, "shell_abi 2") != NULL);
        CHECK(strstr(log, "pack none") != NULL);
    }

    /* XBA-174: an N64 cartridge the same way. Its pack, when the shell accepts it, is handed to n64_runtime_start (a
     * stand-in here, fake_n64_runtime.c: the real one checks source_sha256 = the ROM's, lib_sha256 and the table,
     * consoles/n64/tests test_pack.c); without a pack, or with one the shell refuses, it runs interpreted. */
    {
        int (*load_rom)(const char *, const char *, int *, char *, size_t) =
            (int (*)(const char *, const char *, int *, char *, size_t))dlsym(shell, "xba_pack_load_rom");
        char cart[1100], key[64], dir[1200], so[1300], text[1024], out[1300], log[4096];
        unsigned char rom[0x1000] = { 0 }, bytes[64] = { 0 };
        int c = -1, runtime_rc = -1, rc;

        CHECK(load_rom != NULL);
        if (!load_rom)
            return;
        snprintf(cart, sizeof cart, "%s/n64", root);
        mkdirs(cart);
        snprintf(cart, sizeof cart, "%s/n64/Game.z64", root);
        rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
        rom[0x10] = 0x63; rom[0x11] = 0x5A; rom[0x12] = 0x2B; rom[0x13] = 0xFF;
        memcpy(rom + 0x3B, "NSME", 4);
        write_file(cart, rom, sizeof rom);
        CHECK(recomp_pack_rom_identify(cart, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_N64);
        CHECK(!strcmp(key, "NSME-635A2BFF"));

        snprintf(out, sizeof out, "%s/n64/stdout-none.txt", root);
        rc = load_rom_logged(load_rom, files, cart, &runtime_rc, reason, sizeof reason, out, log, sizeof log);
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0 && reason[0] == 0);
        CHECK(strstr(log, "PACK_NONE n64 NSME-635A2BFF (not prepared") != NULL);
        CHECK(strstr(log, "n64 runtime ready") != NULL && strstr(log, "pack none") != NULL);
        CHECK(strstr(log, "n64 work dir ") != NULL && strstr(log, "/work\n") != NULL);

        snprintf(dir, sizeof dir, "%s/packs/n64-%s", files, key);
        mkdirs(dir);
        snprintf(so, sizeof so, "%s/libtitle.so", dir);
        write_file(so, bytes, sizeof bytes);
        snprintf(text, sizeof text, "{\"format\":1,\"console\":\"n64\",\"title_key\":\"%s\",\"title\":\"Fake N64\","
                 "\"abi\":\"%s\",\"shell_abi\":1,\"lib\":\"libtitle.so\",\"lib_size\":64}", key, recomp_pack_host_abi());
        snprintf(out, sizeof out, "%s/pack.json", dir);
        write_file(out, text, strlen(text));
        snprintf(out, sizeof out, "%s/n64/stdout.txt", root);
        rc = load_rom_logged(load_rom, files, cart, &runtime_rc, reason, sizeof reason, out, log, sizeof log);
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0);
        CHECK(strstr(log, "PACK_NONE") == NULL && strstr(log, "PACK_STALE") == NULL && strstr(log, "IGNORED") == NULL);
        CHECK(strstr(log, "n64 runtime ready") != NULL && strstr(log, so) != NULL);   /* the pack handed over */

        snprintf(text, sizeof text, "{\"format\":1,\"console\":\"n64\",\"title_key\":\"%s\",\"abi\":\"%s\","
                 "\"shell_abi\":2,\"lib\":\"libtitle.so\",\"lib_size\":64}", key, recomp_pack_host_abi());
        snprintf(out, sizeof out, "%s/pack.json", dir);
        write_file(out, text, strlen(text));
        runtime_rc = 5;
        snprintf(out, sizeof out, "%s/n64/stdout-stale.txt", root);
        rc = load_rom_logged(load_rom, files, cart, &runtime_rc, reason, sizeof reason, out, log, sizeof log);
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0);
        CHECK(strstr(log, "PACK_STALE n64 NSME-635A2BFF (") != NULL && strstr(log, "shell_abi 2") != NULL);
        CHECK(strstr(log, "pack none") != NULL);
    }

    /* XBA-259: a GameCube DOL the same way. No pack: the interpreter runs it (PACK_NONE); a pack the shell accepts
     * is handed to gc_runtime_start (a stand-in here, fake_gc_runtime.c). */
    {
        int (*load_rom)(const char *, const char *, int *, char *, size_t) =
            (int (*)(const char *, const char *, int *, char *, size_t))dlsym(shell, "xba_pack_load_rom");
        char prog[1100], key[64], dir[1200], so[1300], text[1024], out[1300], log[4096];
        unsigned char dol[0x400], bytes[64] = { 0 };
        int c = -1, runtime_rc = -1, rc;

        CHECK(load_rom != NULL);
        if (!load_rom)
            return;
        snprintf(prog, sizeof prog, "%s/gc", root);
        mkdirs(prog);
        snprintf(prog, sizeof prog, "%s/gc/padtest.dol", root);
        make_dol(dol, sizeof dol);
        write_file(prog, dol, sizeof dol);
        CHECK(recomp_pack_rom_identify(prog, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_GC);

        snprintf(out, sizeof out, "%s/gc/stdout-none.txt", root);
        rc = load_rom_logged(load_rom, files, prog, &runtime_rc, reason, sizeof reason, out, log, sizeof log);
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0 && reason[0] == 0);
        CHECK(strstr(log, "PACK_NONE gc DOL-") != NULL && strstr(log, "(not prepared") != NULL);
        CHECK(strstr(log, "gc runtime ready") != NULL && strstr(log, "pack none") != NULL);
        CHECK(strstr(log, "gc work dir ") != NULL && strstr(log, "/work\n") != NULL);

        snprintf(dir, sizeof dir, "%s/packs/gc-%s", files, key);
        mkdirs(dir);
        snprintf(so, sizeof so, "%s/libtitle.so", dir);
        write_file(so, bytes, sizeof bytes);
        snprintf(text, sizeof text, "{\"format\":1,\"console\":\"gc\",\"title_key\":\"%s\",\"title\":\"Pad test\","
                 "\"abi\":\"%s\",\"shell_abi\":1,\"lib\":\"libtitle.so\",\"lib_size\":64}", key, recomp_pack_host_abi());
        snprintf(out, sizeof out, "%s/pack.json", dir);
        write_file(out, text, strlen(text));
        snprintf(out, sizeof out, "%s/gc/stdout.txt", root);
        rc = load_rom_logged(load_rom, files, prog, &runtime_rc, reason, sizeof reason, out, log, sizeof log);
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0);
        CHECK(strstr(log, "PACK_NONE") == NULL && strstr(log, "PACK_STALE") == NULL);
        CHECK(strstr(log, "gc runtime ready") != NULL && strstr(log, so) != NULL);
    }

    /* XBA-280: an Xbox 360 disc image needs its pack: none is PACK_MISSING; a pack is handed to x360_runtime_start
     * (a stand-in, fake_x360_runtime.c) with the pack's default.xex, the image as the disc, the pack's shaders. */
    {
        int (*load_rom)(const char *, const char *, int *, char *, size_t) =
            (int (*)(const char *, const char *, int *, char *, size_t))dlsym(shell, "xba_pack_load_rom");
        char img[1100], key[64], dir[1200], so[1300], text[1024], out[1300], log[4096];
        unsigned char x[0x200], bytes[64] = { 0 };
        int c = -1, runtime_rc = -1, rc;

        CHECK(load_rom != NULL);
        if (!load_rom)
            return;
        make_xex(x, sizeof x, 0x4D5307E6u);
        snprintf(img, sizeof img, "%s/x360", root);
        mkdirs(img);
        snprintf(img, sizeof img, "%s/x360/game.iso", root);
        make_xdvdfs(img, 0, "default.xex", x, sizeof x);
        CHECK(recomp_pack_rom_identify(img, &c, key, sizeof key) == 0 && c == RECOMP_CONSOLE_X360);
        unsetenv("RECOMP_X360_DISC");
        unsetenv("RECOMP_X360_SHADERS");
        snprintf(out, sizeof out, "%s/x360/stdout-none.txt", root);
        rc = load_rom_logged(load_rom, files, img, &runtime_rc, reason, sizeof reason, out, log, sizeof log);
        CHECK(rc == RECOMP_PACK_MISSING && strstr(log, "PACK_MISSING x360 X360-4D5307E6-") != NULL);
        CHECK(strstr(log, "x360 runtime ready") == NULL);

        snprintf(dir, sizeof dir, "%s/packs/x360-%s", files, key);
        mkdirs(dir);
        snprintf(so, sizeof so, "%s/libtitle.so", dir);
        write_file(so, bytes, sizeof bytes);
        snprintf(out, sizeof out, "%s/default.xex", dir);
        write_file(out, x, sizeof x);
        snprintf(text, sizeof text, "{\"format\":1,\"console\":\"x360\",\"title_key\":\"%s\",\"title\":\"T\","
                 "\"abi\":\"%s\",\"shell_abi\":1,\"lib\":\"libtitle.so\",\"lib_size\":64}", key,
                 recomp_pack_host_abi());
        snprintf(out, sizeof out, "%s/pack.json", dir);
        write_file(out, text, strlen(text));
        snprintf(out, sizeof out, "%s/x360/stdout.txt", root);
        rc = load_rom_logged(load_rom, files, img, &runtime_rc, reason, sizeof reason, out, log, sizeof log);
        CHECK(rc == RECOMP_PACK_OK && runtime_rc == 0);
        CHECK(strstr(log, "PACK_LOADED x360 X360-4D5307E6-") != NULL && strstr(log, "x360 runtime ready") != NULL);
        CHECK(strstr(log, "/default.xex, disc ") != NULL && strstr(log, "x360/game.iso") != NULL);
        CHECK(strstr(log, "/shaders)") != NULL && strstr(log, "x360 work dir ") != NULL);
        unsetenv("RECOMP_X360_DISC");
        unsetenv("RECOMP_X360_SHADERS");
    }

    /* A title linked into the shell: its strong symbols, not the forwarders. */
    void *st = dlopen(static_path, RTLD_NOW | RTLD_LOCAL);
    if (!st) { fprintf(stderr, "FAIL dlopen static shell: %s\n", dlerror()); failures++; return; }
    int (*srun)(uint32_t) = (int (*)(uint32_t))dlsym(st, "toolkit_run");
    int (*sframe)(int *) = (int (*)(int *))dlsym(st, "toolkit_pb_frame");
    title_main_fn smain = (title_main_fn)dlsym(st, "recomp_title_main");
    CHECK(srun && sframe && smain);
    if (srun && sframe && smain) {
        CHECK(srun(0x11000) == 0x5711);
        CHECK(sframe(&cursor) == 6 && cursor == 9);
        CHECK(smain(NULL) == 77);
    }
}

int main(int argc, char **argv)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/recomp_pack_XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }

    test_title_id();
    test_parse();
    test_env_force();
    test_images();
    test_validate_and_find();
    test_consoles();
    test_x360();
    if (argc >= 5)
        test_routing(argv[1], argv[2], argv[3], argv[4]);
    else
        printf("routing: no libraries given, skipped\n");

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) { /* best effort */ }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("recomp_pack: all checks passed\n");
    return 0;
}
