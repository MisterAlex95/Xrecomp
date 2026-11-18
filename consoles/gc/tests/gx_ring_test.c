/* gx_ring_test -- gx_ring.h (RECOMP_GC_GX_STREAM=storage) against a simulated GPU, without GL. The GPU runs a queue of
 * draws and fences at its own pace; each draw checks, when it executes, that the bytes it reads are still the ones
 * written for it. Random upload sizes and alignments, rings from tiny to large, a GPU that is fast, slow or stalled
 * until waited for. Exit 0, 1 on a failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_ring.h"

#define RING_MAX (1u << 16)
#define QUEUE_MAX (1u << 16)

typedef struct cmd {
    int is_fence;
    unsigned id;           /* fence: its number */
    uint32_t off, n, tag;  /* draw: the bytes it reads and what they must hold */
} cmd;

typedef struct sim {
    uint32_t mem[RING_MAX]; /* one tag per byte of the ring */
    cmd q[QUEUE_MAX];
    unsigned first, count;
    unsigned next_fence, fences_live, done_fence; /* fences up to done_fence have executed */
    unsigned waits, null_every, made;
    unsigned long draws_checked, failures;
} sim;

static uint32_t rng_state = 0x12345678u;

static uint32_t rnd(uint32_t n)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state % n;
}

static void fail(sim *s, const char *what, uint32_t a, uint32_t b)
{
    if (s->failures++ < 10)
        printf("FAIL %s (%u, %u)\n", what, a, b);
}

/* The GPU executes one command. */
static void gpu_step(sim *s)
{
    cmd *c = &s->q[s->first];
    s->first = (s->first + 1) % QUEUE_MAX;
    s->count--;
    if (c->is_fence) {
        s->done_fence = c->id;
        return;
    }
    for (uint32_t i = 0; i < c->n; i++)
        if (s->mem[c->off + i] != c->tag) {
            fail(s, "a draw read bytes overwritten while it was in flight (offset, tag)", c->off + i, c->tag);
            break;
        }
    s->draws_checked++;
}

static void issue(sim *s, cmd c)
{
    if (s->count == QUEUE_MAX)
        gpu_step(s);
    s->q[(s->first + s->count) % QUEUE_MAX] = c;
    s->count++;
}

static void *op_fence(void *user)
{
    sim *s = user;
    if (s->null_every && ++s->made % s->null_every == 0) { /* glFenceSync failed: gx_gl.c has finished the GPU's work */
        while (s->count)
            gpu_step(s);
        return NULL;
    }
    unsigned id = ++s->next_fence;
    s->fences_live++;
    issue(s, (cmd){.is_fence = 1, .id = id});
    return (void *)(uintptr_t)id;
}

static void op_retire(void *user, void *sync)
{
    sim *s = user;
    if (!sync)
        return;
    unsigned id = (unsigned)(uintptr_t)sync;
    if (s->done_fence < id)
        s->waits++;
    while (s->done_fence < id) {
        if (!s->count) {
            fail(s, "waiting for a fence that was never issued", id, s->done_fence);
            break;
        }
        gpu_step(s);
    }
    s->fences_live--;
}

static void op_discard(void *user, void *sync)
{
    sim *s = user;
    if (sync)
        s->fences_live--;
}

static const gx_ring_ops ops = {op_fence, op_retire, op_discard};

/* speed: commands the GPU may execute per upload, in 1/16; 0: only when waited for. */
static void run(uint32_t size, uint32_t align_max, uint32_t n_max, unsigned speed, unsigned null_every, unsigned rounds)
{
    static sim s;
    memset(&s, 0, sizeof s);
    s.null_every = null_every;
    gx_ring r;
    gx_ring_init(&r, size);
    uint32_t last_off = 0;
    for (unsigned i = 1; i <= rounds; i++) {
        uint32_t align = 1u << rnd(align_max + 1);
        uint32_t n = 1 + rnd(n_max);
        if (!gx_ring_fits(&r, n))
            n = (uint32_t)(r.size / 4);
        uint64_t head0 = r.head;
        uint32_t off = (uint32_t)gx_ring_reserve(&r, &ops, &s, n, align);
        if (off % align)
            fail(&s, "misaligned offset (offset, alignment)", off, align);
        if (off + n > size)
            fail(&s, "reservation past the end of the ring (offset, bytes)", off, n);
        if (r.head - r.tail > size)
            fail(&s, "more bytes live than the ring holds", (uint32_t)(r.head - r.tail), size);
        if (r.head < head0 + n)
            fail(&s, "head moved back", (uint32_t)r.head, (uint32_t)head0);
        for (uint32_t b = 0; b < n; b++) /* the CPU writes: any draw still queued on these bytes will notice */
            s.mem[off + b] = i;
        issue(&s, (cmd){.off = off, .n = n, .tag = i});
        gx_ring_after_draw(&r, &ops, &s);
        last_off = off;
        if (speed)
            for (unsigned k = (speed + rnd(16)) / 16; k && s.count; k--)
                gpu_step(&s);
    }
    (void)last_off;
    gx_ring_close(&r, &ops, &s);
    while (s.count)
        gpu_step(&s);
    gx_ring_release(&r, &ops, &s);
    if (s.fences_live)
        fail(&s, "fences left alive", s.fences_live, 0);
    if (s.draws_checked != rounds)
        fail(&s, "draws executed", (uint32_t)s.draws_checked, rounds);
    printf("ring %6u  align <= %3u  upload <= %5u  gpu %2u/16  null fence 1/%u: %u wraps, %u waits, %s\n", size,
           1u << align_max, n_max, speed, null_every, (unsigned)r.wraps, s.waits, s.failures ? "FAILED" : "ok");
    if (s.failures)
        exit(1);
}

int main(void)
{
    /* the GPU only runs when waited for: every reuse of the ring must wait */
    run(4096, 8, 1000, 0, 0, 200000);
    run(65536, 8, 16384, 0, 0, 100000);
    /* slower than, near and faster than the CPU */
    for (unsigned speed = 4; speed <= 64; speed *= 2) {
        run(4096, 8, 300, speed, 0, 200000);
        run(5000, 6, 1250, speed, 0, 200000); /* a size that is no multiple of the alignment */
        run(65536, 8, 4000, speed, 0, 100000);
    }
    /* a single huge upload after another, the most skipped at the wraps */
    run(4096, 0, 1024, 16, 0, 100000);
    run(4096, 10, 1024, 16, 0, 100000);
    /* tiny uploads: more regions than fences would hold without the slices */
    run(65536, 2, 16, 8, 0, 400000);
    /* glFenceSync failing now and then */
    run(4096, 8, 1000, 8, 3, 100000);
    /* The ring must wait when, and only when, it is full. */
    {
        static sim s;
        memset(&s, 0, sizeof s);
        gx_ring r;
        gx_ring_init(&r, 1024);
        for (unsigned i = 0; i < 4; i++) {
            gx_ring_reserve(&r, &ops, &s, 256, 1);
            gx_ring_after_draw(&r, &ops, &s);
        }
        if (s.waits) {
            printf("FAIL a ring that was not full waited\n");
            return 1;
        }
        gx_ring_reserve(&r, &ops, &s, 256, 1);
        if (s.waits != 1) {
            printf("FAIL a full ring did not wait for the GPU (%u waits)\n", s.waits);
            return 1;
        }
    }
    printf("gx_ring_test: ok\n");
    return 0;
}
