/* gc_cheats_test -- GameCube cheats on the real core (XBA-264): the shared engine (recomp_cheats.h, RECOMP_CHEAT_GC)
 * through the runtime's bus (gc_cheat_read32 / gc_cheat_write32), after every field as gc_runtime runs them.
 *
 *   gc_cheats_test              a synthetic program (a loop adding 1 to r3 and storing it): a Gecko 04 write
 *                               patching the add, then a C2 hook replacing it with code in the hook area (the CPU runs
 *                               the hook and branches back), turned off (the instruction back), an AR conditional
 *   gc_cheats_test PADTEST.dol  our libogc pad test: Gecko and AR codes write into the report the program keeps
 *                               (its field counter): the program counts on from the cheat's value
 * Exit 0, 1 on a mismatch, 77 when PADTEST.dol is given but not built. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_core.h"
#include "recomp_cheats.h"

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static uint32_t rd(void *m, uint32_t a)
{
    return gc_cheat_read32(m, a);
}
static void wr(void *m, uint32_t a, uint32_t v)
{
    gc_cheat_write32(m, a, v);
}

static recomp_cheats S;

static void set_cheats(const char *text)
{
    recomp_cheats_free(&S);
    recomp_cheats_init(&S, RECOMP_CHEAT_GC);
    recomp_cheats_parse_text(&S, text);
    for (size_t i = 0; i < S.n; i++)
        CHECK(!S.list[i].error[0], "cheat %s: %s", S.list[i].name, S.list[i].error);
}

static void field(gc_machine *m, const recomp_cheat_bus *b)
{
    CHECK(gc_run_frame(m) == 0, "field: %s", gc_error(m) ? gc_error(m) : "?");
    recomp_cheats_apply(&S, b);
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void test_synthetic(void)
{
    static uint8_t dol[0x100 + 32];
    memset(dol, 0, sizeof dol);
    be32(dol + 0x00, 0x100);
    be32(dol + 0x48, 0x80003100u);
    be32(dol + 0x90, 32);
    be32(dol + 0xE0, 0x80003100u);
    static const uint32_t code[] = {
        0x3C808000u, /* 3100 lis r4, 0x8000 */
        0x38630001u, /* 3104 addi r3, r3, 1 */
        0x90644000u, /* 3108 stw r3, 0x4000(r4) */
        0x4BFFFFF8u, /* 310C b 3104 */
        0x60000000u, 0x60000000u, 0x60000000u, 0x60000000u,
    };
    for (int i = 0; i < 8; i++)
        be32(dol + 0x100 + 4 * i, code[i]);
    char why[160];
    gc_machine *m = gc_create(dol, sizeof dol, why, sizeof why);
    if (!m) {
        printf("FAIL gc_create: %s\n", why);
        failures++;
        return;
    }
    recomp_cheat_bus b = {m, NULL, NULL, rd, wr, 1, NULL, 0, NULL};
    set_cheats("");
    field(m, &b);

    /* a Gecko 04 write over the add: addi r3, r3, 2 -> the counter stays even from a reset */
    set_cheats("[add 2]\nenabled=1\n04003104 38630002\n04004000 00000000\n");
    recomp_cheats_apply(&S, &b);
    gc_run_frame(m);
    uint32_t c = gc_peek32(m, 0x80004000u);
    CHECK(gc_peek32(m, 0x80003104u) == 0x38630002u, "patched instruction %08X", gc_peek32(m, 0x80003104u));
    CHECK(c > 1000 && c % 2 == 0, "counter %u after the patch (want even)", c);

    /* C2: the add replaced by a hook that adds 5 (then branches back to the store) */
    wr(m, 0x80003104u, 0x38630001u);
    set_cheats("[add 5]\nenabled=1\nC2003104 00000001\n38630005 00000000\n");
    recomp_cheats_apply(&S, &b);
    uint32_t br = gc_peek32(m, 0x80003104u);
    CHECK((br & 0xFC000003u) == 0x48000000u, "a branch at the hook: %08X", br);
    CHECK(gc_peek32(m, 0x80001800u) == 0x38630005u, "hook code %08X", gc_peek32(m, 0x80001800u));
    CHECK((gc_peek32(m, 0x80001804u) & 0xFC000003u) == 0x48000000u, "branch back %08X", gc_peek32(m, 0x80001804u));
    wr(m, 0x80004000u, 0); /* the next store is r3 + 5k; r3 itself is whatever it was */
    uint32_t r0 = 0;
    gc_run_frame(m);
    r0 = gc_peek32(m, 0x80004000u);
    gc_run_frame(m);
    uint32_t r1 = gc_peek32(m, 0x80004000u);
    CHECK(r1 > r0 && (r1 - r0) % 5 == 0, "hooked counter %u -> %u (want steps of 5)", r0, r1);
    /* the list emptied (the app's file reloaded with no cheat): the runtime still applies once, the hook goes */
    recomp_cheats_parse_text(&S, "");
    CHECK(S.n == 0 && recomp_cheats_pending(&S), "an emptied list with a hook in place is not pending");
    if (recomp_cheats_pending(&S))
        recomp_cheats_apply(&S, &b);
    CHECK(gc_peek32(m, 0x80003104u) == 0x38630001u, "hook left after the list emptied: %08X",
          gc_peek32(m, 0x80003104u));
    CHECK(!recomp_cheats_pending(&S), "still pending after the hook went");
    /* back on, then turned off: the add comes back */
    set_cheats("[add 5]\nenabled=1\nC2003104 00000001\n38630005 00000000\n");
    recomp_cheats_apply(&S, &b);
    CHECK((gc_peek32(m, 0x80003104u) & 0xFC000003u) == 0x48000000u, "hook back: %08X", gc_peek32(m, 0x80003104u));
    S.list[0].enabled = 0;
    recomp_cheats_apply(&S, &b);
    CHECK(gc_peek32(m, 0x80003104u) == 0x38630001u, "instruction put back: %08X", gc_peek32(m, 0x80003104u));
    gc_run_frame(m);
    uint32_t r2 = gc_peek32(m, 0x80004000u);
    gc_run_frame(m);
    uint32_t r3 = gc_peek32(m, 0x80004000u);
    CHECK(r3 > r2, "the counter runs on %u -> %u", r2, r3);

    /* AR: if the counter word is not 0, write 0x1234 at 0x80005000 (skip 1); a 16-bit fill */
    set_cheats("[ar]\nenabled=1\nformat=ar\n14004000 00000000\n04005000 00001234\n02005010 0001BEEF\n");
    field(m, &b);
    CHECK(gc_peek32(m, 0x80005000u) == 0x1234u, "AR conditional write %08X", gc_peek32(m, 0x80005000u));
    CHECK(gc_peek32(m, 0x80005010u) == 0xBEEFBEEFu, "AR fill %08X", gc_peek32(m, 0x80005010u));
    recomp_cheats_free(&S);
    gc_destroy(m);
}

/* A bus that counts the writes it is asked for (the Gecko write budget). */
static size_t s_writes;
static void wr_count(void *m, uint32_t a, uint32_t v)
{
    s_writes++;
    gc_cheat_write32(m, a, v);
}

/* Malformed and runaway Gecko codes: the engine stays inside the cheat's lines and inside its write budget. */
static void test_gecko_bounds(void)
{
    static uint8_t dol[0x100 + 32];
    memset(dol, 0, sizeof dol);
    be32(dol + 0x00, 0x100);
    be32(dol + 0x48, 0x80003100u);
    be32(dol + 0x90, 32);
    be32(dol + 0xE0, 0x80003100u);
    be32(dol + 0x100, 0x48000000u); /* b . */
    char why[160];
    gc_machine *m = gc_create(dol, sizeof dol, why, sizeof why);
    if (!m) {
        printf("FAIL gc_create: %s\n", why);
        failures++;
        return;
    }
    recomp_cheat_bus b = {m, NULL, NULL, rd, wr, 1, NULL, 0, NULL};
    /* A goto (66, always) over the F0 that ends the checked lines, onto a 06 whose length (0xFFFFFFF0 bytes) was
     * never checked: it read up to 2^29 lines past the cheat. Nothing may be written, nothing read past the end. */
    set_cheats("[jump]\nenabled=1\nformat=gecko\n66000001 00000000\nF0000000 00000000\n06006000 FFFFFFF0\n");
    recomp_cheats_apply(&S, &b);
    CHECK(gc_peek32(m, 0x80006000u) == 0, "a 06 past the F0 wrote %08X", gc_peek32(m, 0x80006000u));
    /* The same onto an 08 (two lines) on the last line */
    set_cheats("[jump08]\nenabled=1\nformat=gecko\n66000001 00000000\nF0000000 00000000\n08006000 12345678\n");
    recomp_cheats_apply(&S, &b);
    CHECK(gc_peek32(m, 0x80006000u) == 0, "an 08 on the last line wrote %08X", gc_peek32(m, 0x80006000u));
    /* A 16-bit fill of 65536 halfwords repeated 65536 times (60 / 62): billions of writes in one frame before the
     * budget; now at most 65536 a frame (each one a read-modify-write of a word on this bus). */
    set_cheats("[runaway]\nenabled=1\nformat=gecko\n6000FFFF 00000000\n02100000 FFFF0001\n62000000 00000000\n");
    b.write32 = wr_count;
    s_writes = 0;
    recomp_cheats_apply(&S, &b);
    CHECK(s_writes > 0 && s_writes <= 2 * 65536, "runaway fill: %zu bus writes in a frame", s_writes);
    CHECK((gc_peek32(m, 0x80100000u) & 0xFFFF) == 0x0001, "the fill wrote while within its budget: %08X",
          gc_peek32(m, 0x80100000u));
    recomp_cheats_free(&S);
    gc_destroy(m);
}

/* padtest's report: magic "XBAPADT1", then the field counter the program increments (gc_padtest.c). */
static uint32_t find_report(gc_machine *m)
{
    for (uint32_t a = 0x80003100u; a < 0x81800000u - 64; a += 4)
        if (gc_peek32(m, a) == 0x58424150u && gc_peek32(m, a + 4) == 0x41445431u)
            return a;
    return 0;
}

static int test_padtest(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skipped: %s not built (tools/consoles/build_homebrew.sh gc)\n", path);
        return 77;
    }
    static uint8_t dol[16 << 20];
    size_t n = fread(dol, 1, sizeof dol, f);
    fclose(f);
    char why[160];
    gc_machine *m = gc_create(dol, n, why, sizeof why);
    if (!m) {
        printf("FAIL %s: %s\n", path, why);
        return 1;
    }
    gc_pad_state p = {true, 0, 128, 128, 128, 128, 0, 0};
    gc_set_pad(m, 0, &p);
    recomp_cheat_bus b = {m, NULL, NULL, rd, wr, 1, NULL, 0, NULL};
    set_cheats("");
    for (int i = 0; i < 60; i++)
        gc_run_frame(m);
    uint32_t rep = find_report(m);
    CHECK(rep != 0, "no padtest report in MEM1");
    if (rep) {
        uint32_t fields = rep + 8;
        char text[512];
        /* Gecko: the counter set to 0x10000 after each field: the program counts on from there */
        snprintf(text, sizeof text, "[fields]\nenabled=1\nformat=gecko\n%08X 00010000\n",
                 0x04000000u | (fields & 0x01FFFFFFu));
        set_cheats(text);
        for (int i = 0; i < 5; i++)
            field(m, &b);
        gc_run_frame(m);
        uint32_t v = gc_peek32(m, fields);
        CHECK(v > 0x10000u && v <= 0x10003u, "padtest counted from the Gecko value: %08X", v);
        /* AR, pointer write through a word we plant: [ptr] + 0 = 0x00200000 */
        uint32_t ptr = 0x80001F00u;
        wr(m, ptr, fields);
        snprintf(text, sizeof text, "[ptr]\nenabled=1\nformat=ar\n%08X 00200000\n", 0x44000000u | (ptr & 0x01FFFFFFu));
        set_cheats(text);
        field(m, &b);
        gc_run_frame(m);
        v = gc_peek32(m, fields);
        CHECK(v > 0x200000u && v <= 0x200003u, "padtest counted from the AR value: %08X", v);
    }
    recomp_cheats_free(&S);
    gc_destroy(m);
    return 0;
}

int main(int argc, char **argv)
{
    recomp_cheats_init(&S, RECOMP_CHEAT_GC);
    int rc = 0;
    if (argc > 1)
        rc = test_padtest(argv[1]);
    else {
        test_synthetic();
        test_gecko_bounds();
    }
    if (rc == 77)
        return 77;
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gc_cheats_test: ok\n");
    return 0;
}
