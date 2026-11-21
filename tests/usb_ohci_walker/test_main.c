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
#include "usb_gamepad.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* ---- The frame clock against a guest-side walk (XBA-329) -----------------
 *
 * StartOfFrame tells the driver that no walk still holds an ED it skipped.
 * The controller thread used to advance the frame and raise SF even when a
 * guest thread was in the middle of a catch-up walk (its own walk skipped,
 * list_try() having failed), and right after list_leave() otherwise. Here one
 * thread runs controller passes back to back and the other walks the same
 * long control list from the guest's side; HcFmNumber must never move inside
 * a guest walk, and no frame may be lost: the passes that found the lists
 * busy owe theirs to the next one. */
#define FR_ED_VA(i)  (0x00410000u + (uint32_t)(i) * 16u)
#define FR_EDS       60
#define FR_CONTROL   (0x80u | 0x10u)     /* operational, ControlListEnable */
#define FR_TICK      4u                  /* frames a controller pass moves */

static int s_fr_stop;
static long s_fr_walked, s_fr_deferred;
static long s_fr_run, s_fr_longest_run;   /* busy passes in a row */

static void *fr_controller(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&s_fr_stop, __ATOMIC_ACQUIRE)) {
        /* A nap between passes, as the real thread sleeps a tick between
         * two: passes back to back would keep the lists from the guest
         * side, and a guest walker preempted while it holds them would see
         * thousands of busy passes go by. */
        struct timespec nap = { 0, 20000 };
        if (xbox_OhciControllerPassForTest(FR_CONTROL)) {
            s_fr_walked++;
            s_fr_run = 0;
        } else {
            __atomic_fetch_add(&s_fr_deferred, 1, __ATOMIC_RELAXED);
            if (++s_fr_run > s_fr_longest_run)
                s_fr_longest_run = s_fr_run;
        }
        nanosleep(&nap, NULL);
    }
    return NULL;
}

static int frame_race(void)
{
    pthread_t c;
    long walks = 0, busy = 0, moved = 0, total;
    uint32_t f0 = 0, f1 = 0, expect;
    int i, failures = 0;
    uint64_t t0;
    struct timespec ts;

    /* Sixty endpoints with nothing queued (HeadP == TailP): a walk reads
     * every one of them and retires nothing, so no done queue blocks the
     * next pass. Long enough for the two sides to overlap. */
    for (i = 0; i < FR_EDS; i++) {
        st(FR_ED_VA(i) + 0, 1u | (8u << 16));
        st(FR_ED_VA(i) + 4, 0x00411000u);
        st(FR_ED_VA(i) + 8, 0x00411000u);
        st(FR_ED_VA(i) + 12, i + 1 < FR_EDS ? FR_ED_VA(i + 1) : 0);
    }
    xbox_OhciControlHeadForTest(FR_ED_VA(0));
    xbox_OhciFrameAdvanceForTest(0, 0, NULL);
    xbox_OhciGuestWalkForTest(FR_CONTROL, &f0);

    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    pthread_create(&c, NULL, fr_controller, NULL);
    /* At least 50000 walks, and on until a few hundred passes have found
     * the lists busy (the two sides did race), within 10 s. */
    for (;;) {
        uint64_t now;
        int r = xbox_OhciGuestWalkForTest(FR_CONTROL, NULL);
        if (r < 0)
            busy++;
        else {
            walks++;
            moved += r;
        }
        /* Gaps, as a guest has between two release points, or the
         * controller would hardly ever get the lists (a slow build, TSan)
         * and owe more frames than it may carry. */
        if (((walks + busy) & 15) == 0) {
            struct timespec nap = { 0, 20000 };
            nanosleep(&nap, NULL);
        }
        if (((walks + busy) & 1023) != 0)
            continue;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
        if ((walks >= 50000
             && __atomic_load_n(&s_fr_deferred, __ATOMIC_RELAXED) >= 300)
            || now - t0 > 10000)
            break;
    }
    __atomic_store_n(&s_fr_stop, 1, __ATOMIC_RELEASE);
    pthread_join(c, NULL);

    /* One last pass with the lists free pays what is still owed. */
    if (xbox_OhciControllerPassForTest(FR_CONTROL))
        s_fr_walked++;
    xbox_OhciGuestWalkForTest(FR_CONTROL, &f1);
    total = s_fr_walked + s_fr_deferred;
    expect = (f0 + (uint32_t)total * FR_TICK) & 0xFFFFu;

    printf("  frame race: %ld guest walks (%ld found the lists busy), %ld "
           "controller passes (%ld found them busy), frame %04X -> %04X\n",
           walks, busy, total, s_fr_deferred, f0, f1);
#define FR_CHECK(ok, what) do { int ok_ = (ok); \
        printf("  %-60s %s\n", what, ok_ ? "PASS" : "FAIL"); \
        if (!ok_) failures++; } while (0)
    FR_CHECK(moved == 0, "the frame never moved inside a guest-side walk");
    /* What follows depends on the scheduler, not on the code under test: a
     * starved host may never let the two sides overlap (then the run proves
     * nothing: skipped), or keep a walker preempted inside the lists for so
     * many passes that the owed frames reach their cap (then frames are
     * dropped by design, and the total is not checked). */
    if (s_fr_deferred == 0) {
        printf("  the two sides never overlapped on this host: skipped\n");
        return failures ? failures : -77;
    }
    if (s_fr_longest_run * FR_TICK < 0x4000u)
        FR_CHECK(f1 == expect, "no frame lost: busy passes owed theirs to the next");
    else
        printf("  %ld busy passes in a row reached the owed-frame cap: total not checked\n",
               s_fr_longest_run);
    if (moved)
        printf("  %ld guest walks saw the frame advance under them\n", moved);
    return failures;
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

    /* `frame_race`: only the frame clock against a guest-side walk (its own
     * ctest entry, and the one worth running under -fsanitize=thread). */
    if (argc > 2 && strcmp(argv[2], "frame_race") == 0) {
        failures = frame_race();
        if (failures < 0)
            return 77;
        printf("\n%d failure(s)\n", failures);
        return failures ? 1 : 0;
    }

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

    /* Hotplug (XBA-123): the pads follow the controllers, and the root hub
     * shows each change as a console's does. Pad n is on root port index
     * (2 + n) % 4, which XAPI calls controller port n + 1. */
    {
        const uint32_t CCS = 1u, PES = 2u, CSC = 1u << 16, PESC = 1u << 17, RHSC = 0x40u;
        UsbSetup set_addr = { 0x00, 0x05, 1, 0, 0 }, set_conf = { 0x00, 0x09, 1, 0, 0 };
        uint8_t buf[8];
        uint32_t ists, ps;
        int pad = -1, ev;

        ev = xbox_OhciHotplugStepForTest(1u, 1000, &pad);
        ps = xbox_OhciPortStatusForTest(2, &ists);
        CHECK(ev == 1 && pad == 0, "a controller on port 1: pad 1 plugged in");
        CHECK((ps & (CCS | CSC)) == (CCS | CSC) && (ists & RHSC), "on root port 3, connect change, RHSC raised");
        ev = xbox_OhciHotplugStepForTest(3u, 1300, &pad);
        CHECK(ev == 0, "a second controller waits while pad 1 enumerates");
        usb_gamepad_control(0, &set_addr, buf, (int)sizeof buf);
        usb_gamepad_control(0, &set_conf, buf, (int)sizeof buf);
        ev = xbox_OhciHotplugStepForTest(3u, 1350, &pad);
        ps = xbox_OhciPortStatusForTest(3, NULL);
        CHECK(ev == 1 && pad == 1 && (ps & CCS), "pad 1 configured: pad 2 plugged in on root port 4");
        st(ED_VA + 0, ld(ED_VA + 0) | ED_SKIP);   /* the walker's ED: out of the way */

        ev = xbox_OhciHotplugStepForTest(2u, 2000, &pad);
        CHECK(ev == 0, "controller 1 gone: nothing for a second");
        ev = xbox_OhciHotplugStepForTest(2u, 3000, &pad);
        ps = xbox_OhciPortStatusForTest(2, NULL);
        CHECK(ev == -1 && pad == 0, "then pad 1 is pulled out");
        CHECK(!(ps & (CCS | PES)) && (ps & (CSC | PESC)) == (CSC | PESC),
              "root port 3: disconnected, disabled, both change bits");
        CHECK(usb_gamepad_address(0) == 0 && !usb_gamepad_configured(0), "pad 1 back to the default address");
        CHECK(xbox_OhciPortStatusForTest(3, NULL) & CCS, "pad 2 stays plugged");
        {
            /* An input transfer still queued for pad 1's old address: device not responding, endpoint halted. */
            const uint32_t ed = 0x00402000u, td = 0x00402100u, tail = 0x00402110u, data = 0x00402200u;
            uint32_t done = 0;
            st(ed + 0, 1u | (1u << 7) | (2u << 11) | (8u << 16));
            st(ed + 4, tail);
            st(ed + 8, td);
            st(ed + 12, 0);
            st(td + 0, (2u << 19) | (0xFu << 28));
            st(td + 4, data);
            st(td + 8, tail);
            st(td + 12, data + 19);
            xbox_OhciWalkListForTest(ed, &done);
            CHECK(done == td && (ld(td) >> 28) == 5u && (ld(ed + 8) & HEAD_HALT),
                  "a transfer to the pulled pad: DEVICE NOT RESPONDING, ED halted");
        }
        set_addr.wValue = 2;
        usb_gamepad_control(1, &set_addr, buf, (int)sizeof buf);
        usb_gamepad_control(1, &set_conf, buf, (int)sizeof buf);
        ev = xbox_OhciHotplugStepForTest(3u, 3100, &pad);
        ps = xbox_OhciPortStatusForTest(2, NULL);
        CHECK(ev == 1 && pad == 0 && (ps & CCS), "controller 1 back: pad 1 plugged in again on root port 3");
    }
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
