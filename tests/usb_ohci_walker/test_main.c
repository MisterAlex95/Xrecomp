/*
 * The OHCI descriptor walker racing the driver that owns the queue.
 *
 * A title's USB driver re-arms and cancels transfers on its own threads while
 * the controller thread walks the endpoint lists. The walker used to copy
 * HeadP, walk the copy and store it back every pass; a driver that rewrote
 * HeadP in between got a stale value back, pointing at a descriptor it had
 * already freed, and the next pass followed that descriptor's garbage link.
 * Fable lost its pad that way with the interrupt ED's HeadP at 0xFFFFFFF0.
 *
 * Here one thread is the controller, making a million passes over one
 * interrupt ED, and another is the driver, doing what drivers do to it:
 *
 *   - enqueue: fill the dummy tail descriptor, hang a new dummy behind it,
 *     publish the new TailP (the OHCI way; HeadP untouched);
 *   - reclaim: take retired descriptors back from the done queue;
 *   - cancel: set sKip, wait for a whole controller pass to start and end,
 *     unlink everything still queued by storing HeadP = TailP, clear sKip;
 *   - re-arm an empty ED by rewriting HeadP to a fresh descriptor without
 *     skipping it, then TailP, then freeing the old dummy.
 *
 * Checked throughout: HeadP always names TailP or a queued descriptor (never
 * a free one, never anything outside the pool), Halted is never set, each
 * descriptor retires at most once and only while queued, and at the end every
 * descriptor enqueued is accounted for.
 *
 * Then the frame clock: FrameNumberOverflow at 0x7FFF -> 0x8000 and at the
 * 0xFFFF -> 0 wrap, and only there.
 */
#include "xbox_memory_layout.h"
#include "ohci.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define PASSES     1000000L
#define POOL       16
#define ED_VA      0x00400000u
#define TD_VA(i)   (0x00400100u + (uint32_t)(i) * 16u)
#define ED_SKIP    (1u << 14)
#define HEAD_HALT  1u
#define HEAD_TOG   2u
#define PTR_MASK   0xFFFFFFF0u

enum { TD_FREE, TD_DUMMY, TD_QUEUED, TD_DONE };

static uint8_t *s_mem;
static int s_state[POOL];              /* accessed with __atomic builtins */
static long s_passes;                  /* passes the walker has finished  */
static int s_walker_done;

static long s_bad_retire, s_bad_head, s_bad_halt, s_bad_garbage;
static long s_retired, s_enqueued, s_cancelled, s_rearmed, s_reclaimed;
static uint32_t s_first_bad_head;

static uint32_t *gp(uint32_t va) { return (uint32_t *)(s_mem + va); }
static uint32_t ld(uint32_t va) { return __atomic_load_n(gp(va), __ATOMIC_ACQUIRE); }
static void st(uint32_t va, uint32_t v) { __atomic_store_n(gp(va), v, __ATOMIC_RELEASE); }

static int td_index(uint32_t va)
{
    if (va < TD_VA(0) || va >= TD_VA(POOL) || (va - TD_VA(0)) % 16u)
        return -1;
    return (int)((va - TD_VA(0)) / 16u);
}

static int st_get(int i) { return __atomic_load_n(&s_state[i], __ATOMIC_ACQUIRE); }
static void st_set(int i, int v) { __atomic_store_n(&s_state[i], v, __ATOMIC_RELEASE); }

static void *walker(void *unused)
{
    long pass;
    (void)unused;
    for (pass = 0; pass < PASSES; pass++) {
        uint32_t done = 0, td;
        xbox_OhciWalkListForTest(ED_VA, &done);
        for (td = done; td; ) {
            int i = td_index(td);
            int expect = TD_QUEUED;
            uint32_t next;
            if (i < 0) {
                s_bad_retire++;
                break;
            }
            next = ld(td + 8);
            if (!__atomic_compare_exchange_n(&s_state[i], &expect, TD_DONE, 0,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                if (!s_bad_retire++)
                    printf("  retired TD %d in state %d (pass %ld)\n", i, expect, pass);
            }
            s_retired++;
            td = next;
        }
        /* An RMW, like the controller raising StartOfFrame: see wait_passes. */
        __atomic_fetch_add(&s_passes, 1, __ATOMIC_ACQ_REL);
        /* A pass here is far quicker than a driver step; without a break
         * the driver would see only a handful of passes go by. */
        if ((pass & 3) == 3)
            sched_yield();
    }
    __atomic_store_n(&s_walker_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static uint32_t s_tail;                /* the driver's TailP; it is the only writer */
static unsigned s_rng = 12345u;

static unsigned rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 16) & 0x7FFFu;
}

static int alloc_td(void)
{
    int i;
    for (i = 0; i < POOL; i++)
        if (st_get(i) == TD_FREE)
            return i;
    return -1;
}

static int queued_count(void)
{
    int i, n = 0;
    for (i = 0; i < POOL; i++)
        n += st_get(i) == TD_QUEUED;
    return n;
}

static void make_dummy(int i)
{
    st_set(i, TD_DUMMY);
    st(TD_VA(i) + 0, 0);
    st(TD_VA(i) + 4, 0);
    st(TD_VA(i) + 8, 0xFFFFFFFFu);     /* what XAPI leaves in its dummy */
    st(TD_VA(i) + 12, 0);
}

static void fill_td(uint32_t td, uint32_t next)
{
    st(td + 0, (2u << 19) | (1u << 18) | (0xFu << 28));   /* IN, rounding */
    st(td + 4, 0);
    st(td + 12, 0);
    st(td + 8, next);
}

/* HeadP must name the tail or a queued descriptor. Read twice around the
 * state check: the walker may retire the head between the two reads, and a
 * check is only meaningful for a head that did not move under it. */
static void check_head(void)
{
    uint32_t h1 = ld(ED_VA + 8), h2;
    uint32_t p = h1 & PTR_MASK;
    int i = td_index(p), ok;

    if (h1 & HEAD_HALT)
        s_bad_halt++;
    if (i < 0) {
        if (!s_bad_garbage++)
            s_first_bad_head = h1;
        return;
    }
    ok = p == s_tail || st_get(i) == TD_QUEUED;
    h2 = ld(ED_VA + 8);
    if (!ok && h1 == h2) {
        if (!s_bad_head++)
            s_first_bad_head = h1;
    }
}

/* "Set sKip, then wait for a frame to start": the pass counter plays SF.
 *
 * Read with a read-modify-write, not a load. Store sKip then load the
 * counter, against store the counter then load sKip, is Dekker's pattern:
 * release/acquire does not order a store before a later load (arm64's LDAPR
 * really does reorder them), so the driver could read a count that already
 * includes a pass which had not yet seen sKip -- and free descriptors under
 * it. The real driver gets the same ordering from acknowledging SF, an RMW on
 * HcInterruptStatus that the controller's own fetch-or then reads from. */
static void wait_passes(long n)
{
    long p0 = __atomic_fetch_add(&s_passes, 0, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&s_passes, __ATOMIC_ACQUIRE) < p0 + n
           && !__atomic_load_n(&s_walker_done, __ATOMIC_ACQUIRE))
        sched_yield();
}

static void driver_step(void)
{
    unsigned r = rnd() % 100u;
    int i;

    /* Reclaim retired descriptors. */
    for (i = 0; i < POOL; i++)
        if (st_get(i) == TD_DONE) {
            st_set(i, TD_FREE);
            s_reclaimed++;
        }

    if (r < 80) {
        /* Enqueue at the tail. */
        int d;
        if (queued_count() >= 6 || (d = alloc_td()) < 0)
            return;
        make_dummy(d);
        st_set(td_index(s_tail), TD_QUEUED);
        fill_td(s_tail, TD_VA(d));
        s_tail = TD_VA(d);
        st(ED_VA + 4, s_tail);
        s_enqueued++;
    } else if (r < 90) {
        /* Cancel what is queued, the way the specification says to. */
        uint32_t h, p;
        st(ED_VA, ld(ED_VA) | ED_SKIP);
        wait_passes(2);
        h = ld(ED_VA + 8);
        for (p = h & PTR_MASK; p != s_tail; ) {
            int k = td_index(p);
            uint32_t next;
            if (k < 0)
                break;                  /* garbage head: check_head counts it */
            next = ld(p + 8) & PTR_MASK;
            if (st_get(k) == TD_QUEUED) {
                st_set(k, TD_FREE);
                s_cancelled++;
            }
            p = next;
        }
        st(ED_VA + 8, s_tail | (h & HEAD_TOG));
        st(ED_VA, ld(ED_VA) & ~ED_SKIP);
    } else {
        /* Re-arm an empty ED from scratch, without skipping it: HeadP to a
         * fresh descriptor, then TailP to its dummy, then free the old one.
         * The old walker stored its stale HeadP over this. */
        uint32_t h = ld(ED_VA + 8);
        int a, d, old;
        if ((h & PTR_MASK) != s_tail)
            return;
        if ((a = alloc_td()) < 0)
            return;
        st_set(a, TD_QUEUED);           /* reserve before the next alloc */
        if ((d = alloc_td()) < 0) {
            st_set(a, TD_FREE);
            return;
        }
        make_dummy(d);
        fill_td(TD_VA(a), TD_VA(d));
        old = td_index(s_tail);
        st(ED_VA + 8, TD_VA(a) | (h & HEAD_TOG));
        s_tail = TD_VA(d);
        st(ED_VA + 4, s_tail);
        st_set(old, TD_FREE);
        s_enqueued++;
        s_rearmed++;
    }
}

int main(int argc, char **argv)
{
    pthread_t w;
    FILE *f;
    unsigned char *xbe;
    long n, steps = 0, queued;
    int failures = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!f) {
        fprintf(stderr, "usage: %s test.xbe (tools/conformance/mkxbe.py)\n",
                argv[0]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    xbe = malloc((size_t)n);
    if (!xbe || fread(xbe, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);
    if (!xbox_MemoryLayoutInit(xbe, (size_t)n)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 2;
    }
    s_mem = (uint8_t *)xbox_GetMemoryOffset();

    /* An interrupt IN endpoint of function address 1, endpoint 1; no pad is
     * plugged, so the walker retires every descriptor without moving data. */
    make_dummy(0);
    s_tail = TD_VA(0);
    st(ED_VA + 0, 1u | (1u << 7) | (2u << 11) | (8u << 16));
    st(ED_VA + 4, s_tail);
    st(ED_VA + 8, s_tail);
    st(ED_VA + 12, 0);

    pthread_create(&w, NULL, walker, NULL);
    while (!__atomic_load_n(&s_walker_done, __ATOMIC_ACQUIRE)) {
        driver_step();
        check_head();
        if ((++steps & 15) == 0)
            sched_yield();
    }
    pthread_join(w, NULL);
    check_head();

    /* Reclaim the last retirements, then count what is left on the queue. */
    for (n = 0; n < POOL; n++)
        if (st_get((int)n) == TD_DONE) {
            st_set((int)n, TD_FREE);
            s_reclaimed++;
        }
    queued = queued_count();

    printf("  %ld passes, %ld driver steps: %ld enqueued, %ld retired, "
           "%ld cancelled, %ld re-armed from empty, %ld still queued\n",
           PASSES, steps, s_enqueued, s_retired, s_cancelled, s_rearmed,
           queued);

#define CHECK(ok, what) do { int ok_ = (ok); \
        printf("  %-60s %s\n", what, ok_ ? "PASS" : "FAIL"); \
        if (!ok_) failures++; } while (0)
    CHECK(s_bad_garbage == 0, "HeadP never left the descriptor pool");
    CHECK(s_bad_head == 0, "HeadP always named TailP or a queued descriptor");
    CHECK(s_bad_halt == 0, "the endpoint never halted");
    CHECK(s_bad_retire == 0, "each descriptor retired once, while queued");
    CHECK(s_enqueued == s_retired + s_cancelled + queued,
          "every enqueued descriptor is accounted for");
    CHECK(s_retired > 1000 && s_cancelled > 0 && s_rearmed > 0,
          "the walker retired work and the driver raced it");
    if (s_bad_head || s_bad_garbage)
        printf("  first bad HeadP: %08X (%ld bad, %ld outside the pool)\n",
               s_first_bad_head, s_bad_head, s_bad_garbage);

    /* The frame clock: FrameNumberOverflow whenever bit 15 of HcFmNumber
     * changes (XAPI keeps the upper half of its 32-bit frame count from that
     * interrupt), and not otherwise. */
    {
        uint32_t to = 0;
        int fno;
        fno = xbox_OhciFrameAdvanceForTest(0x7FFFu, 1, &to);
        CHECK(fno && to == 0x8000u, "frame 0x7FFF -> 0x8000 raises FrameNumberOverflow");
        fno = xbox_OhciFrameAdvanceForTest(0xFFFFu, 1, &to);
        CHECK(fno && to == 0x0000u, "frame 0xFFFF -> 0x0000 raises FrameNumberOverflow");
        fno = xbox_OhciFrameAdvanceForTest(0x7FFDu, 4, &to);
        CHECK(fno && to == 0x8001u, "a 4-frame tick across 0x8000 raises it");
        fno = xbox_OhciFrameAdvanceForTest(0xFFFEu, 4, &to);
        CHECK(fno && to == 0x0002u, "a 4-frame tick across the wrap raises it");
        fno = xbox_OhciFrameAdvanceForTest(0x1234u, 4, &to);
        CHECK(!fno && to == 0x1238u, "a tick inside one half does not");
        fno = xbox_OhciFrameAdvanceForTest(0x8000u, 4, &to);
        CHECK(!fno && to == 0x8004u, "nor one just past 0x8000");
    }
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
