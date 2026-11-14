/* gba_pack (XBA-157 / XBA-158): a GBA title pack, the runtime's side. SHA-256, the pack.json checks, the library's
 * table, gba_pack_load's outcome for a valid / stale (another ROM, another shell ABI, a damaged library, a table of
 * another ABI) / missing pack -- the machine runs in every case -- and the discovered file the next Prepare reads.
 *
 *   gba_pack_test <fake_gba_pack.so> */
#define _GNU_SOURCE
#include "gba_core.h"
#include "gba_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                      \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void make_rom(uint8_t *rom, size_t n)
{
    uint8_t sum = 0;
    memset(rom, 0, n);
    rom[0] = 0xFE, rom[1] = 0xFF, rom[2] = 0xFF, rom[3] = 0xEA;   /* b . */
    memcpy(rom + 0xA0, "PACKTEST    ", 12);
    memcpy(rom + 0xAC, "XPCK", 4);
    rom[0xB2] = 0x96;
    for (int i = 0xA0; i <= 0xBC; i++)
        sum = (uint8_t)(sum - rom[i]);
    rom[0xBD] = (uint8_t)(sum - 0x19);
}

static void copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    char buf[65536];
    size_t r;
    CHECK(a && b);
    while (a && b && (r = fread(buf, 1, sizeof buf, a)) > 0)
        fwrite(buf, 1, r, b);
    if (a)
        fclose(a);
    if (b)
        fclose(b);
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *read_text(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f)
        fclose(f);
    buf[n] = 0;
    return buf;
}

/* A pack folder dir with lib copied in as libtitle.so and a pack.json; the fields can be spoilt one at a time. */
static void make_pack(const char *dir, const char *lib, const char *key, const char *rom_sha, int shell_abi,
                      const char *abi, int spoil_sha)
{
    char so[1200], json[1200], hex[65], text[2048];
    struct stat st;
    mkdir(dir, 0755);
    snprintf(so, sizeof so, "%s/libtitle.so", dir);
    copy_file(lib, so);
    CHECK(gba_pack_sha256_file(so, hex) == 0 && stat(so, &st) == 0);
    if (spoil_sha)
        hex[0] = hex[0] == '0' ? '1' : '0';
    snprintf(text, sizeof text,
             "{\"format\": 1, \"console\": \"gba\", \"title_key\": \"%s\", \"title\": \"PACKTEST \\\"x\\\"\",\n"
             " \"rom_sha256\": \"%s\", \"abi\": \"%s\", \"shell_abi\": %d, \"toolkit\": \"t\",\n"
             " \"prepare\": {\"steps_s\": {\"lift\": 1.0}, \"jobs\": 2, \"lib\": \"nested.so\"},\n"
             " \"lib\": \"libtitle.so\", \"lib_sha256\": \"%s\", \"lib_size\": %lld,\n"
             " \"entry\": \"gba_recomp_title_table\", \"discovered\": 0}\n",
             key, rom_sha, abi, shell_abi, hex, (long long)st.st_size);
    snprintf(json, sizeof json, "%s/pack.json", dir);
    write_text(json, text);
}

static void test_sha256(void)
{
    uint8_t sum[32];
    char hex[65];
    gba_pack_sha256("abc", 3, sum);
    gba_pack_hex(sum, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    gba_pack_sha256("", 0, sum);
    gba_pack_hex(sum, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    static char big[1000000];
    memset(big, 'a', sizeof big);
    gba_pack_sha256(big, sizeof big, sum);
    gba_pack_hex(sum, hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static void test_discovered(const char *root)
{
    char path[1200];
    size_t added = 99;
    snprintf(path, sizeof path, "%s/work/gba-XPCK-1/discovered.txt", root);
    uint32_t a[] = {0x08000101u, 0x080001F0u, 0x08000101u};
    CHECK(gba_pack_write_discovered(path, a, 3, &added) == 0 && added == 2);   /* directories made */
    CHECK(!strcmp(read_text(path), "0x08000101\n0x080001F0\n"));
    struct stat st1, st2;
    stat(path, &st1);
    CHECK(gba_pack_write_discovered(path, a, 3, &added) == 0 && added == 0);  /* nothing new: untouched */
    stat(path, &st2);
    CHECK(st1.st_ino == st2.st_ino);
    write_text(path, "# kept by hand\n0x08000101\n\n80000F1\n");
    uint32_t b[] = {0x08000000u};
    CHECK(gba_pack_write_discovered(path, b, 1, &added) == 0 && added == 1);
    CHECK(!strcmp(read_text(path), "0x08000000\n0x080000F1\n0x08000101\n"));
    CHECK(gba_pack_write_discovered(NULL, b, 1, &added) == -1);
}

int main(int argc, char **argv)
{
    char root[] = "/tmp/gba_pack_testXXXXXX", dir[1200], lib[1300], why[512], hex[65], json[1300];
    static uint8_t rom[4096];
    uint8_t sum[32];
    const char *key = "XPCK-0000ABCD";
    gba_pack p;

    if (argc < 2) {
        fprintf(stderr, "usage: gba_pack_test <fake_gba_pack.so>\n");
        return 2;
    }
    const char *fake = argv[1];
    CHECK(mkdtemp(root) != NULL);
    test_sha256();
    test_discovered(root);
    CHECK(strlen(gba_pack_host_abi()) > 0 && strcmp(gba_pack_host_abi(), "unknown") != 0);

    make_rom(rom, sizeof rom);
    gba_pack_sha256(rom, sizeof rom, sum);
    gba_pack_hex(sum, hex);
    setenv("FAKE_PACK_ROM_SHA", hex, 1);
    gba_machine *m = gba_create(rom, sizeof rom, NULL, 0);
    CHECK(m != NULL);
    const char *abi = gba_pack_host_abi();

    /* Valid. */
    snprintf(dir, sizeof dir, "%s/gba-%s", root, key);
    make_pack(dir, fake, key, hex, 1, abi, 0);
    CHECK(gba_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == GBA_PACK_LOADED);
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    CHECK(!strcmp(lib, json));
    CHECK(gba_pack_load(m, lib, key, hex, &p) == GBA_PACK_LOADED);
    CHECK(p.table && p.table->count == 2 && p.table->entries[1].flags == GBA_RECOMP_THUMB);
    p.table->entries[0].fn(m);   /* calls gba_set_keys, resolved against this program */
    gba_pack_close(&p);
    CHECK(p.handle == NULL);
    /* shell_abi 0: not checked. */
    CHECK(gba_pack_check(dir, key, hex, 0, lib, sizeof lib, why, sizeof why) == GBA_PACK_LOADED);

    /* Missing. */
    CHECK(gba_pack_load(m, NULL, key, hex, &p) == GBA_PACK_NONE && p.handle == NULL);
    CHECK(gba_pack_load(m, "", key, hex, &p) == GBA_PACK_NONE);
    snprintf(json, sizeof json, "%s/none", root);
    CHECK(gba_pack_check(json, key, hex, 1, lib, sizeof lib, why, sizeof why) == GBA_PACK_NONE);
    snprintf(json, sizeof json, "%s/none/libtitle.so", root);
    CHECK(gba_pack_load(m, json, key, hex, &p) == GBA_PACK_STALE && p.handle == NULL);

    /* Stale: another ROM (pack.json, then a table that says so), another key, another shell ABI, another host, a
     * damaged library, a table of another ABI. */
    char other[65];
    memcpy(other, hex, 65);
    other[5] = other[5] == 'a' ? 'b' : 'a';
    CHECK(gba_pack_check(dir, key, other, 1, lib, sizeof lib, why, sizeof why) == GBA_PACK_STALE
          && strstr(why, "another ROM"));
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    CHECK(gba_pack_load(m, json, key, other, &p) == GBA_PACK_STALE && p.handle == NULL);
    CHECK(gba_pack_check(dir, "XPCK-00000000", hex, 1, lib, sizeof lib, why, sizeof why) == GBA_PACK_STALE
          && strstr(why, "title_key"));
    CHECK(gba_pack_check(dir, key, hex, 2, lib, sizeof lib, why, sizeof why) == GBA_PACK_STALE
          && strstr(why, "shell_abi 1, this shell is 2"));

    snprintf(dir, sizeof dir, "%s/abi2", root);
    make_pack(dir, fake, key, hex, 2, abi, 0);
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    CHECK(gba_pack_load(m, json, key, hex, &p) == GBA_PACK_STALE);   /* the runtime's own shell ABI is 1 */

    snprintf(dir, sizeof dir, "%s/host", root);
    make_pack(dir, fake, key, hex, 1, "mips", 0);
    CHECK(gba_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == GBA_PACK_STALE && strstr(why, "abi"));

    snprintf(dir, sizeof dir, "%s/damaged", root);
    make_pack(dir, fake, key, hex, 1, abi, 1);
    CHECK(gba_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == GBA_PACK_STALE
          && strstr(why, "sha256"));
    snprintf(json, sizeof json, "%s/pack.json", dir);
    write_text(json, "{\"format\": 1, \"console\": \"gba\"");
    CHECK(gba_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == GBA_PACK_STALE);
    write_text(json, "{\"format\": 1, \"console\": \"gba\"}");
    CHECK(gba_pack_check(dir, key, hex, 1, lib, sizeof lib, why, sizeof why) == GBA_PACK_STALE
          && strstr(why, "lacks"));

    snprintf(dir, sizeof dir, "%s/gba-%s", root, key);
    snprintf(json, sizeof json, "%s/libtitle.so", dir);
    setenv("FAKE_PACK_ABI", "99", 1);
    CHECK(gba_pack_load(m, json, key, hex, &p) == GBA_PACK_STALE && p.handle == NULL);
    CHECK(gba_pack_open(json, hex, &p, why, sizeof why) == -1 && strstr(why, "abi 99"));
    unsetenv("FAKE_PACK_ABI");

    /* A bare library (no pack.json): the table's own checks only. */
    snprintf(json, sizeof json, "%s/bare.so", root);
    copy_file(fake, json);
    CHECK(gba_pack_load(m, json, key, hex, &p) == GBA_PACK_LOADED);
    gba_pack_close(&p);
    CHECK(gba_pack_load(m, json, key, other, &p) == GBA_PACK_STALE);

    /* Whatever the pack, the game runs. */
    static uint16_t fb[GBA_SCREEN_W * GBA_SCREEN_H];
    gba_run_frame(m, fb);
    gba_run_frame(m, fb);
    snprintf(json, sizeof json, "%s/work/disc.txt", root);
    CHECK(gba_pack_save_missed(m, json) >= 0);
    gba_destroy(m);

    char cmd[1300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", root);
    if (failures)
        fprintf(stderr, "gba_pack: %d check(s) failed\n", failures);
    else
        printf("gba_pack: all checks passed\n");
    return failures ? 1 : 0;
}
