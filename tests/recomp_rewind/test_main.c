/* recomp_rewind (XBA-194): the XOR-delta codec, the ring and its memory cap, the settings, the runtimes' capture
 * schedule and steps, the round trip check on a fake machine. */
#define _POSIX_C_SOURCE 200809L
#include "recomp_rewind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static uint32_t s_rng = 12345;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 8;
}

/* Encodes older against newer, applies it to a copy of newer: older must come back. */
static int round_trip(const uint8_t *older, size_t on, const uint8_t *newer, size_t nn, size_t *delta_n)
{
    size_t cap = (on > nn ? on : nn) + 16;
    uint8_t *d = malloc(recomp_rewind_delta_bound(on)), *buf = malloc(cap);
    size_t dn = recomp_rewind_delta_encode(older, on, newer, nn, d);
    int ok = dn <= recomp_rewind_delta_bound(on) && recomp_rewind_delta_size(d, dn) == on;
    memcpy(buf, newer, nn);
    size_t n = nn;
    ok = ok && recomp_rewind_delta_apply(buf, &n, cap, d, dn) == 0 && n == on && memcmp(buf, older, on) == 0;
    if (delta_n)
        *delta_n = dn;
    free(d);
    free(buf);
    return ok;
}

static void test_codec(void)
{
    enum { N = 70000 };
    uint8_t *a = malloc(N), *b = malloc(N);
    for (size_t i = 0; i < N; i++)
        a[i] = (uint8_t)rnd();
    memcpy(b, a, N);
    size_t dn = 0;

    /* The same: a few bytes. */
    CHECK(round_trip(a, N, b, N, &dn));
    CHECK(dn < 16);
    /* A few scattered changes: small; a page changed: about its size. */
    for (int k = 0; k < 40; k++)
        b[rnd() % N] ^= 0x5A;
    CHECK(round_trip(a, N, b, N, &dn));
    CHECK(dn < 40 * 12);
    memcpy(b, a, N);
    for (size_t i = 8192; i < 8192 + 4096; i++)
        b[i] = (uint8_t)rnd();
    CHECK(round_trip(a, N, b, N, &dn));
    CHECK(dn > 4000 && dn < 4200);
    /* Everything different: the bound holds. */
    for (size_t i = 0; i < N; i++)
        b[i] = (uint8_t)~a[i];
    CHECK(round_trip(a, N, b, N, &dn));
    /* The worst case for the coder: one literal, then LIT_END_ZEROS zeros, again and again. */
    memcpy(b, a, N);
    for (size_t i = 0; i < N; i += 9)
        b[i] ^= 1;
    CHECK(round_trip(a, N, b, N, &dn));
    /* Sizes that change, both ways (an N64 state's zero pages), and the edges. */
    CHECK(round_trip(a, N, b, N - 1000, NULL));
    CHECK(round_trip(a, N - 1000, b, N, NULL));
    CHECK(round_trip(a, 1, b, N, NULL));
    CHECK(round_trip(a, N, b, 1, NULL));
    for (size_t n = 1; n < 40; n++)
        for (size_t m = 1; m < 40; m += 3)
            CHECK(round_trip(a + 100, n, b + 7, m, NULL));
    /* Zero tails: the newer capture's missing bytes count as zero. */
    memset(a + N - 500, 0, 500);
    CHECK(round_trip(a, N, a, N - 500, &dn));
    CHECK(dn < 16);

    /* Refusals: a truncated delta, an older capture that does not fit, garbage. */
    uint8_t *d = malloc(recomp_rewind_delta_bound(N));
    for (size_t i = 0; i < N; i++)
        b[i] = (uint8_t)(a[i] ^ (i % 3 == 0));
    dn = recomp_rewind_delta_encode(a, N, b, N, d);
    size_t n = N;
    CHECK(recomp_rewind_delta_apply(b, &n, N, d, dn - 1) == -1);
    n = N;
    CHECK(recomp_rewind_delta_apply(b, &n, N - 1, d, dn) == -1);
    uint8_t junk[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    n = N;
    CHECK(recomp_rewind_delta_apply(b, &n, N, junk, sizeof junk) == -1);
    CHECK(recomp_rewind_delta_size(junk, sizeof junk) == 0);
    free(d);
    free(a);
    free(b);
}

/* A fake capture stream: a "machine" of n bytes changing a little at each step. */
static void step_machine(uint8_t *mem, size_t n, int changes)
{
    for (int k = 0; k < changes; k++)
        mem[rnd() % n] = (uint8_t)rnd();
}

static void test_ring(void)
{
    enum { N = 65536, CAPS = 300 };
    uint8_t *mem = calloc(1, N), **copies = calloc(CAPS, sizeof *copies);
    size_t *sizes = calloc(CAPS, sizeof *sizes);
    recomp_rewind *r = recomp_rewind_create((size_t)256 << 20);
    CHECK(r && recomp_rewind_count(r) == 0 && !recomp_rewind_top(r, NULL) && recomp_rewind_pop(r) == -1);
    for (size_t i = 0; i < N; i++)
        mem[i] = (uint8_t)rnd();
    /* Sizes that change too: every 50th capture is shorter or longer. */
    for (int c = 0; c < CAPS; c++) {
        step_machine(mem, N, 30);
        size_t n = N - (size_t)((c / 50) % 3) * 4000;
        copies[c] = malloc(n);
        memcpy(copies[c], mem, n);
        sizes[c] = n;
        CHECK(recomp_rewind_push(r, mem, n) == 0);
    }
    CHECK(recomp_rewind_count(r) == CAPS && recomp_rewind_dropped(r) == 0);
    /* Everything held: about 30 changed bytes a capture, not 64 KB. */
    CHECK(recomp_rewind_bytes(r) < (size_t)3 * N + CAPS * 400);
    /* Back to the first, each one exact. */
    for (int c = CAPS - 1; c >= 0; c--) {
        size_t n = 0;
        const uint8_t *top = recomp_rewind_top(r, &n);
        CHECK(top && n == sizes[c] && memcmp(top, copies[c], n) == 0);
        CHECK(recomp_rewind_pop(r) == (c > 0 ? 0 : -1));
    }
    size_t n = 0;
    CHECK(recomp_rewind_count(r) == 1 && recomp_rewind_top(r, &n) && n == sizes[0] &&
          !memcmp(recomp_rewind_top(r, NULL), copies[0], n));
    /* Pushing after a pop goes on from there (the ring was truncated). */
    CHECK(recomp_rewind_push(r, copies[5], sizes[5]) == 0 && recomp_rewind_count(r) == 2);
    CHECK(recomp_rewind_pop(r) == 0 && !memcmp(recomp_rewind_top(r, NULL), copies[0], sizes[0]));
    recomp_rewind_clear(r);
    CHECK(recomp_rewind_count(r) == 0 && !recomp_rewind_top(r, NULL));
    recomp_rewind_destroy(r);
    for (int c = 0; c < CAPS; c++)
        free(copies[c]);
    free(copies);
    free(sizes);
    free(mem);
}

static void test_cap(void)
{
    enum { N = 65536, CAPS = 3000 };
    const size_t budget = (size_t)1 << 20;
    uint8_t *mem = calloc(1, N), **copies = calloc(CAPS, sizeof *copies);
    recomp_rewind *r = recomp_rewind_create(budget);
    size_t peak = 0;
    for (int c = 0; c < CAPS; c++) {
        step_machine(mem, N, 200);
        copies[c] = malloc(N);
        memcpy(copies[c], mem, N);
        CHECK(recomp_rewind_push(r, mem, N) == 0);
        if (recomp_rewind_bytes(r) > peak)
            peak = recomp_rewind_bytes(r);
    }
    size_t held = recomp_rewind_count(r);
    CHECK(peak <= budget);
    CHECK(held > 100 && held < CAPS);
    CHECK(recomp_rewind_dropped(r) == CAPS - held);
    /* What is held is the newest ones, each exact down to the oldest kept. */
    int ok = 1;
    for (size_t k = 0; k < held; k++) {
        const uint8_t *top = recomp_rewind_top(r, NULL);
        ok &= top && memcmp(top, copies[CAPS - 1 - k], N) == 0;
        recomp_rewind_pop(r);
    }
    CHECK(ok);
    recomp_rewind_destroy(r);
    /* A cap below one capture: the newest alone stays. */
    r = recomp_rewind_create(1000);
    for (int c = 0; c < 10; c++)
        CHECK(recomp_rewind_push(r, copies[c], N) == 0);
    CHECK(recomp_rewind_count(r) == 1 && !memcmp(recomp_rewind_top(r, NULL), copies[9], N));
    recomp_rewind_destroy(r);
    for (int c = 0; c < CAPS; c++)
        free(copies[c]);
    free(copies);
    free(mem);
}

static void test_config(void)
{
    recomp_rewind_cfg c;
    unsetenv("RECOMP_REWIND");
    unsetenv("RECOMP_REWIND_MB");
    unsetenv("RECOMP_REWIND_FRAMES");
    recomp_rewind_config("gba", &c);
    CHECK(c.on == 1 && c.interval == 2 && c.budget == (size_t)64 << 20);
    recomp_rewind_config("ps1", &c);
    CHECK(c.on == 1 && c.interval == 3);
    recomp_rewind_config("n64", &c);
    CHECK(c.on == 0 && c.interval == 4);
    setenv("RECOMP_REWIND", "1", 1);
    setenv("RECOMP_REWIND_MB", "128", 1);
    setenv("RECOMP_REWIND_FRAMES", "6", 1);
    recomp_rewind_config("n64", &c);
    CHECK(c.on == 1 && c.interval == 6 && c.budget == (size_t)128 << 20);
    setenv("RECOMP_REWIND", "0", 1);
    setenv("RECOMP_REWIND_MB", "4", 1);       /* below the floor: the default */
    setenv("RECOMP_REWIND_FRAMES", "x", 1);
    recomp_rewind_config("gba", &c);
    CHECK(c.on == 0 && c.interval == 2 && c.budget == (size_t)64 << 20);
    unsetenv("RECOMP_REWIND");
    unsetenv("RECOMP_REWIND_MB");
    unsetenv("RECOMP_REWIND_FRAMES");
}

/* ---- The runtimes' side, on a fake machine ---------------------------------------------------------------------- */

typedef struct fake {
    uint32_t frame;
    uint32_t keys;
    uint8_t ram[8192];
    uint32_t audio;   /* "samples" made since the last pull */
} fake;

static int s_break_load;

static void fake_input(void *m, long f)
{
    ((fake *)m)->keys = (uint32_t)(f * 2654435761u) >> 20;
}

static void fake_run(void *m)
{
    fake *x = m;
    x->frame++;
    uint32_t h = x->frame * 40503u ^ x->keys;
    for (int k = 0; k < 16; k++) {
        h = h * 1103515245u + 12345u;
        x->ram[h % sizeof x->ram] += (uint8_t)(x->keys + k);
    }
    x->audio += h & 0xFF;
}

static uint64_t fake_picture(void *m)
{
    fake *x = m;
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < sizeof x->ram; i++)
        h = (h ^ x->ram[i]) * 0x100000001B3ull;
    return h ^ x->frame;
}

static uint64_t fake_audio(void *m)
{
    fake *x = m;
    uint64_t a = x->audio;
    x->audio = 0;
    return a;
}

static size_t fake_size(void *m)
{
    (void)m;
    return sizeof(fake);
}

static size_t fake_save(uint8_t *out, size_t cap, void *m)
{
    if (cap < sizeof(fake))
        return 0;
    memcpy(out, m, sizeof(fake));
    return sizeof(fake);
}

static int fake_load(const uint8_t *in, size_t n, void *m)
{
    if (n != sizeof(fake))
        return -1;
    fake *x = m;
    memcpy(x, in, n);
    if (s_break_load)
        x->ram[7] ^= 1;  /* a load that misses a byte: the check must see it */
    x->audio = 0;
    return 0;
}

static void test_rewinder(void)
{
    fake m;
    memset(&m, 0, sizeof m);
    recomp_rewinder w;
    recomp_rewind_cfg off = {0, 2, (size_t)8 << 20}, on = {1, 2, (size_t)8 << 20};
    CHECK(recomp_rewinder_init(&w, "gba", &off) == 0 && !recomp_rewinder_on(&w));
    CHECK(recomp_rewinder_begin(&w, 1) == 0 && recomp_rewinder_frame(&w, fake_size, fake_save, &m) == 0);
    CHECK(recomp_rewinder_step(&w, fake_load, &m) == -1);
    recomp_rewinder_free(&w);

    CHECK(recomp_rewinder_init(&w, "gba", &on) == 1);
    CHECK(recomp_rewinder_step(&w, fake_load, &m) == -1); /* nothing held yet */
    /* Every 2nd frame, one shown frame each. */
    int caps = 0;
    for (int f = 0; f < 10; f++) {
        CHECK(recomp_rewinder_begin(&w, 0) == 0);
        fake_run(&m);
        caps += recomp_rewinder_frame(&w, fake_size, fake_save, &m);
    }
    CHECK(caps == 5 && recomp_rewind_count(w.ring) == 5);
    /* Fast-forward: 8 frames per shown frame, at most one capture each. */
    for (int s = 0; s < 3; s++) {
        recomp_rewinder_begin(&w, 0);
        int got = 0;
        for (int f = 0; f < 8; f++) {
            fake_run(&m);
            got += recomp_rewinder_frame(&w, fake_size, fake_save, &m);
        }
        CHECK(got == 1);
    }
    CHECK(recomp_rewind_count(w.ring) == 8 && w.captures == 8);
    /* Held: each shown frame loads the newest and drops it; the oldest stays. */
    uint32_t frame_now = m.frame;
    CHECK(recomp_rewinder_begin(&w, 1) == 1 && w.active);
    CHECK(recomp_rewinder_step(&w, fake_load, &m) == 0);
    CHECK(m.frame < frame_now && m.frame > frame_now - 8 && recomp_rewind_count(w.ring) == 7);
    for (int k = 0; k < 20; k++) {
        recomp_rewinder_begin(&w, 1);
        CHECK(recomp_rewinder_step(&w, fake_load, &m) == 0);
    }
    CHECK(recomp_rewind_count(w.ring) == 1 && m.frame == 2); /* the first capture: after frame 2 */
    CHECK(recomp_rewinder_begin(&w, 0) == 0 && !w.active && w.steps == 21);
    recomp_rewinder_free(&w);
    CHECK(!w.ring && !w.buf);
}

static void test_check(void)
{
    fake m;
    memset(&m, 0, sizeof m);
    recomp_rewind_machine mc = {&m, fake_input, fake_run, fake_picture, fake_audio, fake_size, fake_save, fake_load};
    CHECK(recomp_rewind_check("fake", &mc, 600, 2, 90, (size_t)8 << 20) == 0);
    /* Further back than held: the oldest stays, still a round trip. */
    memset(&m, 0, sizeof m);
    CHECK(recomp_rewind_check("fake", &mc, 100, 3, 500, (size_t)8 << 20) == 0);
    /* A small cap: the oldest are dropped, what is held still comes back exact. */
    memset(&m, 0, sizeof m);
    CHECK(recomp_rewind_check("fake", &mc, 3000, 1, 2500, (size_t)96 << 10) == 0);
    /* A load that is not exact is caught. */
    memset(&m, 0, sizeof m);
    s_break_load = 1;
    CHECK(recomp_rewind_check("fake", &mc, 200, 2, 10, (size_t)8 << 20) == 1);
    s_break_load = 0;
    CHECK(recomp_rewind_check("fake", NULL, 200, 2, 10, (size_t)8 << 20) == 2);
}

int main(void)
{
    test_codec();
    test_ring();
    test_cap();
    test_config();
    test_rewinder();
    test_check();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("recomp_rewind: ok\n");
    return 0;
}
