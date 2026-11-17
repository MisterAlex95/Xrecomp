/* gc_state_test -- the GameCube save-state format (gc_state.c, XBA-264) on a synthetic program (no corpus): a round
 * trip of every section's data, the title key, the format version, truncated / damaged blobs (refused whole, the
 * machine untouched), a section this build does not know (skipped), an optional section missing (kept), a required
 * one missing, a section newer than this build; and the cheats' memory access (gc_cheat_write32: big-endian, MEM1
 * only, lifted code invalidated where it writes). The homebrew round trips are gc_state_* (run_state.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_internal.h"
#include "gx/gx_internal.h"

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

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* A DOL with one text section at 0x80003100: a loop that counts in r3 and stores it at 0x80004000. */
static gc_machine *make(void)
{
    static uint8_t dol[0x100 + 32];
    memset(dol, 0, sizeof dol);
    be32(dol + 0x00, 0x100);       /* text 0: file offset */
    be32(dol + 0x48, 0x80003100u); /* address */
    be32(dol + 0x90, 32);          /* size */
    be32(dol + 0xE0, 0x80003100u); /* entry */
    static const uint32_t code[] = {
        0x3C808000u, /* lis r4, 0x8000 */
        0x38630001u, /* addi r3, r3, 1 */
        0x90644000u, /* stw r3, 0x4000(r4) */
        0x4BFFFFF8u, /* b -8 */
        0x60000000u, 0x60000000u, 0x60000000u, 0x60000000u,
    };
    for (int i = 0; i < 8; i++)
        be32(dol + 0x100 + 4 * i, code[i]);
    char why[160];
    gc_machine *m = gc_create(dol, sizeof dol, why, sizeof why);
    if (!m)
        printf("FAIL gc_create: %s\n", why);
    return m;
}

static uint8_t *save(gc_machine *m, const char *key, size_t *n)
{
    size_t cap = gc_state_size(m);
    uint8_t *b = malloc(cap);
    *n = b ? gc_state_save(m, key, b, cap) : 0;
    CHECK(*n == cap, "save: %zu bytes, size said %zu", *n, cap);
    return b;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Offset of a section's header in a blob (header 72 bytes; sections: tag, version, length, bytes), or 0. */
static size_t find(const uint8_t *b, size_t n, const char *tag)
{
    size_t off = 72;
    while (off + 12 <= n) {
        if (!memcmp(b + off, tag, 4))
            return off;
        off += 12 + get32(b + off + 8);
    }
    return 0;
}

/* The blob without one section (count adjusted). */
static uint8_t *without(const uint8_t *b, size_t n, const char *tag, size_t *out_n)
{
    size_t at = find(b, n, tag), len = 12 + get32(b + at + 8);
    uint8_t *o = malloc(n);
    memcpy(o, b, at);
    memcpy(o + at, b + at + len, n - at - len);
    put32(o + 64, get32(b + 64) - 1);
    *out_n = n - len;
    return o;
}

static void test_round_trip(void)
{
    gc_machine *m = make();
    if (!m)
        return;
    for (int f = 0; f < 3; f++)
        CHECK(gc_run_frame(m) == 0, "field %d: %s", f, gc_error(m));
    uint32_t count = gc_peek32(m, 0x80004000u);
    CHECK(count > 1000, "the program counted %u", count);
    /* Values in every section */
    m->h.cpu.ps1[7] = -2.5;
    m->h.cpu.spr[912 + 3] = 0x00070007u; /* GQR3 */
    m->h.cpu.sr[5] = 0x12345678u;
    m->lc[100] = 0xAB;
    m->aram[0x123456] = 0xCD;
    m->vi[0x24] = 0x2828;
    m->pad[2].connected = true;
    m->pad[2].stick_x = 77;
    m->gx->bp[0x41] = 0x00ABCDEFu;
    m->gx->xf.f[12] = 3.25f;
    m->gx->tmem[0x80000] = 0x5A;
    m->gx->creg[1][2] = -300;
    m->gx->cmd_len = 3;
    memcpy(m->gx->cmd, "\x61\x41\x00", 3);
    gc_cpu cpu = m->h.cpu;
    uint64_t now = m->h.now, frames = gc_frames(m);
    size_t n;
    uint8_t *b = save(m, "DOL-12345678", &n);
    printf("state: %zu bytes\n", n);

    /* Change everything, load, compare */
    for (int f = 0; f < 2; f++)
        gc_run_frame(m);
    m->h.cpu.ps1[7] = 1;
    m->h.cpu.spr[915] = 0;
    m->h.cpu.sr[5] = 0;
    m->lc[100] = 0;
    m->aram[0x123456] = 0;
    m->aram[0x200000] = 0x11; /* a page the state does not hold: cleared by the load */
    m->vi[0x24] = 0;
    m->pad[2].connected = false;
    m->gx->bp[0x41] = 0;
    m->gx->xf.f[12] = 0;
    m->gx->tmem[0x80000] = 0;
    m->gx->tmem[0x90000] = 9;
    m->gx->creg[1][2] = 0;
    m->gx->cmd_len = 0;
    uint32_t epoch = m->gx->tex_epoch;
    char why[200] = "";
    CHECK(gc_state_load(m, "DOL-12345678", b, n, why, sizeof why) == GC_STATE_OK, "load: %s", why);
    CHECK(!memcmp(&m->h.cpu, &cpu, sizeof cpu), "CPU registers differ after the load");
    CHECK(m->h.now == now && gc_frames(m) == frames, "clock %llu / %llu", (unsigned long long)m->h.now,
          (unsigned long long)now);
    CHECK(gc_peek32(m, 0x80004000u) == count, "MEM1: %u, %u saved", gc_peek32(m, 0x80004000u), count);
    CHECK(m->lc[100] == 0xAB, "locked cache");
    CHECK(m->aram[0x123456] == 0xCD && m->aram[0x200000] == 0, "ARAM");
    CHECK(m->vi[0x24] == 0x2828, "VI");
    CHECK(m->pad[2].connected && m->pad[2].stick_x == 77, "pads");
    CHECK(m->gx->bp[0x41] == 0x00ABCDEFu && m->gx->xf.f[12] == 3.25f && m->gx->creg[1][2] == -300, "GX registers");
    CHECK(m->gx->tmem[0x80000] == 0x5A && m->gx->tmem[0x90000] == 0, "TMEM");
    CHECK(m->gx->cmd_len == 3 && !memcmp(m->gx->cmd, "\x61\x41\x00", 3), "GX staging bytes");
    CHECK(m->gx->tex_epoch != epoch, "the texture cache is checked again");
    /* The same state again, byte for byte */
    size_t n2;
    uint8_t *b2 = save(m, "DOL-12345678", &n2);
    CHECK(n2 == n && !memcmp(b, b2, n), "a loaded state saves the same bytes");
    free(b2);
    /* And it runs on as before */
    CHECK(gc_run_frame(m) == 0, "a field after the load: %s", gc_error(m));
    CHECK(gc_peek32(m, 0x80004000u) > count, "the program counts on");
    free(b);
    gc_destroy(m);
}

static void test_refused(void)
{
    gc_machine *m = make();
    if (!m)
        return;
    gc_run_frame(m);
    size_t n;
    uint8_t *b = save(m, "DOL-AAAAAAAA", &n);
    uint8_t *c = malloc(n);
    char why[200];
    uint32_t mark = gc_peek32(m, 0x80004000u);
    gc_run_frame(m);
    uint32_t now = gc_peek32(m, 0x80004000u);

    why[0] = 0;
    CHECK(gc_state_load(m, "DOL-BBBBBBBB", b, n, why, sizeof why) == GC_STATE_EGAME, "another game's state");
    CHECK(strstr(why, "another game") != NULL, "why: %s", why);
    CHECK(gc_state_check(m, NULL, b, n, why, sizeof why) == GC_STATE_OK, "no key: not checked (%s)", why);

    memcpy(c, b, n);
    put32(c + 12, GC_STATE_VERSION + 1);
    CHECK(gc_state_load(m, "DOL-AAAAAAAA", c, n, why, sizeof why) == GC_STATE_EVERSION, "another version");
    CHECK(strstr(why, "version") != NULL, "why: %s", why);

    memcpy(c, b, n);
    memcpy(c + 8, "n64", 4);
    CHECK(gc_state_load(m, NULL, c, n, why, sizeof why) == GC_STATE_EGAME, "another console");

    memcpy(c, b, n);
    c[0] = 'Y';
    CHECK(gc_state_load(m, NULL, c, n, why, sizeof why) == GC_STATE_ECORRUPT, "not a state");
    CHECK(gc_state_load(m, NULL, b, n / 2, why, sizeof why) == GC_STATE_ECORRUPT, "truncated");
    CHECK(gc_state_load(m, NULL, b, 20, why, sizeof why) == GC_STATE_ECORRUPT, "header only");

    /* MEM1's page bitmap no longer matches its length: refused before anything changes */
    memcpy(c, b, n);
    size_t mem = find(c, n, "MEM1");
    CHECK(mem != 0, "MEM1 section");
    c[mem + 12] ^= 0x80; /* page 7: stored <-> not stored */
    CHECK(gc_state_load(m, NULL, c, n, why, sizeof why) == GC_STATE_ECORRUPT, "damaged MEM1");
    CHECK(strstr(why, "MEM1") != NULL, "why: %s", why);
    CHECK(gc_peek32(m, 0x80004000u) == now, "a refused state changed nothing");

    /* A newer section version */
    memcpy(c, b, n);
    size_t cpu = find(c, n, "CPU ");
    put32(c + cpu + 4, 99);
    CHECK(gc_state_load(m, NULL, c, n, why, sizeof why) == GC_STATE_EVERSION, "newer CPU section");

    /* A fixed section of another size */
    memcpy(c, b, n);
    size_t hw = find(c, n, "HW  ");
    put32(c + hw + 8, get32(c + hw + 8) - 4); /* shorter: the rest shifts, so the next tag is off too */
    CHECK(gc_state_load(m, NULL, c, n, why, sizeof why) != GC_STATE_OK, "HW of another size");

    /* A required section missing */
    size_t wn;
    uint8_t *w = without(b, n, "MEM1", &wn);
    CHECK(gc_state_load(m, NULL, w, wn, why, sizeof why) == GC_STATE_ECORRUPT, "no MEM1");
    CHECK(strstr(why, "missing") != NULL, "why: %s", why);
    free(w);

    /* An optional section missing: loads, ARAM kept as it was */
    m->aram[5] = 0x77;
    w = without(b, n, "ARAM", &wn);
    CHECK(gc_state_load(m, NULL, w, wn, why, sizeof why) == GC_STATE_OK, "no ARAM: %s", why);
    CHECK(m->aram[5] == 0x77, "ARAM kept");
    CHECK(gc_peek32(m, 0x80004000u) == mark, "MEM1 loaded");
    free(w);

    /* A section this build does not know: skipped */
    uint8_t *x = malloc(n + 12 + 5);
    memcpy(x, b, n);
    memcpy(x + n, "ZZZZ", 4);
    put32(x + n + 4, 1);
    put32(x + n + 8, 5);
    memcpy(x + n + 12, "hello", 5);
    put32(x + 64, get32(x + 64) + 1);
    gc_run_frame(m);
    CHECK(gc_state_load(m, NULL, x, n + 17, why, sizeof why) == GC_STATE_OK, "an unknown section: %s", why);
    CHECK(gc_peek32(m, 0x80004000u) == mark, "loaded");
    free(x);

    /* The EXI and DSP hooks are there (empty until their devices fill them) */
    CHECK(find(b, n, "EXI ") && find(b, n, "DSP "), "EXI / DSP sections");
    free(b);
    free(c);
    gc_destroy(m);
}

/* gc_cheat_write32: big-endian, MEM1 only, invalidates the span it writes into. */
static void dummy_fn(gc_machine *m)
{
    (void)m;
}

static void test_cheat_bus(void)
{
    gc_machine *m = make();
    if (!m)
        return;
    static const gc_recomp_entry entries[] = {{0x80003100u, 0, dummy_fn}};
    static const gc_recomp_span spans[] = {{0x80003100u, 0x80003120u, 0}};
    static const gc_recomp_table t = {GC_RECOMP_ABI, 1, entries, 1, spans, ""};
    CHECK(gc_attach_recomp(m, &t) == 0, "attach");
    gc_cheat_write32(m, 0x80005000u, 0x11223344u);
    CHECK(m->h.ram[0x5000] == 0x11 && m->h.ram[0x5003] == 0x44, "big-endian write");
    CHECK(gc_cheat_read32(m, 0xC0005000u) == 0x11223344u, "uncached mirror reads the same");
    CHECK(gc_cheat_read32(m, 0x81800000u) == 0, "past MEM1 reads 0");
    gc_cheat_write32(m, 0x81800000u, 1); /* dropped */
    m->span_state[0] = 1;
    gc_cheat_write32(m, 0x80003104u, 0x38630001u); /* the same word: no write, the span stays checked */
    CHECK(m->span_state[0] == 1, "an unchanged word does not invalidate");
    gc_cheat_write32(m, 0x80003104u, 0x38630002u); /* addi r3, r3, 2 */
    CHECK(m->span_state[0] == 0, "a patched word invalidates the span");
    gc_attach_recomp(m, NULL);
    gc_destroy(m);
}

int main(void)
{
    test_round_trip();
    test_refused();
    test_cheat_bus();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gc_state_test: ok\n");
    return 0;
}
