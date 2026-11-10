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
    test_validate_and_find();
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
