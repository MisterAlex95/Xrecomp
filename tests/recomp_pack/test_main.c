/* A title pack, the loader's side (recomp_pack.h, pack_loader.h): title ID,
 * pack.json, checks, and the routing of a dlopened pack's symbols through the
 * shell's weak forwarders. See CMakeLists.txt. */
#define _GNU_SOURCE
#include "recomp_pack.h"

#include <dlfcn.h>
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
                    "\"bad.xbe\":\"../x.so\",\"gone.xbe\":\"libimage-3.so\"}}";

    CHECK(recomp_pack_parse(j, strlen(j), &m, err, sizeof err) == 0);
    CHECK(m.images_n == 3 && m.images_dropped == 0);
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

static void test_consoles(void)
{
    char path[1024], path2[1024], key[64], name[64];
    unsigned char rom[0x400], n64[0x1000], v[0x1000];
    int c;

    CHECK(recomp_pack_console_id(NULL) == RECOMP_CONSOLE_XBOX && recomp_pack_console_id("") == RECOMP_CONSOLE_XBOX);
    CHECK(recomp_pack_console_id("gba") == RECOMP_CONSOLE_GBA && recomp_pack_console_id("ps1") == RECOMP_CONSOLE_PS1);
    CHECK(recomp_pack_console_id("n64") == RECOMP_CONSOLE_N64 && recomp_pack_console_id("xbox") == RECOMP_CONSOLE_XBOX);
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
                 "\"lib_size\":%ld,\"images\":{\"Game2.xbe\":\"libimage-1.so\"}}", recomp_pack_host_abi(), n);
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
                CHECK(!strcmp(text, "default.xbe\tdefault.xbe\nGame2.xbe\tGame2.xbe\n"));
            }
        }
        CHECK(set_next && set_next("default.xbe") == 0 && access(next, F_OK) == 0);
        unsetenv("RECOMP_IMAGE");
        unsetenv("RECOMP_IMAGE_MAP");
    }

    /* XBA-147: a game of another console goes to that console's runtime. A
     * fake gba pack (any library bytes: the stub runtime does not load it
     * yet) routes to gba_runtime_start, which logs "gba runtime ready". */
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

        CHECK(load_rom(files, gba, &runtime_rc, reason, sizeof reason) == RECOMP_PACK_MISSING);
        CHECK(strstr(reason, "gba-XRCT-") == reason);

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
        CHECK(strstr(log, "PACK_LOADED gba ") != NULL);
        CHECK(strstr(log, "gba runtime ready") != NULL);
        CHECK(strstr(log, key) != NULL);

        /* The pack says another console: refused, no runtime runs. */
        snprintf(text, sizeof text, "{\"format\":1,\"console\":\"n64\",\"title_key\":\"%s\",\"abi\":\"%s\","
                 "\"shell_abi\":1,\"lib\":\"libtitle.so\",\"lib_size\":64}", key, recomp_pack_host_abi());
        snprintf(out, sizeof out, "%s/pack.json", dir);
        write_file(out, text, strlen(text));
        runtime_rc = 5;
        CHECK(load_rom(files, gba, &runtime_rc, reason, sizeof reason) == RECOMP_PACK_INVALID);
        CHECK(strstr(reason, "console n64") != NULL && runtime_rc == -1);

        /* An Xbox game is not one: the Xbox loader's. */
        CHECK(load_rom(files, xbe, &runtime_rc, reason, sizeof reason) == RECOMP_PACK_INVALID);
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
