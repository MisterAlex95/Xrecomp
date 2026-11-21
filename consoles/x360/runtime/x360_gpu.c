/* x360_gpu.c -- the GPU as the CPU sees it (XBA-270): the Vd* kernel calls, the register page and the command
 * processor (xenos_cp.c) fed from the title's ring buffer, and the renderer (xenos_gl.c) when x360_set_renderer
 * attached one.
 *
 * The GPU's registers sit at 0x7FC80000 + 4 * index (Xenia's MMIO range). Here that page is ordinary memory: a
 * title's store to CP_RB_WPTR lands there and the scheduler, between time slices, sees the write pointer move and
 * runs the new packets (x360_gpu_poll); the read pointer goes back through the write-back address the title gave.
 * Registers titles poll get plausible values written into the page (the vertical counter moves with time).
 * Vertical blank: every 1/60 s of virtual time, when the title registered a graphics interrupt callback, the
 * callback runs on a short-lived guest thread (r3 = 0: vblank, r4 = its user data), as an interrupt would;
 * PM4_INTERRUPT packets do the same with r3 = 1, once for each CPU the packet's mask names, as that CPU: the PCR's
 * (and the KTHREAD's) processor byte says it, since Direct3D's handler clears the bit of the CPU it runs on
 * (lbz 0x10C(r13)) from the word the ring then waits on. The vertical blank runs as CPU 2. VdSwap puts the front
 * buffer's fetch constant and the swap token in the ring; the front buffer of each swap is recorded (and traced).
 *
 * Two halves (WP-22, docs/runtime/x360-gpu-thread.md). The post half runs on the guest thread and owns everything
 * the title can observe: the register page, the vertical blank, the interrupt context (raise_irq), the Vd* calls.
 * The run half is the command processor and the renderer (xenos_cp_set_wptr: packets, draws, resolves, swaps). They
 * meet only at struct gpu_link: the post half posts the write pointer; the run half answers with word stores into
 * guest memory (read pointer write-back, MEM_WRITE / REG_TO_MEM / COND_WRITE / EVENT_WRITE_*, scratch write-back,
 * resolves, memory export), a stalled flag (an unmet WAIT_REG_MEM: post again) and an interrupt-pending word the
 * post half turns into raise_irq; x360_gpu_drain finishes what was posted (save states, rewind, lazy-resolve
 * flushes; only on the run half's own thread: elsewhere it leaves posted work posted) and gpu_hold / gpu_release
 * bracket a change of the run half's state from the guest side (VdInitializeRingBuffer, VdEnableRingBuffer
 * RPtrWriteBack). Stage 1: the run half is called right after the post,
 * on the same thread, so runs are what they were before the split, frame for frame and byte for byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#include "x360_internal.h"
#include "xenos_cp.h"
#include "xenos_gl.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

#define GPU_MMIO 0x7FC80000u
#define VBLANK_100NS 166667u

/* The seam between the post half (guest thread) and the run half (the command processor and the renderer). Every word
 * here is read and written with __atomic builtins: in stage 1 both halves run on one thread and the ordering costs
 * nothing, in stage 2 the run half moves to the GL thread and this is all they share besides guest memory. Host state
 * only (not in save states): x360_gpu_drain empties it before a state is taken or loaded. */
#define LINK_IRQ_CPUS 0x3Fu      /* PM4_INTERRUPT: the CPUs whose source 1 callback is to run */
#define LINK_IRQ_SRC0 0x100u     /* the run half asked for source 0 (not raised by any packet today) */
#define LINK_IRQ_SRC1 0x200u     /* the run half asked for source 1 (PM4_INTERRUPT) */
typedef struct gpu_link {
    uint64_t post;     /* post side: (posts made << 32) | the write pointer of the last one */
    uint32_t taken;    /* run side: the number of the last post it ran to (post >> 32 when there is nothing to do) */
    uint32_t stalled;  /* run side: it stopped at an unmet wait; the post side posts again at its next poll */
    uint32_t irq;      /* run side sets, post side takes: LINK_IRQ_* */
    uint64_t swaps;    /* run side: cp.swaps after its last run (the post side counts swaps where it sees them) */
} gpu_link;

/* Stage 2 (WP-22): the run half on the renderer's thread (the GL thread) while the machine runs on a CPU worker.
 * x360_gpu_overlap hands a job (x360_run_until, x360_run) to the worker and serves the run half on the calling thread
 * until the job ends: posts (x360_gpu_run), drain requests from the worker (gpu_hold), lazy-resolve flushes. Host
 * state; mu guards everything here but `active` (set before the job is handed over, cleared after it ended). */
typedef struct gpu_overlap {
    pthread_mutex_t mu;
    pthread_cond_t work;   /* the GL thread waits: a post, a drain request, the job's end */
    pthread_cond_t done;   /* the worker waits: a run ended (drain, back-pressure), a drain acknowledged */
    pthread_cond_t job_cv; /* the CPU worker waits for a job */
    pthread_t cpu, gl;     /* the worker; the thread serving the run half while a job runs */
    int cpu_made, quit, ok;
    int (*job)(x360_machine *, uint64_t);
    uint64_t arg;
    int job_rc, job_pending, job_done;
    int active;  /* a job runs on the worker and gl serves the run half (atomic: read by the worker unlocked) */
    int running; /* gl is inside x360_gpu_run or a flush, with mu released: the run half's state is busy */
    int held;    /* gpu_hold on the worker: gl starts nothing until gpu_release */
    uint32_t drain_req, drain_ack;
    int flush_req;
    int lockstep; /* RECOMP_X360_GPU_LOCKSTEP=1 (checks): every post waits for its run, as in stage 1 (same results) */
    void (*worker_hook)(int starting, void *user); /* x360_gpu_set_worker_hook: run on the worker as it starts / ends */
    void *worker_user;
} gpu_overlap;

struct x360_gpu {
    /* the run half's state: only x360_gpu_run (and code holding the GPU: gpu_hold, x360_gpu_drain) touches it */
    xenos_cp cp;
    /* the post half's state: guest thread only */
    uint32_t irq_callback, irq_user;
    uint64_t next_vblank;
    uint32_t vblanks, last_wptr;
    int irq_pending[2]; /* source 0 (vblank), 1 (command processor) queued while one runs */
    uint32_t irq_cpus;  /* the CPUs whose source 1 callback is still to run (PM4_INTERRUPT's mask) */
    int ring;
    int irq_thread; /* the interrupt context's slot, -1 before the first interrupt */
    /* WP-19 (host state): swaps seen by x360_gpu_poll, those that came more than 20 ms of virtual time after the one
     * before, and the virtual time of the last (0: none since the start or a state load). Post side (the virtual
     * clock is the guest's): counted from link.swaps, swaps_seen being its value at the last poll. */
    uint64_t host_swaps, long_swaps, last_swap_vt, swaps_seen;
    gpu_link link;  /* the seam */
    /* the thread the run half belongs to (the renderer's GL context): the one that attached the renderer, else the
     * first to run packets. x360_gpu_drain elsewhere (an app's menu or auto-save thread) leaves posted work posted. */
    pthread_t owner;
    int owner_set;
    int deferred; /* x360_gpu_set_deferred: x360_gpu_poll posts only; x360_gpu_run / _drain run (stage 2's hook) */
    gpu_overlap ov;         /* stage 2: the CPU worker and the GL thread's service (x360_gpu_overlap) */
    uint64_t swaps_issued;  /* post side: VdSwap calls (counted from cp.swaps at link_sync), stage 2's back-pressure */
    uint64_t lag_t;         /* post side, stage 2 unpaced: virtual time (+1) since posted work waits on the GL thread */
    uint64_t inline_cpu_ns; /* the part of cp_cpu_ns spent on the guest's own thread (stage 1 runs): x360_gpu_cpu_ns */
    /* the run half's host state (from here down) */
    char shader_dir[512]; /* --dump-shaders: each distinct microcode written there once */
    /* --dump-shaders without a file system call per load (host state, not in save states): the (stage, CRC) of each
     * microcode already on disk, open-addressed (0 = empty, else stage << 32 | CRC | 1 << 33), and per stage the
     * microcode of the last load, so a reload of the same words skips even the byte swap and the CRC */
    uint64_t *dumped;
    uint32_t dumped_cap, dumped_n; /* cap: a power of two, at most half full */
    uint32_t last_words[2];        /* 0: no previous load for that stage */
    uint32_t *last_code[2];
    xenos_gl *gl;         /* --render: the GLES renderer (host state: not in save states) */
    uint64_t cp_ns;       /* wall time running the ring's packets (draws and resolves included): the perf line's gpu */
    uint64_t cp_cpu_ns;   /* the same span in the thread's CPU time (WP-15): the rest of cp_ns is waiting (the GPU) */
};

#define LONG_SWAP_100NS 200000u /* 20 ms */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t thread_cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void gl_draw(void *ctx, const xenos_cp *cp, const xenos_draw *d)
{
    xenos_gl_draw(ctx, cp, d);
}

static void gl_swap(void *ctx, const xenos_cp *cp)
{
    xenos_gl_swap(ctx, cp);
}

static struct x360_gpu *gpu_of(x360_machine *x);

/* the set of dumped (stage, CRC): 1 when key was already in it, else inserted (when there is room) and 0 */
static int dumped_test(struct x360_gpu *g, uint64_t key, int insert)
{
    if (g->dumped_cap) {
        uint32_t m = g->dumped_cap - 1, i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & m;
        for (; g->dumped[i]; i = (i + 1) & m)
            if (g->dumped[i] == key)
                return 1;
        if (!insert)
            return 0;
        if (2 * (g->dumped_n + 1) <= g->dumped_cap) {
            g->dumped[i] = key;
            g->dumped_n++;
            return 0;
        }
    }
    if (!insert)
        return 0;
    uint32_t cap = g->dumped_cap ? 2 * g->dumped_cap : 1024;
    uint64_t *t = calloc(cap, sizeof *t);
    if (!t)
        return 0; /* no memory: the file system check stays the only one */
    for (uint32_t k = 0; k < g->dumped_cap; k++)
        if (g->dumped[k]) {
            uint32_t j = (uint32_t)((g->dumped[k] * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1);
            while (t[j])
                j = (j + 1) & (cap - 1);
            t[j] = g->dumped[k];
        }
    free(g->dumped);
    g->dumped = t;
    g->dumped_cap = cap;
    return dumped_test(g, key, 1);
}

static void dumped_reset(struct x360_gpu *g)
{
    free(g->dumped);
    g->dumped = NULL;
    g->dumped_cap = g->dumped_n = 0;
    for (int k = 0; k < 2; k++) {
        free(g->last_code[k]);
        g->last_code[k] = NULL;
        g->last_words[k] = 0;
    }
}

static void shader_loaded(void *ctx, int pixel, const uint32_t *code, uint32_t words)
{
    x360_machine *x = ctx;
    struct x360_gpu *g = x->gpu;
    if (!g || !g->shader_dir[0] || !words)
        return;
    int st = pixel ? 1 : 0;
    /* the same words as this stage's previous load (the code buffer is the command processor's, so its address says
     * nothing: the words themselves are compared) */
    if (g->last_words[st] == words && !memcmp(g->last_code[st], code, 4 * (size_t)words))
        return;
    uint8_t be[4 * XENOS_SHADER_WORDS];
    for (uint32_t k = 0; k < words; k++) {
        be[4 * k] = (uint8_t)(code[k] >> 24);
        be[4 * k + 1] = (uint8_t)(code[k] >> 16);
        be[4 * k + 2] = (uint8_t)(code[k] >> 8);
        be[4 * k + 3] = (uint8_t)code[k];
    }
    uint32_t crc = x360_crc32(be, 4 * words);
    uint64_t key = (uint64_t)st << 32 | crc | 1ull << 33;
    int known = dumped_test(g, key, 0);
    if (!known) {
        char path[600];
        snprintf(path, sizeof path, "%s/%s_%08X.bin", g->shader_dir, pixel ? "ps" : "vs", crc);
        FILE *f = fopen(path, "rb");
        if (f) { /* already there */
            fclose(f);
            known = 1;
        } else if ((f = fopen(path, "wb"))) {
            known = fwrite(be, 4, words, f) == words;
            known &= fclose(f) == 0;
        }
        if (known)
            dumped_test(g, key, 1);
    }
    if (!known)
        return; /* not on disk: the next load of these words tries again */
    if (!g->last_code[st])
        g->last_code[st] = malloc(4 * XENOS_SHADER_WORDS);
    if (g->last_code[st]) {
        memcpy(g->last_code[st], code, 4 * (size_t)words);
        g->last_words[st] = words;
    }
}

int x360_set_renderer(x360_machine *x, const char *shader_cache, const char *frames_dir, unsigned max_frames,
                      int width, int height, char *why, size_t why_n)
{
    return x360_set_renderer_ex(x, shader_cache, frames_dir, max_frames, width, height, 1, 0, why, why_n);
}

int x360_set_renderer_ex(x360_machine *x, const char *shader_cache, const char *frames_dir, unsigned max_frames,
                         int width, int height, int scale, int attach, char *why, size_t why_n)
{
    struct x360_gpu *g = gpu_of(x);
    if (!g)
        return -1;
    xenos_gl *gl = attach ? xenos_gl_attach(shader_cache, width, height, scale, why, why_n)
                          : xenos_gl_create_scaled(shader_cache, width, height, scale, why, why_n);
    if (!gl)
        return -1;
    xenos_gl_set_frames_dir(gl, frames_dir, max_frames);
    if (x->log)
        xenos_gl_set_log(gl, x->log);
    g->gl = gl;
    g->owner = pthread_self(), g->owner_set = 1; /* the GL context is current here: the run half's thread */
    g->cp.render_ctx = gl;
    g->cp.draw_hook = gl_draw;
    g->cp.swap_hook = gl_swap;
    return 0;
}

void x360_render_stats(x360_machine *x, FILE *out)
{
    if (!x->gpu || !x->gpu->gl)
        return;
    xenos_gl_stats s;
    xenos_gl_get_stats(x->gpu->gl, &s);
    fprintf(out,
            "render: drawn %llu, resolves %llu, frames %llu (%llu from the front buffer); skipped: no shader %llu, "
            "unsupported %llu; textures not handled %llu; shaders missing from the cache %u; eDRAM surfaces %llu, "
            "resolve pictures sampled %llu, mipmapped %llu, computed-index fetches %llu; memory export: %llu draws, "
            "%llu dwords, %llu dropped, %llu shaders skipped\n",
            (unsigned long long)s.drawn, (unsigned long long)s.resolves, (unsigned long long)s.frames,
            (unsigned long long)s.front_buffer_frames,
            (unsigned long long)s.no_shader, (unsigned long long)s.unsupported,
            (unsigned long long)s.unsupported_textures, s.missing_shaders, (unsigned long long)s.surfaces,
            (unsigned long long)s.copy_textures, (unsigned long long)s.mipmapped,
            (unsigned long long)s.computed_fetches, (unsigned long long)s.memexport_draws,
            (unsigned long long)s.memexport_dwords, (unsigned long long)s.memexport_dropped,
            (unsigned long long)s.memexport_skipped);
    if (xenos_gl_lazy_resolves(x->gpu->gl))
        fprintf(out, "render: lazy resolves: %llu deferred, %llu written later, %llu dropped (the CPU wrote first)\n",
                (unsigned long long)s.resolves_deferred, (unsigned long long)s.resolve_flushes,
                (unsigned long long)s.resolves_dropped);
    fprintf(out, "render: rectangle lists: %llu on the GPU, %llu read back\n",
            (unsigned long long)(s.rect_draws - s.rect_lists_read_back), (unsigned long long)s.rect_lists_read_back);
    fprintf(out, "render: eDRAM overlaps: %llu carried over, %llu not; multisampled targets: %llu\n",
            (unsigned long long)s.edram_reinterpreted, (unsigned long long)s.edram_stale,
            (unsigned long long)s.msaa_surfaces);
    uint64_t count;
    const char *why;
    for (int i = 0; (why = xenos_gl_unsupported_reason(x->gpu->gl, i, &count)) != NULL; i++)
        fprintf(out, "render: unsupported: %s: %llu\n", why, (unsigned long long)count);
}

unsigned x360_render_texture(x360_machine *x, int *width, int *height)
{
    if (!x->gpu || !x->gpu->gl)
        return 0;
    return xenos_gl_frame_texture(x->gpu->gl, width, height);
}

void x360_render_reset(x360_machine *x)
{
    if (x->gpu && x->gpu->gl)
        xenos_gl_reset(x->gpu->gl);
}

void x360_render_flush(x360_machine *x)
{
    x360_gpu_drain(x, 1); /* what was posted first, then the pending resolves, on the run half's side */
}

void x360_render_set_readback(x360_machine *x, int on)
{
    if (x->gpu && x->gpu->gl)
        xenos_gl_set_readback(x->gpu->gl, on);
}

void x360_render_end_frame(x360_machine *x)
{
    if (x->gpu && x->gpu->gl)
        xenos_gl_end_frame(x->gpu->gl);
}

int x360_render_scale(x360_machine *x)
{
    return x->gpu && x->gpu->gl ? xenos_gl_scale(x->gpu->gl) : 0;
}

const uint8_t *x360_render_frame(x360_machine *x, int *width, int *height)
{
    if (!x->gpu || !x->gpu->gl)
        return NULL;
    return xenos_gl_frame(x->gpu->gl, width, height);
}

void x360_render_counts(x360_machine *x, uint64_t *drawn, uint64_t *frames, uint64_t *front_buffer_frames)
{
    xenos_gl_stats s;
    memset(&s, 0, sizeof s);
    if (x->gpu && x->gpu->gl)
        xenos_gl_get_stats(x->gpu->gl, &s);
    *drawn = s.drawn, *frames = s.frames, *front_buffer_frames = s.front_buffer_frames;
}

void x360_render_perf_get(x360_machine *x, x360_render_perf *out)
{
    memset(out, 0, sizeof *out);
    out->guest_cpu_ns = x->perf.guest_cpu_ns, out->dispatches = x->perf.dispatches, out->turns = x->perf.turns;
    out->idle = x->perf.idle;
    if (!x->gpu)
        return;
    out->gpu_ns = x->gpu->cp_ns;
    out->cpu_ns = x->gpu->cp_cpu_ns;
    out->swaps = x->gpu->cp.swaps;
    out->guest_swaps = x->gpu->host_swaps, out->long_swaps = x->gpu->long_swaps;
    if (!x->gpu->gl)
        return;
    xenos_gl_stats s;
    xenos_gl_get_stats(x->gpu->gl, &s);
    out->resolve_ns = s.resolve_ns, out->read_ns = s.resolve_read_ns, out->write_ns = s.resolve_write_ns;
    out->draws = s.draw_calls, out->resolves = s.resolves, out->pixels = s.resolved_pixels;
    out->vertices = s.vertices_fetched;
    out->noop_draws = s.noop_draws, out->rect_draws = s.rect_draws, out->passes = s.passes;
    out->hashed = s.hashed_bytes;
    out->loads = s.loads, out->gpu_timer = s.gpu_timer;
    out->gpu_time_ns = s.gpu_time_ns, out->gpu_time_frames = s.gpu_time_frames;
}

int x360_render_perf_format(const x360_render_perf *now, const x360_render_perf *since, uint64_t frames,
                            const char *unit, char *out, size_t n)
{
#define D(f) (now->f >= since->f ? now->f - since->f : 0) /* a counter behind the snapshot: none in this window */
    double k = frames ? 1.0 / (double)frames : 0.0, ms = k / 1e6;
    uint64_t noop = D(noop_draws), draws = D(draws);
    return snprintf(out, n,
                    "gpu %.1f (cpu %.1f, resolve %.1f: read %.1f write %.1f; %.0f draws + %.0f no-op, %.1f rects, "
                    "%.0f passes, %.1f resolves, %.1f Mpx, %.1f k vertices, %.1f MB hashed) ms/%s",
                    (double)D(gpu_ns) * ms, (double)D(cpu_ns) * ms, (double)D(resolve_ns) * ms,
                    (double)D(read_ns) * ms, (double)D(write_ns) * ms, (double)(draws > noop ? draws - noop : 0) * k,
                    (double)noop * k, (double)D(rect_draws) * k, (double)D(passes) * k, (double)D(resolves) * k,
                    (double)D(pixels) * k / 1e6, (double)D(vertices) * k / 1e3, (double)D(hashed) * k / 1e6, unit);
#undef D
}

int x360_render_perf_format_guest(const x360_render_perf *now, const x360_render_perf *since, uint64_t frames,
                                  double seconds, const char *unit, char *out, size_t n)
{
#define D(f) (now->f >= since->f ? now->f - since->f : 0)
    double k = frames ? 1.0 / (double)frames : 0.0;
    char gt[32] = "n/a";
    if (now->gpu_timer && D(gpu_time_frames))
        snprintf(gt, sizeof gt, "%.1f ms", (double)D(gpu_time_ns) / (double)D(gpu_time_frames) / 1e6);
    return snprintf(out, n, "guest %.1f ms (%.0f k dispatches, %.0f turns, %.0f idle), swaps %.1f/s (%llu > 20 ms), gpu-time %s, "
                    "loads %.1f per %s",
                    (double)D(guest_cpu_ns) * k / 1e6, (double)D(dispatches) * k / 1e3, (double)D(turns) * k, (double)D(idle) * k,
                    seconds > 0 ? (double)D(guest_swaps) / seconds : 0.0, (unsigned long long)D(long_swaps), gt,
                    (double)D(loads) * k, unit);
#undef D
}

uint64_t x360_render_programs(x360_machine *x)
{
    xenos_gl_stats s;
    if (!x->gpu || !x->gpu->gl)
        return 0;
    xenos_gl_get_stats(x->gpu->gl, &s);
    return s.programs;
}

void x360_set_shader_dump(x360_machine *x, const char *dir)
{
    struct x360_gpu *g = gpu_of(x);
    if (g) {
        snprintf(g->shader_dir, sizeof g->shader_dir, "%s", dir ? dir : "");
        dumped_reset(g); /* another directory: what the old one holds says nothing */
    }
}

void x360_gpu_opcode_histogram(x360_machine *x, FILE *out)
{
    struct x360_gpu *g = x->gpu;
    if (!g || !g->cp.packets)
        return;
    fprintf(out, "gpu opcodes:");
    for (uint32_t op = 0; op < 128; op++)
        if (g->cp.op_count[op]) {
            const char *n = xenos_cp_opcode_name(op);
            if (n)
                fprintf(out, " %s %u", n, g->cp.op_count[op]);
            else
                fprintf(out, " op%02X %u", op, g->cp.op_count[op]);
        }
    if (g->cp.predicated)
        fprintf(out, "; predicated %llu, skipped %llu", (unsigned long long)g->cp.predicated,
                (unsigned long long)g->cp.predicated_skipped);
    fprintf(out, "\n");
}

/* the run half's access to guest memory: plain loads and stores, as a bus master's. phys_write (with the renderer's
 * direct stores into cp.wmem for resolves) is the seam's store path: every guest-visible effect of the run half but
 * interrupts goes through one of the two. */
static uint32_t phys_read(void *ctx, uint32_t phys)
{
    x360_machine *x = ctx;
    return x360_r32(x, x360_p2v(X360_VIEW_64K, phys & 0x1FFFFFFFu));
}

static void phys_write(void *ctx, uint32_t phys, uint32_t v)
{
    x360_machine *x = ctx;
    x360_w32(x, x360_p2v(X360_VIEW_64K, phys & 0x1FFFFFFFu), v);
}

/* PM4_INTERRUPT (run half): not raise_irq itself, which makes or restarts a guest thread, but a bit in the seam's
 * interrupt word; the post half turns it into raise_irq (gpu_take_irq) */
static void cp_interrupt(void *ctx, uint32_t source, uint32_t cpu)
{
    x360_machine *x = ctx;
    if (x->gpu)
        __atomic_fetch_or(&x->gpu->link.irq, source & 1 ? LINK_IRQ_SRC1 | 1u << (cpu % 6) : LINK_IRQ_SRC0,
                          __ATOMIC_RELEASE);
}

static struct x360_gpu *gpu_of(x360_machine *x)
{
    if (!x->gpu) {
        x->gpu = calloc(1, sizeof *x->gpu);
        if (!x->gpu)
            return NULL;
        x->gpu->cp.ctx = x;
        x->gpu->cp.read32 = phys_read;
        x->gpu->cp.write32 = phys_write;
        x->gpu->cp.interrupt = cp_interrupt;
        x->gpu->irq_thread = -1;
        x->gpu->cp.shader_loaded = shader_loaded;
        x->gpu->cp.mem = x360_p(x, X360_VIEW_64K); /* physical memory, mapped whole (x360_mem.c) */
        x->gpu->cp.wmem = x360_p(x, X360_VIEW_64K); /* the same, for resolves' stores (what phys_write does) */
        xenos_cp_init(&x->gpu->cp);
        gpu_overlap *o = &x->gpu->ov;
        o->ok = !pthread_mutex_init(&o->mu, NULL) && !pthread_cond_init(&o->work, NULL) &&
                !pthread_cond_init(&o->done, NULL) && !pthread_cond_init(&o->job_cv, NULL);
    }
    return x->gpu;
}

int x360_gpu_init(x360_machine *x)
{
    /* the register page: plain memory the title writes and this file reads */
    return x360_commit(x, GPU_MMIO, 0x10000);
}

void x360_gpu_free(x360_machine *x)
{
    if (x->gpu) {
        gpu_overlap *o = &x->gpu->ov;
        if (o->cpu_made) { /* the worker is idle between jobs: tell it to end */
            pthread_mutex_lock(&o->mu);
            o->quit = 1;
            pthread_cond_signal(&o->job_cv);
            pthread_mutex_unlock(&o->mu);
            pthread_join(o->cpu, NULL);
        }
        if (o->ok) {
            pthread_mutex_destroy(&o->mu);
            pthread_cond_destroy(&o->work);
            pthread_cond_destroy(&o->done);
            pthread_cond_destroy(&o->job_cv);
        }
        xenos_gl_destroy(x->gpu->gl);
        dumped_reset(x->gpu);
    }
    free(x->gpu);
    x->gpu = NULL;
}

void x360_set_gpu_trace(x360_machine *x, FILE *f)
{
    struct x360_gpu *g = gpu_of(x);
    if (g)
        g->cp.trace = f;
}

static void raise_irq(x360_machine *x, struct x360_gpu *g, uint32_t source)
{
    if (!g->irq_callback)
        return;
    /* one interrupt context, made once and reused: an interrupt raised while it runs waits for it to end */
    x360_thread *t = g->irq_thread >= 0 ? &x->threads[g->irq_thread] : NULL;
    if (t && t->state != TH_DONE) {
        g->irq_pending[source & 1] = 1;
        return;
    }
    /* the CPU it runs as: the lowest one pending for the command processor (the others when this one's handler
     * ended: one callback per CPU), 2 for the vertical blank (and for a state from before the mask) */
    uint32_t cpu = 2;
    if ((source & 1) && g->irq_cpus) {
        cpu = (uint32_t)__builtin_ctz(g->irq_cpus);
        g->irq_cpus &= g->irq_cpus - 1;
        if (g->irq_cpus)
            g->irq_pending[1] = 1;
    }
    if (t) {
        if (x360_thread_restart(x, t, g->irq_callback, source, g->irq_user))
            return;
    } else {
        t = x360_thread_create(x, 0x4000, g->irq_callback, source, g->irq_user, 0);
        if (!t)
            return;
        t->interrupt = 1;
        g->irq_thread = (int)(t - x->threads);
    }
    x360_w8(x, t->kthread + 0xBF, (uint8_t)cpu);
    x360_w8(x, t->pcr + 0x10C, (uint8_t)cpu);
}

/* ---- the two halves (WP-22) ---- */

/* the interrupts the run half asked for since the last call, into the post half's queue (as cp_interrupt did before
 * the split, at the same point of the poll: right after the packets ran) */
static void gpu_take_irq(struct x360_gpu *g)
{
    /* nearly every turn there is none: a plain load first, not a locked exchange (a flag set meanwhile is taken at
     * the next poll, as if it had come a moment later; without the GPU thread nothing sets it between two polls) */
    if (!__atomic_load_n(&g->link.irq, __ATOMIC_RELAXED))
        return;
    uint32_t f = __atomic_exchange_n(&g->link.irq, 0, __ATOMIC_ACQUIRE);
    if (f & LINK_IRQ_SRC0)
        g->irq_pending[0] = 1;
    if (f & LINK_IRQ_SRC1) {
        g->irq_pending[1] = 1;
        g->irq_cpus |= f & LINK_IRQ_CPUS;
    }
}

/* the seam after the guest side replaced the run half's state (a ring initialised, a state loaded), from the command
 * processor: its stalled flag; what was posted before is dropped (it was for the old ring / the old timeline), except
 * that after a load a post is made pending again when the loaded command processor is behind the loaded write
 * pointer (a state saved where the drain could not run: posted, not run). A drained state has cp.wptr == last_wptr. */
static void link_sync(struct x360_gpu *g, int loaded)
{
    uint64_t post = __atomic_load_n(&g->link.post, __ATOMIC_RELAXED);
    uint32_t seq = (uint32_t)(post >> 32), size_dw = g->cp.rb_size / 4;
    __atomic_store_n(&g->link.stalled, (uint32_t)(g->cp.stalled != 0), __ATOMIC_RELAXED);
    __atomic_store_n(&g->link.swaps, g->cp.swaps, __ATOMIC_RELAXED); /* a load sets cp.swaps: not swaps seen */
    g->swaps_seen = g->cp.swaps;
    g->swaps_issued = g->cp.swaps; /* stage 2's back-pressure counts from here */
    g->lag_t = 0;
    if (loaded && g->ring && size_dw && g->cp.wptr != g->last_wptr % size_dw)
        __atomic_store_n(&g->link.post, (uint64_t)(seq + 1) << 32 | g->last_wptr, __ATOMIC_RELEASE);
    else
        __atomic_store_n(&g->link.taken, seq, __ATOMIC_RELAXED);
}

/* Run half: the packets up to the write pointer last posted (draws, resolves, swaps through the renderer's hooks).
 * Reads only the seam's post word and guest memory; answers through guest memory, link.stalled and link.irq.
 * Returns 1 when there was a post to run. Stage 1: called by x360_gpu_poll right after the post (and by
 * x360_gpu_drain); a later stage calls it on the GL thread. */
/* WP-31: the thread CPU time spent in x360_gpu_run so far (x360_run takes it out of the perf line's guest time) */
uint64_t x360_gpu_cpu_ns(x360_machine *x)
{
    /* only runs on the guest's own thread: stage 2's runs on the GL thread are not in that thread's CPU time */
    return x->gpu ? __atomic_load_n(&x->gpu->inline_cpu_ns, __ATOMIC_RELAXED) : 0;
}

int x360_gpu_run(x360_machine *x)
{
    struct x360_gpu *g = x->gpu;
    if (!g)
        return 0;
    uint64_t post = __atomic_load_n(&g->link.post, __ATOMIC_ACQUIRE);
    if ((uint32_t)(post >> 32) == __atomic_load_n(&g->link.taken, __ATOMIC_RELAXED))
        return 0;
    if (!g->owner_set)
        g->owner = pthread_self(), g->owner_set = 1;
    uint32_t w = (uint32_t)post;
    /* WP-31: the perf line's gpu / cpu time, only when packets run. A post that only retries a stalled wait (write
     * pointer where the ring already was) whose condition still does not hold stops at the same wait again: it
     * happens every scheduler turn while the wait lasts, and is not timed (4 clock reads, a system call on Linux). */
    uint32_t size_dw = g->cp.rb_size / 4;
    if (g->cp.stalled && size_dw && w % size_dw == g->cp.wptr && !xenos_cp_stall_may_end(&g->cp)) {
        xenos_cp_set_wptr(&g->cp, w);
    } else {
        uint64_t t0 = mono_ns(), c0 = thread_cpu_ns();
        xenos_cp_set_wptr(&g->cp, w);
        uint64_t cpu = thread_cpu_ns() - c0;
        g->cp_ns += mono_ns() - t0;
        g->cp_cpu_ns += cpu;
        if (!__atomic_load_n(&g->ov.active, __ATOMIC_RELAXED))
            __atomic_store_n(&g->inline_cpu_ns, g->inline_cpu_ns + cpu, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g->link.stalled, (uint32_t)(g->cp.stalled != 0), __ATOMIC_RELAXED);
    __atomic_store_n(&g->link.swaps, g->cp.swaps, __ATOMIC_RELAXED);
    __atomic_store_n(&g->link.taken, (uint32_t)(post >> 32), __ATOMIC_RELEASE); /* drain waits on this one */
    return 1;
}

/* WP-32: 1 when posting the same write pointer again for a stalled ring would do nothing: the run half runs here
 * (not deferred), nothing posted is waiting, the retry would stop at the same wait (xenos_cp_stall_may_end: same
 * packet, condition still false) and the read pointer it writes back is already there. Then the poll does not post;
 * the wait is looked at again at the next one (every scheduler turn), as the retry would have. */
static int stall_retry_core(x360_machine *x, struct x360_gpu *g, uint32_t w)
{
    uint32_t size_dw = g->cp.rb_size / 4;
    if (!g->cp.stalled || !size_dw || w % size_dw != g->cp.wptr)
        return 0;
    uint64_t post = __atomic_load_n(&g->link.post, __ATOMIC_RELAXED);
    if ((uint32_t)(post >> 32) != __atomic_load_n(&g->link.taken, __ATOMIC_RELAXED))
        return 0;
    if (g->cp.rptr_writeback && g->cp.write32 && phys_read(x, g->cp.rptr_writeback & 0x1FFFFFFFu) != g->cp.rptr)
        return 0;
    return !xenos_cp_stall_may_end(&g->cp);
}

/* Stage 2: a job runs on the CPU worker and this is that thread (x360_gpu_overlap) */
static int overlap_remote(struct x360_gpu *g)
{
    return __atomic_load_n(&g->ov.active, __ATOMIC_ACQUIRE) && !pthread_equal(pthread_self(), g->ov.gl);
}

/* the same, from the post side. Stage 2: the command processor is the GL thread's. While it runs (or a post waits
 * for it) a retry is useless: that run ends with the wait looked at again, and the next poll retries if it still
 * stalls. While it is idle it starts nothing without a post, which only this thread makes, nor while held: under the
 * lock the command processor can be read here. */
static int stall_retry_useless(x360_machine *x, struct x360_gpu *g, uint32_t w)
{
    if (g->deferred)
        return 0;
    if (!overlap_remote(g))
        return stall_retry_core(x, g, w);
    gpu_overlap *o = &g->ov;
    pthread_mutex_lock(&o->mu);
    uint64_t post = __atomic_load_n(&g->link.post, __ATOMIC_RELAXED);
    int r = o->running || o->held || (uint32_t)(post >> 32) != __atomic_load_n(&g->link.taken, __ATOMIC_ACQUIRE)
                ? 1
                : stall_retry_core(x, g, w);
    pthread_mutex_unlock(&o->mu);
    return r;
}

/* Stage 2, on the CPU worker: wake the GL thread for the post just made; then back-pressure: while more than
 * GPU_QUEUE_SWAPS swaps are issued and not run, wait for the GL thread, unless it stalled on a wait (which may need
 * this thread's progress) or has nothing posted left (the swaps are ring words not posted yet). */
#define GPU_QUEUE_SWAPS 2
static void overlap_posted(struct x360_gpu *g)
{
    gpu_overlap *o = &g->ov;
    pthread_mutex_lock(&o->mu);
    pthread_cond_signal(&o->work);
    /* signed: after a load (link_sync re-bases swaps_issued on cp.swaps) swaps posted before it can run past it */
    while ((o->lockstep || ((int64_t)(g->swaps_issued - __atomic_load_n(&g->link.swaps, __ATOMIC_RELAXED)) >
                                GPU_QUEUE_SWAPS &&
                            !__atomic_load_n(&g->link.stalled, __ATOMIC_RELAXED))) &&
           (o->running || (uint32_t)(__atomic_load_n(&g->link.post, __ATOMIC_RELAXED) >> 32) !=
                              __atomic_load_n(&g->link.taken, __ATOMIC_ACQUIRE)))
        pthread_cond_wait(&o->done, &o->mu);
    pthread_mutex_unlock(&o->mu);
}

/* Post half, first part: the registers titles poll, and the write pointer posted when the title moved it (or the
 * run half stalled on a wait that may hold now: it looks again). Guest thread. */
static void gpu_post(x360_machine *x, struct x360_gpu *g, uint64_t now)
{
    /* registers titles poll: the vertical counter, the display interrupt status, the viewport size. The status
     * reads 1 (the vertical blank occurred), as in Xenia: Direct3D's interrupt handler tests that bit before it
     * runs its vertical blank routine, the only display interrupt this runtime raises. */
    x360_w32(x, GPU_MMIO + 4 * 0x194C, (uint32_t)(now % VBLANK_100NS * 750 / VBLANK_100NS));
    x360_w32(x, GPU_MMIO + 4 * 0x1951, 1);
    x360_w32(x, GPU_MMIO + 4 * 0x1961, 1280u << 16 | 720u);
    if (g->ring) {
        uint32_t w = x360_r32(x, GPU_MMIO + 4 * XENOS_REG_CP_RB_WPTR);
        if (w != g->last_wptr ||
            (__atomic_load_n(&g->link.stalled, __ATOMIC_RELAXED) && !stall_retry_useless(x, g, w))) {
            g->last_wptr = w;
            uint64_t post = __atomic_load_n(&g->link.post, __ATOMIC_RELAXED);
            /* release: the ring's packets (and what they point at), stored by the guest before it moved the write
             * pointer, are visible to the run half before the post is */
            __atomic_store_n(&g->link.post, ((post >> 32) + 1) << 32 | w, __ATOMIC_RELEASE);
            if (overlap_remote(g))
                overlap_posted(g);
        }
    }
}

/* Post half, second part: the run half's interrupts, then the vertical blank, then raise_irq for what is queued */
static void gpu_post_irq(x360_machine *x, struct x360_gpu *g, uint64_t now)
{
    uint64_t sw = __atomic_load_n(&g->link.swaps, __ATOMIC_RELAXED);
    if (sw != g->swaps_seen) { /* the clock does not move inside the ring: one swap time for all of them */
        g->host_swaps += sw - g->swaps_seen;
        if (g->last_swap_vt && now - g->last_swap_vt > LONG_SWAP_100NS)
            g->long_swaps++;
        g->last_swap_vt = now;
        g->swaps_seen = sw;
    }
    gpu_take_irq(g);
    if (g->irq_callback && now >= g->next_vblank) {
        g->next_vblank = now + VBLANK_100NS - (now - g->next_vblank) % VBLANK_100NS;
        g->vblanks++;
        raise_irq(x, g, 0);
    }
    for (uint32_t s = 0; s < 2; s++)
        if (g->irq_pending[s]) {
            g->irq_pending[s] = 0;
            raise_irq(x, g, s);
        }
}

/* Stage 2, an unpaced job (no run_until: x360_run without --swaps): virtual time is the guest's instructions, and a
 * guest that spins on a fence (or a read pointer) while the GL thread takes host time over its work would burn
 * virtual frames, seeing the GPU many frames late. Posted work is allowed to wait at most this long in virtual time;
 * past it the worker waits for the GL thread to run everything posted (a run stopped at an unmet wait counts as run:
 * that wait needs the guest). The player's paced frames keep their swap back-pressure only. */
#define GPU_LAG_100NS (VBLANK_100NS / 2)
static void overlap_lag(x360_machine *x, struct x360_gpu *g, uint64_t now)
{
    if ((uint32_t)(__atomic_load_n(&g->link.post, __ATOMIC_RELAXED) >> 32) ==
        __atomic_load_n(&g->link.taken, __ATOMIC_ACQUIRE)) {
        g->lag_t = 0;
        return;
    }
    if (!g->lag_t) {
        g->lag_t = now + 1;
        return;
    }
    if (now + 1 - g->lag_t <= GPU_LAG_100NS)
        return;
    x360_gpu_wait_posted(x);
    g->lag_t = 0;
}

void x360_gpu_poll(x360_machine *x)
{
    struct x360_gpu *g = x->gpu;
    if (!g)
        return;
    uint64_t now = x360_now(x);
    gpu_post(x, g, now);
    if (!g->deferred && !overlap_remote(g))
        x360_gpu_run(x); /* stage 1: the run half right here; stage 2: gpu_post woke the GL thread */
    else if (!x->run_until && overlap_remote(g))
        overlap_lag(x, g, now);
    gpu_post_irq(x, g, now);
}

void x360_gpu_set_deferred(x360_machine *x, int on)
{
    struct x360_gpu *g = gpu_of(x);
    if (g)
        g->deferred = on != 0;
}

/* ---- stage 2: the run half on the GL thread, the machine on a CPU worker (WP-22) ---- */

static void *cpu_worker(void *arg)
{
    x360_machine *x = arg;
    gpu_overlap *o = &x->gpu->ov;
    if (o->worker_hook)
        o->worker_hook(1, o->worker_user);
    pthread_mutex_lock(&o->mu);
    for (;;) {
        while (!o->job_pending && !o->quit)
            pthread_cond_wait(&o->job_cv, &o->mu);
        if (o->quit)
            break;
        o->job_pending = 0;
        pthread_mutex_unlock(&o->mu);
        int rc = o->job(x, o->arg);
        pthread_mutex_lock(&o->mu);
        o->job_rc = rc;
        o->job_done = 1;
        pthread_cond_signal(&o->work);
    }
    pthread_mutex_unlock(&o->mu);
    if (o->worker_hook)
        o->worker_hook(0, o->worker_user);
    return NULL;
}

void x360_gpu_set_worker_hook(x360_machine *x, void (*hook)(int starting, void *user), void *user)
{
    struct x360_gpu *g = gpu_of(x);
    if (g && !g->ov.cpu_made)
        g->ov.worker_hook = hook, g->ov.worker_user = user;
}

int x360_gpu_overlap_wanted(void)
{
    /* off by default until checked on a device (gain unmeasured there; runs not reproducible against stage 1) */
    const char *e = getenv("RECOMP_X360_GPU_THREAD");
    return e && *e == '1';
}

/* Runs job(x, arg) on the CPU worker while this thread (the renderer's: its GL context is current here) runs the
 * command processor and the renderer for what the job posts, and returns the job's result. Between jobs the worker
 * is parked and nothing runs on it: callers keep doing what they did between x360_run_until calls (states, rewind,
 * cheats, audio) on their own thread. Posted work may be left over when the job ends; this thread's next job, drain
 * (x360_state_save / _load, x360_render_flush) or x360_gpu_run runs it. Falls back to job(x, arg) right here when the
 * run half belongs to another thread, the GPU is deferred (tests), guest threads run on host threads, or the worker
 * cannot be made. */
int x360_gpu_overlap(x360_machine *x, int (*job)(x360_machine *, uint64_t), uint64_t arg)
{
    struct x360_gpu *g = gpu_of(x);
    if (!g || !g->ov.ok || g->deferred || x->host || __atomic_load_n(&g->ov.active, __ATOMIC_RELAXED) ||
        (g->owner_set && !pthread_equal(pthread_self(), g->owner)))
        return job(x, arg);
    gpu_overlap *o = &g->ov;
    if (!o->cpu_made) {
        pthread_attr_t at;
        int made = 0;
        if (!pthread_attr_init(&at)) {
            pthread_attr_setstacksize(&at, 16u << 20); /* lifted code: up to XE_CALL_DEPTH direct calls deep */
            made = !pthread_create(&o->cpu, &at, cpu_worker, x);
            pthread_attr_destroy(&at);
        }
        if (!made)
            return job(x, arg);
        o->cpu_made = 1;
    }
    if (!g->owner_set)
        g->owner = pthread_self(), g->owner_set = 1;
    const char *ls = getenv("RECOMP_X360_GPU_LOCKSTEP");
    pthread_mutex_lock(&o->mu);
    o->lockstep = ls && *ls == '1';
    o->gl = pthread_self();
    o->job = job, o->arg = arg, o->job_done = 0, o->job_pending = 1;
    __atomic_store_n(&o->active, 1, __ATOMIC_RELEASE);
    pthread_cond_signal(&o->job_cv);
    for (;;) {
        uint64_t post = __atomic_load_n(&g->link.post, __ATOMIC_ACQUIRE);
        if (!o->held && (uint32_t)(post >> 32) != __atomic_load_n(&g->link.taken, __ATOMIC_RELAXED)) {
            o->running = 1;
            pthread_mutex_unlock(&o->mu);
            x360_gpu_run(x);
            pthread_mutex_lock(&o->mu);
            o->running = 0;
            pthread_cond_broadcast(&o->done);
            continue;
        }
        if (o->flush_req) { /* after the posts: a drain's flush sees every resolve they made */
            o->flush_req = 0;
            if (g->gl) {
                o->running = 1;
                pthread_mutex_unlock(&o->mu);
                xenos_gl_flush_resolves(g->gl, &g->cp);
                pthread_mutex_lock(&o->mu);
                o->running = 0;
                pthread_cond_broadcast(&o->done);
            }
            continue;
        }
        if (o->drain_ack != o->drain_req) {
            o->drain_ack = o->drain_req;
            pthread_cond_broadcast(&o->done);
            continue;
        }
        if (o->job_done)
            break;
        pthread_cond_wait(&o->work, &o->mu);
    }
    __atomic_store_n(&o->active, 0, __ATOMIC_RELEASE);
    int rc = o->job_rc;
    pthread_mutex_unlock(&o->mu);
    return rc;
}

/* Stage 2, on the CPU worker, from the scheduler when every guest thread waits and it would jump the clock to the
 * next deadline: posted work the GL thread has not run yet (or is running) may be what a thread waits on (a fence,
 * the read pointer, a swap's interrupt), so jumping now would let the guest see GPU results late by however much
 * virtual time the jump skips. Waits for the GL thread to run everything posted (not a drain: no flush, no
 * stalled-wait retry) and returns 1 when it waited (the scheduler then polls again instead of jumping), 0 when
 * there was nothing to wait for (and always when no job runs on the worker: stage 1, the off path, is untouched).
 * Ends: the GL thread serves posts until the job ends, and the job is this thread; held is only set inside a Vd*
 * call on this thread, never here; a run that stops at an unmet wait still sets taken. */
int x360_gpu_wait_posted(x360_machine *x)
{
    struct x360_gpu *g = x->gpu;
    if (!g || !overlap_remote(g))
        return 0;
    gpu_overlap *o = &g->ov;
    int waited = 0;
    pthread_mutex_lock(&o->mu);
    while (!o->held && (o->running || (uint32_t)(__atomic_load_n(&g->link.post, __ATOMIC_RELAXED) >> 32) !=
                                          __atomic_load_n(&g->link.taken, __ATOMIC_ACQUIRE))) {
        waited = 1;
        pthread_cond_signal(&o->work);
        pthread_cond_wait(&o->done, &o->mu);
    }
    pthread_mutex_unlock(&o->mu);
    return waited;
}

/* Everything posted run to its end, and (flush_resolves) the lazy resolves written to guest memory, the interrupts
 * the run half raised queued on the post side: afterwards guest memory and the GPU state are what a save state takes,
 * as long as the post side posts nothing new (the caller holds the machine: s_mu, x360_run not running). Stage 1:
 * the run half is called here; a later stage hands the request to the GL thread and waits for it.
 * Only on the run half's thread: elsewhere (an app's menu or auto-save thread, no GL context) nothing runs and
 * nothing is flushed; posted work stays posted (a state then has the command processor behind its write pointer,
 * and link_sync posts again after loading it) and lazy resolves stay pending (the title resolves them again). */
void x360_gpu_drain(x360_machine *x, int flush_resolves)
{
    struct x360_gpu *g = x->gpu;
    if (!g)
        return;
    if (overlap_remote(g)) { /* stage 2, on the CPU worker: the GL thread drains, this thread waits for it */
        gpu_overlap *o = &g->ov;
        /* A wait that stalled while this thread opened it (the GL thread read the word before the store, and the
         * poll after the store skipped the retry: it was still running) is retried here, as stage 1's poll would
         * have at once: posted again while its condition may hold, a few rounds (each may stop at the next wait). */
        for (int round = 0; round < 4; round++) {
            pthread_mutex_lock(&o->mu);
            uint32_t mine = ++o->drain_req;
            if (flush_resolves)
                o->flush_req = 1;
            pthread_cond_signal(&o->work);
            while ((int32_t)(o->drain_ack - mine) < 0)
                pthread_cond_wait(&o->done, &o->mu);
            pthread_mutex_unlock(&o->mu);
            if (round == 3 || !g->ring || !__atomic_load_n(&g->link.stalled, __ATOMIC_ACQUIRE) ||
                stall_retry_useless(x, g, g->last_wptr))
                break; /* the last round posts nothing it would not drain */
            uint64_t post = __atomic_load_n(&g->link.post, __ATOMIC_RELAXED);
            __atomic_store_n(&g->link.post, ((post >> 32) + 1) << 32 | g->last_wptr, __ATOMIC_RELEASE);
        }
        gpu_take_irq(g);
        return;
    }
    if (g->owner_set && !pthread_equal(pthread_self(), g->owner)) {
        gpu_take_irq(g);
        return;
    }
    x360_gpu_run(x);
    if (flush_resolves && g->gl)
        xenos_gl_flush_resolves(g->gl, &g->cp);
    gpu_take_irq(g);
}

/* A Vd* call that changes the run half's state from the guest side, between gpu_hold and gpu_release: drained first
 * where the drain can run, so nothing posted runs against the state changed under it; a later stage also takes the
 * run half's mutex here. replaced (the ring initialised): link_sync drops what was posted for the old ring. States
 * do not use the bracket: x360_state_save / state_load drain at their start, x360_gpu_state calls link_sync. */
static void gpu_hold(x360_machine *x, struct x360_gpu *g)
{
    x360_gpu_drain(x, 0);
    if (overlap_remote(g)) { /* stage 2: the GL thread is idle after the drain; it starts nothing until the release */
        pthread_mutex_lock(&g->ov.mu);
        g->ov.held = 1;
        while (g->ov.running) /* nothing should be running after the drain; never change state under a run */
            pthread_cond_wait(&g->ov.done, &g->ov.mu);
        pthread_mutex_unlock(&g->ov.mu);
    }
}

static void gpu_release(struct x360_gpu *g, int replaced)
{
    if (replaced)
        link_sync(g, 0);
    if (overlap_remote(g)) {
        pthread_mutex_lock(&g->ov.mu);
        g->ov.held = 0;
        pthread_cond_signal(&g->ov.work);
        pthread_mutex_unlock(&g->ov.mu);
    }
}

/* save states (x360_state.c): the command processor and the interrupt state, field by field */
void x360_gpu_state(x360_machine *x, void *s, void (*u32)(void *, uint32_t *), void (*u64)(void *, uint64_t *),
                    void (*i32)(void *, int *), uint32_t version, int load)
{
    struct x360_gpu *g = gpu_of(x);
    xenos_cp *cp = &g->cp;
    /* no gpu_hold: x360_state_save / state_load drained at their start (where a drain can run); a drain here, between
     * sections, would run posted work against a half-loaded machine */
    if (load)
        g->last_swap_vt = 0; /* the clock jumps: the next swap's interval is not one */
    for (uint32_t k = 0; k < XENOS_REGS; k++)
        u32(s, &cp->regs[k]);
    u32(s, &cp->rb_base);
    u32(s, &cp->rb_size);
    u32(s, &cp->rptr);
    u32(s, &cp->wptr);
    u32(s, &cp->rptr_writeback);
    u32(s, &cp->counter);
    i32(s, &cp->stalled);
    cp->wait_known = 0; /* WP-31/32: the stalled wait is read again at the next retry */
    u32(s, &cp->ib_depth);
    if (cp->ib_depth > XENOS_IB_DEPTH)
        cp->ib_depth = 0;
    for (uint32_t k = 0; k < XENOS_IB_DEPTH; k++) {
        u32(s, &cp->ib[k].base);
        u32(s, &cp->ib[k].len);
        u32(s, &cp->ib[k].pos);
        u32(s, &cp->ib[k].done);
        if (!cp->ib[k].len && k < cp->ib_depth)
            cp->ib_depth = 0; /* a corrupt state must not divide by zero */
    }
    u32(s, &cp->last_swap_fb);
    u32(s, &cp->last_swap_w);
    u32(s, &cp->last_swap_h);
    for (int k = 0; k < 2; k++) {
        xenos_shader *sh = &cp->shader[k];
        u32(s, &sh->words);
        u32(s, &sh->address);
        u32(s, &sh->loads);
        if (sh->words > XENOS_SHADER_WORDS)
            sh->words = XENOS_SHADER_WORDS;
        for (uint32_t w = 0; w < sh->words; w++)
            u32(s, &sh->code[w]);
    }
    u64(s, &cp->packets);
    u64(s, &cp->draws);
    u64(s, &cp->swaps);
    u32(s, &g->irq_callback);
    u32(s, &g->irq_user);
    u64(s, &g->next_vblank);
    u32(s, &g->vblanks);
    u32(s, &g->last_wptr);
    i32(s, &g->irq_pending[0]);
    i32(s, &g->irq_pending[1]);
    i32(s, &g->ring);
    i32(s, &g->irq_thread);
    if (g->irq_thread >= X360_MAX_THREADS) /* a damaged state: made again at the next interrupt */
        g->irq_thread = -1;
    if (version >= 2) { /* version 2: the CPUs still to interrupt */
        u32(s, &g->irq_cpus);
        g->irq_cpus &= 0x3F;
    } else if (load)
        g->irq_cpus = 0;
    if (load)
        link_sync(g, 1); /* the seam follows the loaded command processor */
}

uint64_t x360_gpu_next_event(x360_machine *x)
{
    return x->gpu && x->gpu->irq_callback ? x->gpu->next_vblank : 0;
}

/* save states, section GBIN: tiled rendering's bin registers. A state from before them has no such section and
 * loads with what the command processor starts with (x360_gpu_bin_reset). */
void x360_gpu_bin_reset(x360_machine *x)
{
    struct x360_gpu *g = x->gpu;
    if (g)
        g->cp.bin_mask = g->cp.bin_select = 0xFFFFFFFFu; /* in state_load, drained (see x360_gpu_state) */
}

void x360_gpu_bin_state(x360_machine *x, void *s, void (*u64)(void *, uint64_t *))
{
    struct x360_gpu *g = gpu_of(x); /* drained by x360_state_save / state_load (see x360_gpu_state) */
    u64(s, &g->cp.bin_mask);
    u64(s, &g->cp.bin_select);
}

/* packets, draws and swaps are the run half's counters: read as they stand (a later stage: after a drain, or racy for
 * the stats line only) */
void x360_gpu_stats(x360_machine *x, uint64_t *packets, uint64_t *draws, uint64_t *swaps, uint32_t *vblanks)
{
    struct x360_gpu *g = x->gpu;
    *packets = g ? g->cp.packets : 0;
    *draws = g ? g->cp.draws : 0;
    *swaps = g ? g->cp.swaps : 0;
    *vblanks = g ? g->vblanks : 0;
}

/* ---- the Vd* calls (signatures from Xenia's xboxkrnl_video.cc) ---- */

void x360_k_VdInitializeRingBuffer(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    struct x360_gpu *g = gpu_of(x);
    gpu_hold(x, g);
    xenos_cp_ring_init(&g->cp, A(0), A(1)); /* (physical address, log2 of the size in quadwords) */
    gpu_release(g, 1);
    g->ring = 1;
    g->last_wptr = 0;
    x360_w32(x, GPU_MMIO + 4 * XENOS_REG_CP_RB_WPTR, 0);
    RET(0);
}

void x360_k_VdEnableRingBufferRPtrWriteBack(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    struct x360_gpu *g = gpu_of(x);
    gpu_hold(x, g);
    g->cp.rptr_writeback = A(0);
    gpu_release(g, 0);
    RET(0);
}

void x360_k_VdSetGraphicsInterruptCallback(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    struct x360_gpu *g = gpu_of(x);
    g->irq_callback = A(0);
    g->irq_user = A(1);
    g->next_vblank = x360_now(x) + VBLANK_100NS;
    RET(0);
}

void x360_k_VdSwap(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (ring pointer, fetch constant, ..., front buffer*, format*, color space*, width*, height*): the last two in
     * the caller's stack slots. The 64 words reserved in the ring get the swap token, then a NOP over the rest. */
    uint32_t buf = A(0), fb_ptr = A(5), w_ptr = (uint32_t)x360_stack_arg(x, c, 8),
             h_ptr = (uint32_t)x360_stack_arg(x, c, 9);
    struct x360_gpu *g = gpu_of(x);
    if (g)
        g->swaps_issued++; /* stage 2's back-pressure (overlap_posted) */
    uint32_t fb = fb_ptr ? x360_r32(x, fb_ptr) : 0;
    memset(x360_p(x, buf), 0, 64 * 4);
    /* as Xenia's VdSwap: the front buffer's texture fetch constant (from the Direct3D texture header, its base made
     * physical) into fetch constant 0, then the token */
    uint32_t fetch = A(1), at = buf;
    if (fetch) {
        x360_w32(x, at, 0u << 30 | (6u - 1) << 16 | 0x4800u);
        for (uint32_t k = 0; k < 6; k++) {
            uint32_t v = x360_r32(x, fetch + 4 * k);
            if (k == 1)
                v = (v & 0xFFFu) | x360_v2p(v & 0xFFFFF000u); /* a pointer into a physical view */
            x360_w32(x, at + 4 + 4 * k, v);
        }
        at += 28;
    }
    x360_w32(x, at, 3u << 30 | (4u - 1) << 16 | (uint32_t)PM4_XE_SWAP << 8);
    x360_w32(x, at + 4, 0x53574150u); /* 'SWAP' */
    x360_w32(x, at + 8, x360_v2p(fb));
    x360_w32(x, at + 12, w_ptr ? x360_r32(x, w_ptr) : 0);
    x360_w32(x, at + 16, h_ptr ? x360_r32(x, h_ptr) : 0);
    uint32_t used = (at + 20 - buf) / 4; /* the rest of the 64 words: one NOP packet */
    x360_w32(x, at + 20, 3u << 30 | (64u - used - 1 - 1) << 16 | (uint32_t)PM4_NOP << 8);
    RET(0);
}

void x360_k_VdGetSystemCommandBuffer(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0)) {
        memset(x360_p(x, A(0)), 0, 0x94);
        x360_w32(x, A(0), 0xBEEF0000u);
    }
    if (A(1))
        x360_w32(x, A(1), 0xBEEF0001u);
    RET(0);
}

void x360_k_VdInitializeEngines(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(1);
}

void x360_k_VdIsHSIOTrainingSucceeded(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(1);
}

void x360_k_VdGetCurrentDisplayGamma(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    float p = 2.22222233f;
    uint32_t bits;
    memcpy(&bits, &p, 4);
    if (A(0))
        x360_w32(x, A(0), 2); /* TV (BT.709), Xenia's default */
    if (A(1))
        x360_w32(x, A(1), bits);
    RET(0);
}

void x360_k_VdPersistDisplay(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(1)) {
        uint32_t p = x360_heap_alloc(x, &x->heap_phys, 0x1000, 0);
        x360_w32(x, A(1), p ? x360_p2v(X360_VIEW_4K, p) : 0);
    }
    RET(0);
}

void x360_k_VdNop0(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}
