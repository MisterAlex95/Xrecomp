/* d->lock between the APU frame thread and the other threads.
 *
 * The frame thread holds d->lock for its whole loop and drops it once per
 * 32-sample frame. It used to call SwitchToThread() there on every frame,
 * which on a host with busy cores cost a scheduler quantum per 0.67 ms frame
 * and starved the audio device. It now yields only while a thread waits in
 * mcpx_apu_lock_guest; this checks both halves:
 *
 *  1. nobody waiting: the frame thread never hands the lock over;
 *  2. a thread taking the lock every 20 us gets it within 2 ms each time while the frame thread runs flat out, and the frame thread keeps
 *     producing periods meanwhile.
 *
 * The frame thread runs flat out because the fake device always reads empty
 * and the pacing high mark is set out of reach, so throttle() never waits
 * (it is that wait which drops the lock otherwise); the test tone keeps it
 * out of idle on lightweight frames. Each submit spins 200 us under the lock
 * so the lock is really held most of the time. A small zeroed buffer stands
 * in for RAM, as in apu_lifecycle. */
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "apu_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int64_t now_us(void) { return qemu_clock_get_us(QEMU_CLOCK_REALTIME); }

/* ---- fake device: every xa2_* symbol, so apu_xaudio2.o is not linked ---- */
static volatile LONG submits;
int  xa2_init(void)            { return 1; }
void xa2_shutdown(void)        {}
void xa2_clear(void)         {}
int  xa2_is_active(void)       { return 1; }
int  xa2_get_buffer_size(void) { return 512; }
int  xa2_get_buffer_count(void) { return 4; }
unsigned xa2_underruns(void)   { return 0; }
int  xa2_queued_samples(void)  { return 0; }
uint64_t xa2_samples_played(void) { return 0; }
void xa2_expect_drain(void)    {}
int  xa2_submit_samples(const int16_t *s, int n)
{
    int64_t until = now_us() + 200;
    (void)s; (void)n;
    while (now_us() < until)
        ;
    InterlockedIncrement(&submits);
    return 1;
}

#define FAKE_RAM     (256u << 10)
#define HAMMER_US    2000000
#define LIMIT_US     2000

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static LONG rd(volatile LONG *p) { return InterlockedCompareExchange(p, 0, 0); }

static struct {
    MCPXAPUState *d;
    long acquisitions, late;
    int64_t max_us;
} hammer;

static void *hammer_thread(void *arg)
{
    int64_t end = now_us() + HAMMER_US;
    (void)arg;
    while (now_us() < end) {
        int64_t t0 = now_us(), dt;
        mcpx_apu_lock_guest(hammer.d);
        dt = now_us() - t0;
        mcpx_apu_unlock_guest(hammer.d);
        hammer.acquisitions++;
        if (dt > hammer.max_us) hammer.max_us = dt;
        if (dt > LIMIT_US) hammer.late++;
        /* A guest thread does some work between register accesses. */
        t0 = now_us();
        while (now_us() - t0 < 20)
            ;
    }
    return NULL;
}

int main(void)
{
    uint8_t *ram = (uint8_t *)calloc(1, FAKE_RAM);
    MCPXAPUState *d;
    QemuThread t;
    LONG s0, s1, h0, h1;
    char what[128];

    if (!ram) { fprintf(stderr, "out of memory\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    d = mcpx_apu_init_standalone(ram);
    if (!d) { fprintf(stderr, "init failed\n"); return 2; }
    d->monitor.queued_bytes_high = 1 << 28;   /* render ahead without bound */
    mcpx_apu_play_test_tone(d);               /* resumes the frame thread */

    /* 1. Nobody waits: no hand-over. Init and the test tone took the lock
     * through mcpx_apu_lock_guest before this point, while the thread was
     * idle in a condition wait (which drops the lock), so none counted. */
    Sleep(50);
    h0 = rd(&d->lock_handovers);
    s0 = rd(&submits);
    Sleep(300);
    s1 = rd(&submits);
    h1 = rd(&d->lock_handovers);
    snprintf(what, sizeof what, "frame thread runs alone: %ld periods", (long)(s1 - s0));
    check(s1 - s0 > 100, what);
    snprintf(what, sizeof what, "nobody waiting: %ld hand-overs", (long)(h1 - h0));
    check(h1 == h0, what);

    /* 2. A thread hammering the lock. */
    hammer.d = d;
    s0 = rd(&submits);
    qemu_thread_create(&t, "hammer", hammer_thread, NULL, QEMU_THREAD_JOINABLE);
    qemu_thread_join(&t);
    s1 = rd(&submits);
    h1 = rd(&d->lock_handovers);
    printf("  hammer: %ld acquisitions in %d ms, max %lld us, %ld over %d us;"
           " frame thread: %ld periods, %ld hand-overs\n",
           hammer.acquisitions, HAMMER_US / 1000, (long long)hammer.max_us,
           hammer.late, LIMIT_US, (long)(s1 - s0), (long)(h1 - h0));
    check(hammer.acquisitions > 100, "the hammering thread gets the lock");
    /* The bound is per acquisition, but on an oversubscribed host either
     * thread can lose its core for a scheduler quantum (the frame thread
     * while it holds the lock, the hammer before it reads the clock), which
     * no lock policy prevents: with 12 busy processes and a running title
     * on 10 cores, 0.15-0.4% of acquisitions were late; lighter loads show
     * well under 0.1%. Without the hand-over the hammer starves outright
     * (17 acquisitions in 2 s, waits over 300 ms), and 6% were late in the
     * best run, so 1% still tells the two apart. */
    snprintf(what, sizeof what, "acquisitions within %d us: %ld late of %ld (max %lld us)",
             LIMIT_US, hammer.late, hammer.acquisitions, (long long)hammer.max_us);
    check(hammer.late * 100 <= hammer.acquisitions, what);
    snprintf(what, sizeof what, "frame thread keeps rendering: %ld periods",
             (long)(s1 - s0));
    check(s1 - s0 > 100, what);
    check(h1 > h0, "the frame thread handed the lock over");

    mcpx_apu_play_test_tone(d);               /* toggles the tone off */
    mcpx_apu_shutdown(d);
    free(ram);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
