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
 * PM4_INTERRUPT packets do the same with r3 = 1. VdSwap puts the front buffer's fetch constant and the swap token in
 * the ring; the front buffer of each swap is recorded (and traced). */
#include <stdlib.h>
#include <string.h>

#include "x360_internal.h"
#include "xenos_cp.h"
#include "xenos_gl.h"

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
    int irq_thread; /* the interrupt context's slot, -1 before the first interrupt */
    char shader_dir[512]; /* --dump-shaders: each distinct microcode written there once */
    xenos_gl *gl;         /* --render: the GLES renderer (host state: not in save states) */
};

static void gl_draw(void *ctx, const xenos_cp *cp, const xenos_draw *d)
{
    xenos_gl_draw(ctx, cp, d);
}

static void gl_swap(void *ctx, const xenos_cp *cp)
{
    xenos_gl_swap(ctx, cp);
}

static struct x360_gpu *gpu_of(x360_machine *x);

static void shader_loaded(void *ctx, int pixel, const uint32_t *code, uint32_t words)
{
    x360_machine *x = ctx;
    struct x360_gpu *g = x->gpu;
    if (!g || !g->shader_dir[0] || !words)
        return;
    uint8_t be[4 * XENOS_SHADER_WORDS];
    for (uint32_t k = 0; k < words; k++) {
        be[4 * k] = (uint8_t)(code[k] >> 24);
        be[4 * k + 1] = (uint8_t)(code[k] >> 16);
        be[4 * k + 2] = (uint8_t)(code[k] >> 8);
        be[4 * k + 3] = (uint8_t)code[k];
    }
    char path[600];
    snprintf(path, sizeof path, "%s/%s_%08X.bin", g->shader_dir, pixel ? "ps" : "vs", x360_crc32(be, 4 * words));
    FILE *f = fopen(path, "rb");
    if (f) { /* already there */
        fclose(f);
        return;
    }
    f = fopen(path, "wb");
    if (f) {
        fwrite(be, 4, words, f);
        fclose(f);
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

void x360_set_shader_dump(x360_machine *x, const char *dir)
{
    struct x360_gpu *g = gpu_of(x);
    if (g)
        snprintf(g->shader_dir, sizeof g->shader_dir, "%s", dir ? dir : "");
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
    fprintf(out, "\n");
}

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
        x->gpu->irq_thread = -1;
        x->gpu->cp.shader_loaded = shader_loaded;
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
    if (x->gpu)
        xenos_gl_destroy(x->gpu->gl);
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
    if (t) {
        x360_thread_restart(x, t, g->irq_callback, source, g->irq_user);
        return;
    }
    t = x360_thread_create(x, 0x4000, g->irq_callback, source, g->irq_user, 0);
    if (t) {
        t->interrupt = 1;
        g->irq_thread = (int)(t - x->threads);
    }
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

/* save states (x360_state.c): the command processor and the interrupt state, field by field */
void x360_gpu_state(x360_machine *x, void *s, void (*u32)(void *, uint32_t *), void (*u64)(void *, uint64_t *),
                    void (*i32)(void *, int *))
{
    struct x360_gpu *g = gpu_of(x);
    xenos_cp *cp = &g->cp;
    for (uint32_t k = 0; k < XENOS_REGS; k++)
        u32(s, &cp->regs[k]);
    u32(s, &cp->rb_base);
    u32(s, &cp->rb_size);
    u32(s, &cp->rptr);
    u32(s, &cp->wptr);
    u32(s, &cp->rptr_writeback);
    u32(s, &cp->counter);
    i32(s, &cp->stalled);
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
    /* as Xenia's VdSwap: the front buffer's texture fetch constant (from the Direct3D texture header, its base made
     * physical) into fetch constant 0, then the token */
    uint32_t fetch = A(1), at = buf;
    if (fetch) {
        x360_w32(x, at, 0u << 30 | (6u - 1) << 16 | 0x4800u);
        for (uint32_t k = 0; k < 6; k++) {
            uint32_t v = x360_r32(x, fetch + 4 * k);
            if (k == 1)
                v = (v & 0xFFFu) | ((v & 0xFFFFF000u) & 0x1FFFFFFFu); /* the physical views' addresses */
            x360_w32(x, at + 4 + 4 * k, v);
        }
        at += 28;
    }
    x360_w32(x, at, 3u << 30 | (4u - 1) << 16 | (uint32_t)PM4_XE_SWAP << 8);
    x360_w32(x, at + 4, 0x53574150u); /* 'SWAP' */
    x360_w32(x, at + 8, fb & 0x1FFFFFFFu);
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
