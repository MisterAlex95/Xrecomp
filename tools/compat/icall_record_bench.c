/* What recording indirect-call hits by default would cost (XBA-138; numbers and
 * the proposal in docs/technical/compat-runner.md « Recording hits by default »).
 * Not built by any project: a measurement, kept so it can be rerun.
 *
 *   cc -O2 -pthread tools/compat/icall_record_bench.c -o /tmp/b
 *   /tmp/b MODE [THREADS [TARGETS [CALLS]]]
 *
 * Mimics RECOMP_ICALL (templates/runtime/recomp_types.h): the trace ring write,
 * the global count, the code-range check, a direct-map lookup, the call.
 * MODE 0: no record (the default build today); 1: RECOMP_ICALL_OBSERVE as the
 * RECOMP_ICALL_FEEDBACK build has it (|= on every call); 2: test-before-set
 * (a store only the first time a target is seen). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

#define FB_BASE 0x00010000u
#define FB_SIZE 0x00800000u
typedef void (*fn_t)(void);
static volatile unsigned char g_seen[FB_SIZE];
static volatile uint32_t g_trace[16];
static volatile uint32_t g_trace_idx;
static volatile uint64_t g_count;
static fn_t *g_map;             /* direct map, one slot per 4 bytes of VA */
static volatile uint32_t g_sink;
static void f0(void) { g_sink++; }
static void f1(void) { g_sink += 2; }

static uint32_t *g_targets;
static int g_ntargets, g_mode;
static long g_iters;

static inline void icall(uint32_t va)
{
    g_trace[g_trace_idx & 15] = va;
    g_trace_idx++;
    g_count++;
    if (va - FB_BASE >= FB_SIZE) return;
    fn_t fn = g_map[(va - FB_BASE) >> 2];
    if (fn) {
        uint32_t off = va - FB_BASE;
        if (g_mode == 1) g_seen[off] |= 1;
        else if (g_mode == 2) { if (!(g_seen[off] & 1)) g_seen[off] |= 1; }
        fn();
    }
}

static void *worker(void *arg)
{
    (void)arg;
    for (long i = 0; i < g_iters; i++)
        icall(g_targets[i % g_ntargets]);
    return NULL;
}

int main(int argc, char **argv)
{
    g_mode = atoi(argv[1]);
    int threads = argc > 2 ? atoi(argv[2]) : 1;
    g_ntargets = argc > 3 ? atoi(argv[3]) : 4096;
    g_iters = argc > 4 ? atol(argv[4]) : 50000000L;
    g_map = calloc(FB_SIZE / 4, sizeof(fn_t));
    g_targets = malloc(sizeof(uint32_t) * g_ntargets);
    srand(1);
    for (int i = 0; i < g_ntargets; i++) {
        uint32_t va = FB_BASE + ((uint32_t)rand() % (FB_SIZE / 16)) * 16;
        g_targets[i] = va;
        g_map[(va - FB_BASE) >> 2] = (i & 1) ? f1 : f0;
    }
    pthread_t t[8];
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < threads; i++) pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (double)g_iters;
    printf("mode %d threads %d targets %d: %.2f ns per call (per thread)\n", g_mode, threads, g_ntargets, ns);
    return 0;
}
