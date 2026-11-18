/* x360_gpu.c -- the GPU as the CPU sees it (XBA-270): the Vd* kernel calls, the register page and the command
 * processor (xenos_cp.c) fed from the title's ring buffer. No rendering yet (XBA-229 step 3).
 *
 * The GPU's registers sit at 0x7FC80000 + 4 * index (Xenia's MMIO range). Here that page is ordinary memory: a
 * title's store to CP_RB_WPTR lands there and the scheduler, between time slices, sees the write pointer move and
 * runs the new packets (x360_gpu_poll); the read pointer goes back through the write-back address the title gave.
 * Registers titles poll get plausible values written into the page (the vertical counter moves with time).
 * Vertical blank: every 1/60 s of virtual time, when the title registered a graphics interrupt callback, the
 * callback runs on a short-lived guest thread (r3 = 0: vblank, r4 = its user data), as an interrupt would;
 * PM4_INTERRUPT packets do the same with r3 = 1. The front buffer of each VdSwap is recorded (and traced). */
#include <stdlib.h>
#include <string.h>

#include "x360_internal.h"
#include "xenos_cp.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

#define GPU_MMIO 0x7FC80000u
#define VBLANK_100NS 166667u

struct x360_gpu {
    xenos_cp cp;
    uint32_t irq_callback, irq_user;
    uint64_t next_vblank;
    uint32_t vblanks, last_wptr;
    int irq_pending[2]; /* source 0 (vblank), 1 (command processor) queued while one runs */
    int ring;
};

static uint32_t phys_read(void *ctx, uint32_t phys)
{
    x360_machine *x = ctx;
    return x360_r32(x, 0xA0000000u + (phys & 0x1FFFFFFFu));
}

static void phys_write(void *ctx, uint32_t phys, uint32_t v)
{
    x360_machine *x = ctx;
    x360_w32(x, 0xA0000000u + (phys & 0x1FFFFFFFu), v);
}

static void cp_interrupt(void *ctx, uint32_t source, uint32_t cpu)
{
    x360_machine *x = ctx;
    (void)cpu;
    if (x->gpu)
        x->gpu->irq_pending[source & 1] = 1;
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
        xenos_cp_init(&x->gpu->cp);
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
    /* one interrupt thread at a time: a pending one waits for the running one to end */
    for (int k = 0; k < x->nthreads; k++)
        if (x->threads[k].state != TH_FREE && x->threads[k].state != TH_DONE && x->threads[k].interrupt) {
            g->irq_pending[source & 1] = 1;
            return;
        }
    x360_thread *t = x360_thread_create(x, 0x4000, g->irq_callback, source, g->irq_user, 0);
    if (t)
        t->interrupt = 1;
}

void x360_gpu_poll(x360_machine *x)
{
    struct x360_gpu *g = x->gpu;
    if (!g)
        return;
    uint64_t now = x360_now(x);
    /* registers titles poll: the vertical counter, interrupt status, the viewport size */
    x360_w32(x, GPU_MMIO + 4 * 0x194C, (uint32_t)(now % VBLANK_100NS * 750 / VBLANK_100NS));
    x360_w32(x, GPU_MMIO + 4 * 0x1961, 1280u << 16 | 720u);
    if (g->ring) {
        uint32_t w = x360_r32(x, GPU_MMIO + 4 * XENOS_REG_CP_RB_WPTR);
        if (w != g->last_wptr || g->cp.stalled) {
            g->last_wptr = w;
            xenos_cp_set_wptr(&g->cp, w);
        }
    }
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

uint64_t x360_gpu_next_event(x360_machine *x)
{
    return x->gpu && x->gpu->irq_callback ? x->gpu->next_vblank : 0;
}

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
    xenos_cp_ring_init(&g->cp, A(0), A(1)); /* (physical address, log2 of the size in quadwords) */
    g->ring = 1;
    g->last_wptr = 0;
    x360_w32(x, GPU_MMIO + 4 * XENOS_REG_CP_RB_WPTR, 0);
    RET(0);
}

void x360_k_VdEnableRingBufferRPtrWriteBack(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    gpu_of(x)->cp.rptr_writeback = A(0);
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
    uint32_t fb = fb_ptr ? x360_r32(x, fb_ptr) : 0;
    memset(x->base + buf, 0, 64 * 4);
    x360_w32(x, buf, 3u << 30 | (4u - 1) << 16 | (uint32_t)PM4_XE_SWAP << 8);
    x360_w32(x, buf + 4, 0x53574150u); /* 'SWAP' */
    x360_w32(x, buf + 8, fb & 0x1FFFFFFFu);
    x360_w32(x, buf + 12, w_ptr ? x360_r32(x, w_ptr) : 0);
    x360_w32(x, buf + 16, h_ptr ? x360_r32(x, h_ptr) : 0);
    x360_w32(x, buf + 20, 3u << 30 | (64u - 5 - 1 - 1) << 16 | (uint32_t)PM4_NOP << 8);
    RET(0);
}

void x360_k_VdGetSystemCommandBuffer(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0)) {
        memset(x->base + A(0), 0, 0x94);
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
        x360_w32(x, A(1), p ? 0xE0000000u + p : 0);
    }
    RET(0);
}

void x360_k_VdNop0(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}
