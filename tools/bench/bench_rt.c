/* The runtime side of the lifted-code benchmark (tools/bench, XBA-37/38).
 *
 * Plays libmain.so: it defines what the generated C links against (the
 * per-thread guest CPU g_cpu, the guest memory offset, the indirect-call
 * trace) and drives the workloads. Built either into one executable with the
 * lifted C (a desktop title) or as libbenchrt.so, dlopened by bench_main.c,
 * which in turn dlopens libtitle.so (the lifted C) -- the Android layout,
 * where the guest registers are thread-locals of a dlopened library.
 *
 * Workloads (tools/bench/corpus.s) are cdecl `uint32_t w(uint32_t n)`. Each
 * is run `reps` times; the best wall time of the runs is reported, with the
 * checksum (eax) the build must agree on with every other build.
 *
 * Output, one line per workload: `name va n checksum best_ns`. */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#include "recomp_types.h"
#include "bench_syms.h"         /* generated: BENCH_SYM_*, BENCH_WORKLOADS */

RECOMP_TLS RecompCpu g_cpu = {
    .r_fp_control_word = 0x037Fu,
    .r_mxcsr = 0x1F80u,
    .r_fp_cc = 0x4000,
    .r_eflags = 0x00000202u,
};
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
int g_force_return;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
volatile int g_recomp_cas64_lock;

recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return NULL; }

void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "bench: indirect call to 0x%08X has no function\n", va);
    exit(3);
}

void recomp_icall_not_code_log(uint32_t va)
{
    fprintf(stderr, "bench: indirect call to 0x%08X is not code\n", va);
    exit(3);
}

#define WINDOW (4u << 20)       /* guest addresses 0 .. 4 MB */
#define STACK_TOP 0x00300000u

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }

static void fill(uint8_t *mem)
{
    uint32_t s = 1, i;
    for (i = 0; i < 1024; i++)
        ((uint32_t *)(mem + BENCH_SYM_src_buf))[i] = lcg(&s);
    for (i = 0; i < 4096; i++)
        mem[BENCH_SYM_byte_buf + i] = (uint8_t)(lcg(&s) >> 24);
    for (i = 0; i < 16; i++)
        ((float *)(mem + BENCH_SYM_matrix))[i] = (float)((int)(lcg(&s) >> 24) - 128) / 64.0f;
    for (i = 0; i < 1024; i++)
        ((float *)(mem + BENCH_SYM_vec_in))[i] = (float)((int)(lcg(&s) >> 22) - 512) / 256.0f;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* lookup: the title's recomp_lookup (dlsym'd in the shared layout). */
int bench_run(recomp_func_t (*lookup)(uint32_t), int (*dispatch_init)(void),
              const unsigned char *image, size_t image_len, int reps, double scale, const char *only)
{
    static const struct { const char *name; uint32_t va; uint32_t n; } wl[] = { BENCH_WORKLOADS };
    uint8_t *mem = mmap(NULL, WINDOW, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    size_t i;
    if (mem == MAP_FAILED) { perror("mmap"); return 2; }
    memcpy(mem + BENCH_IMAGE_VA, image, image_len);
    g_xbox_mem_offset = (ptrdiff_t)mem;
    g_xbox_code_lo = BENCH_IMAGE_VA;
    g_xbox_code_hi = BENCH_IMAGE_VA + (uint32_t)image_len;
    if (dispatch_init)
        dispatch_init();
    for (i = 0; i < sizeof wl / sizeof wl[0]; i++) {
        recomp_func_t fn = lookup(wl[i].va);
        uint32_t n = (uint32_t)(wl[i].n * scale), sum = 0;
        uint64_t best = UINT64_MAX;
        int r;
        if (only && !strstr(only, wl[i].name))
            continue;
        if (n < 1) n = 1;
        if (!fn) { fprintf(stderr, "bench: %s (0x%08X) not lifted\n", wl[i].name, wl[i].va); return 2; }
        for (r = 0; r < reps; r++) {
            uint64_t t0;
            fill(mem);
            g_esp = STACK_TOP;
            g_df = 0;
            g_fp_top = 0;
            g_esp -= 4; MEM32(g_esp) = n;
            g_esp -= 4; MEM32(g_esp) = 0xFFFFFFF0u;     /* the return address */
            t0 = now_ns();
            fn();
            t0 = now_ns() - t0;
            g_esp += 4;                                 /* cdecl: the caller pops n */
            if (g_esp != STACK_TOP) {
                fprintf(stderr, "bench: %s left esp at 0x%08X\n", wl[i].name, g_esp);
                return 2;
            }
            if (r && g_eax != sum) {
                fprintf(stderr, "bench: %s is not deterministic\n", wl[i].name);
                return 2;
            }
            sum = g_eax;
            if (t0 < best) best = t0;
        }
        printf("%s 0x%08X %u 0x%08X %llu\n", wl[i].name, wl[i].va, n, sum, (unsigned long long)best);
        fflush(stdout);
    }
    return 0;
}
