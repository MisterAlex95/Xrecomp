/*
 * Mods on the Xbox runtime (XBA-288, xbox_mods.h): on the synthetic XBE, in a child process per run (the guest
 * window is mapped once per process).
 *
 *   pure        xbox_ModsFramesWide's RECOMP_ASPECT table, xbox_ModsParseWrites' format and refusals
 *   patched     RECOMP_MOD_XBE names a copy with one data byte changed: that byte is in guest memory
 *   other       a copy of another title is refused: the disc's byte stays
 *   wide        RECOMP_MOD_FRAMES=anamorphic-16:9, RECOMP_ASPECT=16:9: the wide write lands; 4:3: it does not;
 *               auto on a desktop: it does not
 */
#include "xbox_memory_layout.h"
#include "xbox_mods.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* No generated code here: the runtime's lookups find nothing. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va);
recomp_func_t recomp_lookup_manual(uint32_t va);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

static int g_fail;

static void check(int ok, const char *what)
{
    printf("  %-70s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        g_fail = 1;
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)(uintptr_t)(xbox_GetMemoryOffset() + (ptrdiff_t)va);
}

static unsigned char *g_xbe;
static size_t g_len;
static uint32_t g_va;       /* a byte of a section with raw data, + 8 */
static uint32_t g_raw;      /* its file offset */

/* The first section with at least 16 bytes of raw data. */
static int find_section(void)
{
    uint32_t base = rd32(g_xbe + 0x104), n = rd32(g_xbe + 0x11C), hdr = rd32(g_xbe + 0x120) - base, i;

    for (i = 0; i < n; i++) {
        const uint8_t *s = g_xbe + hdr + i * 0x38;
        if (rd32(s + 16) >= 16) {
            g_va = rd32(s + 4) + 8;
            g_raw = rd32(s + 12) + 8;
            return 1;
        }
    }
    return 0;
}

static const char *write_copy(const char *dir, const char *name, int other_title)
{
    static char path[2][256];
    char *p = path[other_title ? 1 : 0];
    unsigned char *c = malloc(g_len);
    FILE *f;

    memcpy(c, g_xbe, g_len);
    c[g_raw] ^= 0x5A;
    if (other_title) {
        uint32_t cert = rd32(c + 0x118) - rd32(c + 0x104);
        c[cert + 8] ^= 1;
    }
    snprintf(p, 256, "%s/%s", dir, name);
    f = fopen(p, "wb");
    fwrite(c, 1, g_len, f);
    fclose(f);
    free(c);
    return p;
}

static void env(const char *k, const char *v)
{
    if (v)
        setenv(k, v, 1);
    else
        unsetenv(k);
}

static const char *g_for;       /* RECOMP_MOD_FOR for the next runs (NULL: unset) */

/* One run in a child: env as given (NULL values unset), then init. The child's bits: 1 the patched byte is
 * loaded, 2 the wide write landed, 4 init failed. */
static int run(const char *mod_xbe, const char *frames, const char *aspect, const char *writes)
{
    pid_t pid = fork();
    int st = 0;

    if (pid == 0) {
        int bits = 0;
        env("RECOMP_MOD_XBE", mod_xbe);
        env("RECOMP_MOD_FRAMES", frames);
        env("RECOMP_ASPECT", aspect);
        env("RECOMP_MOD_WIDE_WRITES", writes);
        env("RECOMP_MOD_FOR", g_for);
        if (!xbox_MemoryLayoutInit(g_xbe, g_len))
            _exit(4);
        if (*guest(g_va) == (uint8_t)(g_xbe[g_raw] ^ 0x5A))
            bits |= 1;
        if (rd32(guest(g_va + 4)) == 0xEFBEADDEu)
            bits |= 2;
        _exit(bits);
    }
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 8;
}

int main(int argc, char **argv)
{
    char dir[] = "/tmp/xmodsXXXXXX", writes[64];
    XboxModWrite w[XBOX_MODS_WRITES_MAX];
    const char *patched, *other;
    int r;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("pure\n");
    check(xbox_ModsFramesWide("anamorphic-16:9", "16:9", 0) && xbox_ModsFramesWide("anamorphic-16:9", "169", 0),
          "16:9: wide on a desktop");
    check(!xbox_ModsFramesWide("anamorphic-16:9", "4:3", 1) && !xbox_ModsFramesWide("anamorphic-16:9", "4/3", 1),
          "4:3: never wide, Android included");
    check(xbox_ModsFramesWide("anamorphic-16:9", NULL, 1) && !xbox_ModsFramesWide("anamorphic-16:9", NULL, 0)
          && xbox_ModsFramesWide("anamorphic-16:9", "auto", 1),
          "auto: wide on Android, 4:3 on a desktop");
    check(!xbox_ModsFramesWide(NULL, "16:9", 1) && !xbox_ModsFramesWide("stretch", "16:9", 1),
          "no or unknown frames: never wide");
    r = xbox_ModsParseWrites("00010F88=0000804100001041;20000=01", w, XBOX_MODS_WRITES_MAX);
    check(r == 2 && w[0].va == 0x10F88 && w[0].n == 8 && w[0].bytes[2] == 0x80 && w[1].va == 0x20000
          && w[1].n == 1 && w[1].bytes[0] == 1, "writes: two entries parsed (tools/mods/display.py's format)");
    check(xbox_ModsParseWrites("", w, 8) == 0 && xbox_ModsParseWrites(NULL, w, 8) == 0, "writes: none");
    check(xbox_ModsParseWrites("10=ABC", w, 8) < 0 && xbox_ModsParseWrites("=AB", w, 8) < 0
          && xbox_ModsParseWrites("123456789=AB", w, 8) < 0 && xbox_ModsParseWrites("10:AB", w, 8) < 0
          && xbox_ModsParseWrites("FFFFFFFF=ABCD", w, 8) < 0 && xbox_ModsParseWrites("10=AB;20=CD", w, 1) < 0
          && xbox_ModsParseWrites("10=", w, 8) < 0, "writes: malformed ones refused whole");

    g_xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &g_len);
    if (!g_xbe || !find_section() || !mkdtemp(dir)) {
        printf("no synthetic XBE (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    patched = write_copy(dir, "patched.xbe", 0);
    other = write_copy(dir, "other.xbe", 1);
    snprintf(writes, sizeof writes, "%08X=DEADBEEF", g_va + 4);

    printf("patched XBE (data byte at %08X)\n", g_va);
    check(run(NULL, NULL, NULL, NULL) == 0, "no mod: the disc's byte");
    check(run(patched, NULL, NULL, NULL) == 1, "RECOMP_MOD_XBE: the patched copy's byte is in guest memory");
    check(run(other, NULL, NULL, NULL) == 0, "a copy of another title: refused, the disc's byte");
    check(run("/nonexistent/x.xbe", NULL, NULL, NULL) == 0, "a missing copy: the disc's byte");
    printf("display hint\n");
    check(run(patched, "anamorphic-16:9", "16:9", writes) == 3, "16:9: the wide write lands (and the patched byte)");
    check(run(NULL, "anamorphic-16:9", "4:3", writes) == 0, "4:3: no wide write");
    check(run(NULL, "anamorphic-16:9", NULL, writes) == 0, "auto on a desktop: no wide write");
    check(run(NULL, NULL, "16:9", writes) == 0, "no frames hint: no wide write, whatever the setting");
    check(run(NULL, "anamorphic-16:9", "16:9", "00001000=DEADBEEF") == 0, "a write outside the image: skipped");

    printf("RECOMP_MOD_FOR (the disc's other images share the pack's options)\n");
    {
        static char key[40], other_key[40];
        snprintf(key, sizeof key, "%zu-%08X", g_len, xbox_ModsCrc32(g_xbe, g_len));
        snprintf(other_key, sizeof other_key, "%zu-%08X", g_len, xbox_ModsCrc32(g_xbe, g_len) ^ 1u);
        check(xbox_ModsCrc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "CRC32 check value (zlib's)");
        check(xbox_ModsForThis(g_xbe, g_len, key) && xbox_ModsForThis(g_xbe, g_len, NULL)
              && !xbox_ModsForThis(g_xbe, g_len, other_key), "the key matches this XBE only");
        g_for = key;
        check(run(patched, "anamorphic-16:9", "16:9", writes) == 3, "made for this XBE: patched copy and wide write");
        g_for = other_key;
        check(run(patched, "anamorphic-16:9", "16:9", writes) == 0,
              "made for another XBE of the title: neither the patched copy nor the wide write");
        g_for = NULL;
    }

    remove(patched);
    remove(other);
    rmdir(dir);
    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail;
}
