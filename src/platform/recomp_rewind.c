/* recomp_rewind -- rewind for the console runtimes (XBA-194, recomp_rewind.h). libc only. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime */
#include "recomp_rewind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---- The codec ------------------------------------------------------------------------------------------------ */

#define LIT_END_ZEROS 8   /* a literal run ends at this many zero bytes */

static size_t put_varint(uint8_t *p, size_t v)
{
    size_t n = 0;
    while (v >= 0x80) {
        p[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

/* Reads a varint at *pos (< end); returns 0, or -1 when truncated or too long. */
static int get_varint(const uint8_t *p, size_t end, size_t *pos, size_t *v)
{
    size_t x = 0;
    for (unsigned shift = 0; shift < sizeof(size_t) * 8; shift += 7) {
        if (*pos >= end)
            return -1;
        uint8_t b = p[(*pos)++];
        x |= (size_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *v = x;
            return 0;
        }
    }
    return -1;
}

size_t recomp_rewind_delta_bound(size_t n)
{
    /* The worst case: tokens of one literal byte and LIT_END_ZEROS zeros cost 3 bytes per 9; a single literal run
     * of everything costs n + its two varints. Header varint + slack. */
    return n + n / LIT_END_ZEROS + 32;
}

static uint64_t load64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* The encoder, into out[0..cap): its size, or 0 when the delta does not fit (out is then undefined). A token is
 * written only when its two varints (10 bytes each at most) and its literal bytes fit. */
static size_t delta_encode_cap(const uint8_t *older, size_t older_n, const uint8_t *newer, size_t newer_n,
                               uint8_t *out, size_t cap)
{
    size_t common = older_n < newer_n ? older_n : newer_n;
    if (cap < 32)
        return 0;
    size_t o = put_varint(out, older_n);
    size_t i = 0;
#define XB(k) ((uint8_t)(older[(k)] ^ ((k) < newer_n ? newer[(k)] : 0)))
    while (i < older_n) {
        if (cap - o < 20)
            return 0;
        /* The zero run: whole words while both captures are there, then bytes. */
        size_t z = i;
        while (z + 8 <= common && load64(older + z) == load64(newer + z))
            z += 8;
        while (z < older_n && !XB(z))
            z++;
        o += put_varint(out + o, z - i);
        if (z >= older_n) {
            out[o++] = 0; /* no literal */
            break;
        }
        /* The literal run: up to the first LIT_END_ZEROS zero bytes (or the end). */
        size_t e = z, zeros = 0;
        while (e < older_n) {
            if (XB(e))
                zeros = 0;
            else if (++zeros == LIT_END_ZEROS) {
                e -= LIT_END_ZEROS - 1;
                break;
            }
            e++;
        }
        if (e - z > cap - o - 10)
            return 0;
        o += put_varint(out + o, e - z);
        for (size_t k = z; k < e; k++)
            out[o++] = XB(k);
        i = e;
    }
#undef XB
    return o;
}

size_t recomp_rewind_delta_encode(const uint8_t *older, size_t older_n, const uint8_t *newer, size_t newer_n,
                                  uint8_t *out)
{
    return delta_encode_cap(older, older_n, newer, newer_n, out, recomp_rewind_delta_bound(older_n));
}

size_t recomp_rewind_delta_size(const uint8_t *delta, size_t delta_n)
{
    size_t pos = 0, v = 0;
    if (!delta || get_varint(delta, delta_n, &pos, &v) != 0)
        return 0;
    return v;
}

int recomp_rewind_delta_apply(uint8_t *buf, size_t *n, size_t cap, const uint8_t *delta, size_t delta_n)
{
    size_t pos = 0, older_n = 0;
    if (!buf || !n || !delta || get_varint(delta, delta_n, &pos, &older_n) != 0 || older_n > cap)
        return -1;
    if (older_n > *n)
        memset(buf + *n, 0, older_n - *n); /* the newer capture's missing bytes count as zero */
    size_t i = 0;
    while (i < older_n) {
        size_t zeros, lits;
        if (get_varint(delta, delta_n, &pos, &zeros) != 0 || zeros > older_n - i)
            return -1;
        i += zeros;
        if (get_varint(delta, delta_n, &pos, &lits) != 0 || lits > older_n - i || lits > delta_n - pos)
            return -1;
        const uint8_t *src = delta + pos;
        uint8_t *dst = buf + i;
        size_t k = 0;
        for (; k + 8 <= lits; k += 8) {
            uint64_t a, b;
            memcpy(&a, dst + k, 8);
            memcpy(&b, src + k, 8);
            a ^= b;
            memcpy(dst + k, &a, 8);
        }
        for (; k < lits; k++)
            dst[k] ^= src[k];
        pos += lits;
        i += lits;
        if (!zeros && !lits && i < older_n)
            return -1; /* an empty token before the end: not ours */
    }
    if (pos != delta_n)
        return -1;
    *n = older_n;
    return 0;
}

/* ---- The ring ------------------------------------------------------------------------------------------------- */

#define SCRATCH_MIN ((size_t)64 << 10)

typedef struct delta {
    uint8_t *p;
    size_t n;
} delta;

struct recomp_rewind {
    size_t budget;
    uint8_t *top;          /* the newest capture, whole */
    size_t top_n, top_cap; /* top_n 0: empty */
    uint8_t *scratch;      /* the coding buffer: as large as the largest delta so far needed, not a worst case */
    size_t scratch_cap;
    delta *d;              /* the older captures, oldest first, a circular array */
    size_t d_head, d_len, d_cap;
    size_t d_bytes;        /* the deltas' bytes */
    uint64_t dropped;
};

recomp_rewind *recomp_rewind_create(size_t budget)
{
    recomp_rewind *r = calloc(1, sizeof *r);
    if (r)
        r->budget = budget;
    return r;
}

void recomp_rewind_clear(recomp_rewind *r)
{
    if (!r)
        return;
    for (size_t k = 0; k < r->d_len; k++)
        free(r->d[(r->d_head + k) % r->d_cap].p);
    r->d_head = r->d_len = 0;
    r->d_bytes = 0;
    r->top_n = 0;
}

void recomp_rewind_destroy(recomp_rewind *r)
{
    if (!r)
        return;
    recomp_rewind_clear(r);
    free(r->d);
    free(r->top);
    free(r->scratch);
    free(r);
}

size_t recomp_rewind_count(const recomp_rewind *r)
{
    return r && r->top_n ? r->d_len + 1 : 0;
}

size_t recomp_rewind_bytes(const recomp_rewind *r)
{
    return r ? r->d_bytes + r->top_cap + r->scratch_cap + r->d_cap * sizeof(delta) : 0;
}

size_t recomp_rewind_budget(const recomp_rewind *r)
{
    return r ? r->budget : 0;
}

uint64_t recomp_rewind_dropped(const recomp_rewind *r)
{
    return r ? r->dropped : 0;
}

static int grow(uint8_t **p, size_t *cap, size_t need)
{
    if (*cap >= need)
        return 0;
    uint8_t *np = realloc(*p, need);
    if (!np)
        return -1;
    *p = np;
    *cap = need;
    return 0;
}

static void drop_oldest(recomp_rewind *r)
{
    delta *o = &r->d[r->d_head];
    r->d_bytes -= o->n;
    free(o->p);
    o->p = NULL;
    r->d_head = (r->d_head + 1) % r->d_cap;
    r->d_len--;
    r->dropped++;
}

/* The delta from the newest capture to state (n bytes) appended to the ring. The coding buffer starts small (a
 * delta of two captures a few frames apart is a few KB to a few MB, whatever the state's size: XBA-278) and doubles
 * when a delta does not fit, up to the worst case; once much larger than the deltas it codes it shrinks again, so the
 * memory cap keeps room for history rather than for a worst case that rarely comes. */
static int push_delta(recomp_rewind *r, const uint8_t *state, size_t n)
{
    if (r->d_len == r->d_cap) {
        size_t nc = r->d_cap ? r->d_cap * 2 : 64;
        delta *nd = malloc(nc * sizeof *nd);
        if (!nd)
            return -1;
        for (size_t k = 0; k < r->d_len; k++)
            nd[k] = r->d[(r->d_head + k) % r->d_cap];
        free(r->d);
        r->d = nd;
        r->d_cap = nc;
        r->d_head = 0;
    }
    size_t bound = recomp_rewind_delta_bound(r->top_n), want = r->scratch_cap;
    if (want < SCRATCH_MIN)
        want = SCRATCH_MIN;
    size_t dn = 0;
    for (;;) {
        if (want > bound)
            want = bound;
        if (grow(&r->scratch, &r->scratch_cap, want) != 0)
            return -1;
        dn = delta_encode_cap(r->top, r->top_n, state, n, r->scratch, r->scratch_cap);
        if (dn || r->scratch_cap >= bound)
            break;
        want = r->scratch_cap * 2;
    }
    if (!dn)
        return -1;
    uint8_t *dp = malloc(dn);
    if (!dp)
        return -1;
    memcpy(dp, r->scratch, dn);
    if (r->scratch_cap > SCRATCH_MIN && r->scratch_cap / 8 > dn) { /* far larger than what it codes now */
        size_t small = dn * 2 > SCRATCH_MIN ? dn * 2 : SCRATCH_MIN;
        uint8_t *s = realloc(r->scratch, small);
        if (s) {
            r->scratch = s;
            r->scratch_cap = small;
        }
    }
    r->d[(r->d_head + r->d_len) % r->d_cap] = (delta){dp, dn};
    r->d_len++;
    r->d_bytes += dn;
    return 0;
}

static void fit_budget(recomp_rewind *r)
{
    while (r->d_len && recomp_rewind_bytes(r) > r->budget)
        drop_oldest(r);
}

int recomp_rewind_push(recomp_rewind *r, const uint8_t *state, size_t n)
{
    if (!r || !state || !n)
        return -1;
    if (r->top_n && push_delta(r, state, n) != 0)
        return -1;
    if (grow(&r->top, &r->top_cap, n) != 0) {
        if (r->top_n) { /* the delta just pushed has no newer capture to apply to: take it back */
            r->d_len--;
            r->d_bytes -= r->d[(r->d_head + r->d_len) % r->d_cap].n;
            free(r->d[(r->d_head + r->d_len) % r->d_cap].p);
        }
        return -1;
    }
    memcpy(r->top, state, n);
    r->top_n = n;
    fit_budget(r);
    return 0;
}

int recomp_rewind_push_swap(recomp_rewind *r, uint8_t **buf, size_t *cap, size_t n)
{
    if (!r || !buf || !*buf || !cap || !n || n > *cap)
        return -1;
    if (r->top_n && push_delta(r, *buf, n) != 0)
        return -1;
    uint8_t *old = r->top;
    size_t old_cap = r->top_cap;
    r->top = *buf;
    r->top_cap = *cap;
    r->top_n = n;
    *buf = old;
    *cap = old_cap;
    fit_budget(r);
    return 0;
}

const uint8_t *recomp_rewind_top(const recomp_rewind *r, size_t *n)
{
    if (n)
        *n = r ? r->top_n : 0;
    return r && r->top_n ? r->top : NULL;
}

int recomp_rewind_pop(recomp_rewind *r)
{
    if (!r || !r->top_n || !r->d_len)
        return -1;
    size_t last = (r->d_head + r->d_len - 1) % r->d_cap;
    delta *d = &r->d[last];
    size_t older_n = recomp_rewind_delta_size(d->p, d->n);
    int ok = older_n && grow(&r->top, &r->top_cap, older_n) == 0 &&
             recomp_rewind_delta_apply(r->top, &r->top_n, r->top_cap, d->p, d->n) == 0;
    r->d_bytes -= d->n;
    free(d->p);
    d->p = NULL;
    r->d_len--;
    if (!ok) {
        /* The newest is gone too (a half-applied delta): nothing left that can be trusted. */
        recomp_rewind_clear(r);
        return -1;
    }
    return 0;
}

/* ---- The runtimes' side --------------------------------------------------------------------------------------- */

static int env_int(const char *name, int lo, int hi, int def)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return def;
    char *end;
    long x = strtol(v, &end, 10);
    if (*end || x < lo || x > hi)
        return def;
    return (int)x;
}

void recomp_rewind_config(const char *console, recomp_rewind_cfg *cfg)
{
    if (!cfg)
        return;
    int on = 1, interval = 2;
    if (console && !strcasecmp(console, "ps1")) {
        interval = 3;
    } else if (console && !strcasecmp(console, "x360")) {
        on = 0; /* Xbox 360 (XBA-280): the title's memory in use, up to 512 MB */
        interval = 4;
    } else if (console && (!strcasecmp(console, "n64") || !strcasecmp(console, "gc"))) {
        /* N64: 4 / 8 MB a capture; GameCube (XBA-264): up to ~42 MB (MEM1, ARAM). Off until XBA-278, which made them
         * fit the default 64 MB with history (the coding buffer as large as the deltas, a swap for the newest) and
         * keeps their cost per frame bounded (recomp_rewinder_frame spaces costly captures). */
        interval = 4;
    }
    const char *v = getenv("RECOMP_REWIND");
    if (v && (!strcmp(v, "0") || !strcasecmp(v, "off")))
        on = 0;
    else if (v && (!strcmp(v, "1") || !strcasecmp(v, "on")))
        on = 1;
    cfg->on = on;
    cfg->interval = env_int("RECOMP_REWIND_FRAMES", 1, 60, interval);
    cfg->budget = (size_t)env_int("RECOMP_REWIND_MB", RECOMP_REWIND_MB_MIN, RECOMP_REWIND_MB_MAX,
                                  RECOMP_REWIND_MB_DEFAULT) << 20;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int recomp_rewinder_init(recomp_rewinder *w, const char *console, const recomp_rewind_cfg *cfg)
{
    if (!w)
        return 0;
    memset(w, 0, sizeof *w);
    snprintf(w->console, sizeof w->console, "%s", console ? console : "?");
    recomp_rewind_cfg c;
    if (cfg)
        c = *cfg;
    else
        recomp_rewind_config(console, &c);
    w->interval = w->interval_set = c.interval > 0 ? c.interval : 1;
    if (c.on)
        w->ring = recomp_rewind_create(c.budget);
    if (w->ring)
        printf("%s: rewind on (a capture every %d frame%s, %zu MB)\n", w->console, w->interval,
               w->interval == 1 ? "" : "s", c.budget >> 20);
    else
        printf("%s: rewind off\n", w->console);
    fflush(stdout);
    return w->ring != NULL;
}

void recomp_rewinder_free(recomp_rewinder *w)
{
    if (!w)
        return;
    if (w->ring && w->captures) {
        printf("%s: rewind: %llu captures, %.3f ms each (max %.3f), %zu KB raw, %zu held in %.1f MB (cap %zu MB), "
               "%llu dropped, %llu steps back, one every %d frames%s\n",
               w->console, (unsigned long long)w->captures, (double)w->cap_ns / 1e6 / (double)w->captures,
               (double)w->cap_ns_max / 1e6, (size_t)(w->cap_raw / w->captures) >> 10, recomp_rewind_count(w->ring),
               (double)recomp_rewind_bytes(w->ring) / (1 << 20), recomp_rewind_budget(w->ring) >> 20,
               (unsigned long long)recomp_rewind_dropped(w->ring), (unsigned long long)w->steps, w->interval,
               w->failed ? ", some captures failed" : "");
        fflush(stdout);
    }
    recomp_rewind_destroy(w->ring);
    free(w->buf);
    memset(w, 0, sizeof *w);
}

int recomp_rewinder_on(const recomp_rewinder *w)
{
    return w && w->ring;
}

int recomp_rewinder_begin(recomp_rewinder *w, int held)
{
    if (!w || !w->ring)
        return 0;
    w->captured = 0;
    held = held ? 1 : 0;
    if (held != w->active) {
        printf("%s: rewind %s (%zu captures held)\n", w->console, held ? "start" : "end",
               recomp_rewind_count(w->ring));
        fflush(stdout);
        w->active = held;
        w->since = 0;
    }
    return held;
}

int recomp_rewinder_frame(recomp_rewinder *w, recomp_rewind_size_fn size, recomp_rewind_save_fn save, void *user)
{
    if (!w || !w->ring || !size || !save)
        return 0;
    if (++w->since < w->interval || w->captured)
        return 0;
    uint64_t t0 = mono_ns();
    /* XBA-278: the buffer from the last capture is large enough nearly always: save into it at once, and ask the
     * size (a walk of the machine's memory, as long as the save's own) only when it was not. */
    size_t n = w->buf_cap ? save(w->buf, w->buf_cap, user) : 0;
    int fresh = 0; /* a new buffer: its first writes fault its pages in, not what a capture costs */
    if (!n) {
        size_t need = size(user);
        if (!need || grow(&w->buf, &w->buf_cap, need + need / 8) != 0) { /* room for it to grow a little */
            w->failed++;
            return 0;
        }
        fresh = 1;
        n = save(w->buf, w->buf_cap, user);
    }
    /* The capture becomes the ring's newest by a swap of buffers, not a copy: the old newest's buffer is the next
     * capture's. */
    if (!n || recomp_rewind_push_swap(w->ring, &w->buf, &w->buf_cap, n) != 0) {
        w->failed++;
        return 0;
    }
    uint64_t dt = mono_ns() - t0;
    w->since = 0;
    w->captured = 1;
    w->captures++;
    w->cap_ns += dt;
    if (dt > w->cap_ns_max)
        w->cap_ns_max = dt;
    w->cap_raw += n;
    /* XBA-278: captures cost the frame thread at most RECOMP_REWIND_COST_NS per console frame on average. One into a
     * fresh buffer (page faults) is not counted. */
    if (!fresh) {
        w->cost_avg_ns = w->cost_avg_ns ? (w->cost_avg_ns * 7 + dt) / 8 : dt;
        uint64_t need = (w->cost_avg_ns + RECOMP_REWIND_COST_NS - 1) / RECOMP_REWIND_COST_NS;
        int iv = need > 60 ? 60 : (int)need;
        if (iv < w->interval_set)
            iv = w->interval_set;
        /* Up at once, down only when clearly cheaper (no flapping around a boundary): under two thirds of what the
         * current spacing allows. Compared on the cost, not on the rounded spacing: iv * 3 / 2 < interval never let
         * an interval one above a setting of 3 come back (3 * 3 / 2 is 4), so one slow capture (a preempted frame
         * thread) kept it there for good. */
        if (iv > w->interval ||
            (iv < w->interval && w->cost_avg_ns * 3 / 2 < (uint64_t)w->interval * RECOMP_REWIND_COST_NS)) {
            printf("%s: rewind captures cost %.2f ms: one every %d frames\n", w->console,
                   (double)w->cost_avg_ns / 1e6, iv);
            fflush(stdout);
            w->interval = iv;
        }
    }
    return 1;
}

int recomp_rewinder_step(recomp_rewinder *w, recomp_rewind_load_fn load, void *user)
{
    size_t n = 0;
    const uint8_t *top = w && w->ring ? recomp_rewind_top(w->ring, &n) : NULL;
    if (!top || !load || load(top, n, user) != 0)
        return -1;
    w->steps++;
    if (recomp_rewind_count(w->ring) > 1)
        recomp_rewind_pop(w->ring);
    return 0;
}

/* ---- The round trip check ------------------------------------------------------------------------------------- */

int recomp_rewind_check(const char *console, const recomp_rewind_machine *mc, long frames, int interval, int back,
                        size_t budget)
{
    if (!mc || !mc->input || !mc->run || !mc->picture || !mc->audio || !mc->size || !mc->save || !mc->load ||
        frames <= 0 || interval < 1 || back < 1)
        return 2;
    const char *name = console ? console : "?";
    uint64_t *pic = calloc((size_t)frames + 1, sizeof *pic), *snd = calloc((size_t)frames + 1, sizeof *snd);
    long *cap = calloc((size_t)frames + 1, sizeof *cap);
    recomp_rewinder w;
    recomp_rewind_cfg cfg = {1, interval, budget};
    int rc = 2;
    if (!pic || !snd || !cap || !recomp_rewinder_init(&w, name, &cfg)) {
        printf("REWIND FAIL %s: no memory\n", name);
        free(pic);
        free(snd);
        free(cap);
        return 2;
    }
    size_t ncap = 0;
    for (long f = 1; f <= frames; f++) {
        recomp_rewinder_begin(&w, 0);
        mc->input(mc->m, f);
        mc->run(mc->m);
        snd[f] = mc->audio(mc->m);
        pic[f] = mc->picture(mc->m);
        if (recomp_rewinder_frame(&w, mc->size, mc->save, mc->m))
            cap[ncap++] = f;
    }
    size_t held = recomp_rewind_count(w.ring), bytes = recomp_rewind_bytes(w.ring);
    uint64_t dropped = recomp_rewind_dropped(w.ring);
    double ms = w.captures ? (double)w.cap_ns / 1e6 / (double)w.captures : 0.0;
    long at = 0, bad = 0, first_bad = 0;
    if (!ncap || !held) {
        printf("REWIND FAIL %s: nothing captured (%llu failed)\n", name, (unsigned long long)w.failed);
        goto out;
    }
    rc = 1;
    if (bytes > budget && held > 1) { /* the newest alone may exceed a tiny cap: it always stays */
        printf("REWIND FAIL %s: the ring holds %zu bytes, over its cap of %zu\n", name, bytes, budget);
        goto out;
    }
    /* Back, as the runtime does while the hotkey is held: one capture per shown frame. */
    recomp_rewinder_begin(&w, 1);
    for (int k = 0; k < back; k++) {
        if (recomp_rewinder_step(&w, mc->load, mc->m) != 0) {
            printf("REWIND FAIL %s: step %d back did not load\n", name, k + 1);
            goto out;
        }
        at = cap[(size_t)k < held ? ncap - 1 - (size_t)k : ncap - held];
    }
    rc = 0;
    if (mc->picture(mc->m) != pic[at]) {
        printf("REWIND FAIL %s: the picture after %d steps back is not frame %ld's\n", name, back, at);
        rc = 1;
    }
    recomp_rewinder_begin(&w, 0);
    mc->audio(mc->m); /* a load leaves nothing queued; nothing to compare */
    for (long f = at + 1; f <= frames; f++) {
        recomp_rewinder_begin(&w, 0);
        mc->input(mc->m, f);
        mc->run(mc->m);
        uint64_t a = mc->audio(mc->m), q = mc->picture(mc->m);
        if ((a != snd[f] || q != pic[f]) && !bad++)
            first_bad = f;
        recomp_rewinder_frame(&w, mc->size, mc->save, mc->m);
    }
    if (bad) {
        printf("REWIND FAIL %s: %ld of the %ld frames after frame %ld differ from the first run (first: frame %ld)\n",
               name, bad, frames - at, at, first_bad);
        rc = 1;
    }
    if (!rc)
        printf("REWIND OK %s: %ld frames, a capture every %d (%zu taken, %zu held, %llu dropped), %d steps back to "
               "frame %ld, its picture and the %ld frames after it identical; capture %.3f ms (max %.3f), %zu KB raw, "
               "ring %.2f MB of %.2f MB (%.1f KB per capture held)\n",
               name, frames, interval, ncap, held, (unsigned long long)dropped, back, at, frames - at, ms,
               (double)w.cap_ns_max / 1e6, (size_t)(w.cap_raw / w.captures) >> 10, (double)bytes / (1 << 20),
               (double)budget / (1 << 20), (double)bytes / 1024.0 / (double)held);
out:
    fflush(stdout);
    recomp_rewinder_free(&w);
    free(pic);
    free(snd);
    free(cap);
    return rc;
}
