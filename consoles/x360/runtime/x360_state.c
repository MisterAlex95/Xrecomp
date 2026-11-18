/* x360_state.c -- save states of the Xbox 360 machine (XBA-270), in the GameCube's format (gc_state.c).
 *
 * The blob: a header (magic "XRSTATE\0", console "x360", X360_STATE_VERSION, the title key, the section count), then
 * sections, each a tag (four characters), a section version, a length and its bytes. Fields are written one by one
 * in little-endian order (no struct copied), one walker per section for the size, the save and the load alike.
 *
 *   THRD  required  every guest thread: its context (GPRs, FPRs, the 128 vector registers, LR, CTR, PC, CR, XER,
 *                   FPSCR, VSCR, MSR, the reservation) and the scheduler's view (state, suspend count, ids, KTHREAD /
 *                   PCR / TLS / stack addresses, the wait it is in), the running thread, the virtual clock
 *   KERN  required  handles, timers, TLS slots, the kernel bump allocator, the title's status
 *   HEAP  required  the five heaps' page maps (what is allocated, and how long each allocation is)
 *   MEM   required  guest memory in use: the image, every allocated page of every heap (the physical memory through
 *                   its view), the GPU register page; pages that are all zero are left out
 *   GPU   optional  the command processor (register file, ring, write-back, the shaders last loaded, counters) and
 *                   the interrupt state (callback, next vertical blank, pending interrupts)
 *   XMA   optional  the XMA context array's place, which contexts are in use and enabled (XBA-280)
 *   SEH   optional  an exception being dispatched on each thread (x360_seh.c, XBA-282)
 *   FILE  optional  the open files (device, path, position: they are opened again at load) and symbolic links
 *
 * A state is taken between time slices (x360_run returned), when every thread's context is in its slot. Not state:
 * the pack (lifted code: the same title's pack must be bound), the reservation's host address, the disc handle
 * (the run's disc stays), statistics. The key is the title's (the image CRC as hex when the caller gives none). */
#include <stdlib.h>
#include <string.h>

#include "x360_internal.h"
#include "xenos_cp.h"

#define STATE_MAGIC "XRSTATE\0"
#define HEADER_BYTES 72u
#define KEY_BYTES 48u
#define PAGE 4096u

typedef struct io {
    uint8_t *out;
    const uint8_t *in;
    size_t at, cap;
    int load, bad;
} io;

static void io_bytes(io *s, void *p, size_t n)
{
    if (s->load) {
        if (s->at + n > s->cap) {
            s->bad = 1;
            memset(p, 0, n);
        } else
            memcpy(p, s->in + s->at, n);
    } else if (s->out)
        memcpy(s->out + s->at, p, n);
    s->at += n;
}

static void io_u32(io *s, uint32_t *v)
{
    uint8_t b[4];
    if (!s->load)
        for (int i = 0; i < 4; i++)
            b[i] = (uint8_t)(*v >> (8 * i));
    io_bytes(s, b, 4);
    if (s->load)
        *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static void io_u64(io *s, uint64_t *v)
{
    uint32_t lo = (uint32_t)*v, hi = (uint32_t)(*v >> 32);
    io_u32(s, &lo);
    io_u32(s, &hi);
    if (s->load)
        *v = (uint64_t)hi << 32 | lo;
}

static void io_i32(io *s, int *v)
{
    uint32_t u = (uint32_t)*v;
    io_u32(s, &u);
    if (s->load)
        *v = (int)u;
}

static void io_f64(io *s, double *v)
{
    uint64_t u;
    memcpy(&u, v, 8);
    io_u64(s, &u);
    if (s->load)
        memcpy(v, &u, 8);
}

/* ---- sections ---- */

static void sec_threads(x360_machine *x, io *s)
{
    io_i32(s, &x->nthreads);
    io_i32(s, &x->cur);
    io_u32(s, &x->next_thread_id);
    io_u64(s, &x->m.rc_native);
    io_u64(s, &x->m.tb_base);
    io_u64(s, &x->time_skip);
    io_u64(s, &x->st.hle_calls);   /* the virtual clock counts these too (x360_now) */
    io_u64(s, &x->st.interpreted);
    if (s->load && (x->nthreads < 0 || x->nthreads > X360_MAX_THREADS || x->cur >= X360_MAX_THREADS)) {
        s->bad = 1;
        return;
    }
    for (int k = 0; k < x->nthreads; k++) {
        x360_thread *t = &x->threads[k];
        xe_cpu *c = &t->cpu;
        if (s->load) {
            memset(&t->seh, 0, sizeof t->seh); /* the SEH section, when the state has one, sets it again */
            t->interp = 1; /* XBA-283: a thread stopped in interpreted code resumes there, not a new miss */
        }
        for (int r = 0; r < 32; r++)
            io_u64(s, &c->gpr[r]);
        for (int r = 0; r < 32; r++)
            io_f64(s, &c->fpr[r]);
        for (int r = 0; r < 128; r++)
            for (int e = 0; e < 4; e++)
                io_u32(s, &c->vr[r].u32[e]);
        io_u64(s, &c->lr);
        io_u64(s, &c->ctr);
        io_u32(s, &c->pc);
        io_u32(s, &c->cr);
        io_u32(s, &c->xer_so);
        io_u32(s, &c->xer_ov);
        io_u32(s, &c->xer_ca);
        io_u32(s, &c->fpscr);
        io_u32(s, &c->vscr);
        io_u32(s, &c->msr);
        io_u32(s, &c->reserve);
        io_i32(s, &t->state);
        io_i32(s, &t->suspend);
        io_u32(s, &t->id);
        io_u32(s, &t->handle);
        io_u32(s, &t->kthread);
        io_u32(s, &t->pcr);
        io_u32(s, &t->tls_static);
        io_u32(s, &t->tls_slots);
        io_u32(s, &t->stack_base);
        io_u32(s, &t->stack_limit);
        io_i32(s, &t->wait_kind);
        io_i32(s, &t->wait_all);
        io_i32(s, &t->wait_count);
        for (int w = 0; w < X360_MAX_WAIT; w++)
            io_u32(s, &t->wait_obj[w]);
        io_u64(s, &t->wait_deadline);
        io_i32(s, &t->wait_timed);
        io_u32(s, &t->exit_code);
        io_i32(s, &t->interrupt);
    }
}

static void sec_kernel(x360_machine *x, io *s)
{
    for (int k = 0; k < X360_MAX_HANDLES; k++) {
        io_u32(s, &x->handles[k].obj);
        io_i32(s, &x->handles[k].used);
    }
    for (int k = 0; k < X360_MAX_TIMERS; k++) {
        io_u32(s, &x->timers[k].obj);
        io_u64(s, &x->timers[k].due);
        io_u32(s, &x->timers[k].period);
    }
    io_u64(s, &x->tls_used);
    io_u32(s, &x->kalloc_cur);
    io_u32(s, &x->kalloc_end);
    io_u64(s, &x->rng);
    for (int k = 0; k < 8; k++) {
        x360_audio_client *a = &x->audio[k];
        io_u32(s, &a->callback);
        io_u32(s, &a->arg);
        io_u64(s, &a->next);
        io_i32(s, &a->thread);
        io_u32(s, &a->frames_requested);
        io_u32(s, &a->frames_submitted);
    }
    int status = x->status == X360_BUDGET ? X360_RUNNING : x->status; /* a budget is the caller's, not state */
    io_i32(s, &status);
    if (s->load)
        x->status = status;
    io_u32(s, &x->exit_code);
}

static x360_heap *heap_k(x360_machine *x, int k)
{
    x360_heap *hs[] = {&x->heap_v4k, &x->heap_v64k, &x->heap_stack, &x->heap_kernel, &x->heap_phys};
    return hs[k];
}

static void sec_heaps(x360_machine *x, io *s)
{
    /* per heap: its page count, then (first page, length in pages) for each allocation, ended by a zero length */
    for (int k = 0; k < 5; k++) {
        x360_heap *h = heap_k(x, k);
        uint32_t pages = h->pages;
        io_u32(s, &pages);
        if (pages != h->pages) {
            s->bad = 1;
            return;
        }
        if (!s->load) {
            for (uint32_t p = 0; p < h->pages; p++)
                if (h->len[p]) {
                    uint32_t first = p, n = h->len[p];
                    io_u32(s, &first);
                    io_u32(s, &n);
                }
            uint32_t zero = 0;
            io_u32(s, &zero);
            io_u32(s, &zero);
            continue;
        }
        memset(h->len, 0, h->pages * sizeof *h->len);
        memset(h->used, 0, h->pages);
        for (;;) {
            uint32_t first = 0, n = 0;
            io_u32(s, &first);
            io_u32(s, &n);
            if (s->bad || !n)
                break;
            if (first >= h->pages || n > h->pages - first) {
                s->bad = 1;
                return;
            }
            h->len[first] = n;
            memset(h->used + first, 1, n);
        }
    }
}

/* every guest page in use: callback(x, guest address of the 4 KB page, ctx) */
static void each_page(x360_machine *x, void (*fn)(x360_machine *, uint32_t, void *), void *ctx)
{
    for (uint32_t a = x->image_base; a < x->image_base + x->image_size; a += PAGE)
        fn(x, a, ctx);
    for (uint32_t a = 0x7FC80000u; a < 0x7FC90000u; a += PAGE)
        fn(x, a, ctx);
    for (uint32_t a = 0x7FEA0000u; a < 0x7FEB0000u; a += PAGE) /* the APU registers (XMA kicks) */
        fn(x, a, ctx);
    for (int k = 0; k < 5; k++) {
        x360_heap *h = heap_k(x, k);
        for (uint32_t p = 0; p < h->pages; p++)
            if (h->used[p])
                for (uint32_t o = 0; o < h->page; o += PAGE) {
                    uint32_t a = h->base + p * h->page + o;
                    fn(x, h->physical ? 0xA0000000u + a : a, ctx);
                }
    }
}

static int all_zero(const uint8_t *p)
{
    for (unsigned k = 0; k < PAGE; k++)
        if (p[k])
            return 0;
    return 1;
}

static void page_save(x360_machine *x, uint32_t a, void *ctx)
{
    io *s = ctx;
    const uint8_t *p = x->base + a;
    if (all_zero(p))
        return;
    io_u32(s, &a);
    io_bytes(s, (void *)p, PAGE);
}

static void page_commit_zero(x360_machine *x, uint32_t a, void *ctx)
{
    (void)ctx;
    x360_commit(x, a, PAGE);
    memset(x->base + a, 0, PAGE);
}

static void sec_memory(x360_machine *x, io *s)
{
    if (!s->load) {
        each_page(x, page_save, s);
        uint32_t end = 0xFFFFFFFFu;
        io_u32(s, &end);
        return;
    }
    each_page(x, page_commit_zero, NULL); /* the restored heaps say which pages exist; their content follows */
    for (;;) {
        uint32_t a = 0;
        io_u32(s, &a);
        if (s->bad || a == 0xFFFFFFFFu)
            break;
        if (a & (PAGE - 1)) {
            s->bad = 1;
            break;
        }
        x360_commit(x, a, PAGE);
        io_bytes(s, x->base + a, PAGE);
    }
}

void x360_gpu_state(x360_machine *x, void *io_, void (*u32)(void *, uint32_t *), void (*u64)(void *, uint64_t *),
                    void (*i32)(void *, int *));

static void cb_u32(void *s, uint32_t *v) { io_u32(s, v); }
static void cb_u64(void *s, uint64_t *v) { io_u64(s, v); }
static void cb_i32(void *s, int *v) { io_i32(s, v); }

static void sec_gpu(x360_machine *x, io *s)
{
    x360_gpu_state(x, s, cb_u32, cb_u64, cb_i32);
}

void x360_fs_state(x360_machine *x, void *io_, void (*u32)(void *, uint32_t *), void (*u64)(void *, uint64_t *),
                   void (*i32)(void *, int *), void (*bytes)(void *, void *, size_t), int load);

static void cb_bytes(void *s, void *p, size_t n) { io_bytes(s, p, n); }

static void sec_files(x360_machine *x, io *s)
{
    x360_fs_state(x, s, cb_u32, cb_u64, cb_i32, cb_bytes, s->load);
}

/* the controllers and the pad script's position (x360_input.c): their own optional section, so states without it
 * still load (pads disconnected) */
static void sec_pads(x360_machine *x, io *s)
{
    for (int k = 0; k < 4; k++) {
        x360_pad_slot *p = &x->pads[k];
        uint32_t b = p->state.buttons, tr = (uint32_t)p->state.left_trigger << 8 | p->state.right_trigger;
        uint32_t l = (uint16_t)p->state.thumb_lx | (uint32_t)(uint16_t)p->state.thumb_ly << 16;
        uint32_t r = (uint16_t)p->state.thumb_rx | (uint32_t)(uint16_t)p->state.thumb_ry << 16;
        io_i32(s, &p->connected);
        io_u32(s, &p->packet);
        io_u32(s, &b);
        io_u32(s, &tr);
        io_u32(s, &l);
        io_u32(s, &r);
        if (s->load) {
            p->state.buttons = (uint16_t)b, p->state.left_trigger = (uint8_t)(tr >> 8);
            p->state.right_trigger = (uint8_t)tr;
            p->state.thumb_lx = (int16_t)l, p->state.thumb_ly = (int16_t)(l >> 16);
            p->state.thumb_rx = (int16_t)r, p->state.thumb_ry = (int16_t)(r >> 16);
        }
    }
    uint32_t at = x->pad_script_at;
    io_u32(s, &at);
    if (s->load)
        x->pad_script_at = at <= x->pad_script_n ? at : x->pad_script_n;
}

/* XBA-282: an exception being dispatched (x360_seh.c): each thread's levels, field by field */
static void io_seh_ctx(io *s, x360_seh_ctx *k)
{
    for (int r = 0; r < 32; r++)
        io_u64(s, &k->gpr[r]);
    for (int r = 0; r < 32; r++)
        io_f64(s, &k->fpr[r]);
    io_u64(s, &k->lr);
    io_u64(s, &k->ctr);
    io_u32(s, &k->pc);
    io_u32(s, &k->cr);
    io_u32(s, &k->xer);
}

static void sec_seh(x360_machine *x, io *s)
{
    for (int k = 0; k < x->nthreads && k < X360_MAX_THREADS; k++) {
        x360_seh *e = &x->threads[k].seh;
        io_i32(s, &e->depth);
        if (s->load && (e->depth < 0 || e->depth > X360_SEH_DEPTH)) {
            s->bad = 1;
            return;
        }
        for (int d = 0; d < e->depth; d++) {
            x360_seh_level *l = &e->lv[d];
            io_i32(s, &l->phase);
            io_i32(s, &l->wait);
            io_u32(s, &l->scope);
            io_seh_ctx(s, &l->exc);
            io_seh_ctx(s, &l->cur);
            io_seh_ctx(s, &l->next);
            uint32_t *f[] = {&l->rec, &l->area, &l->establisher, &l->fstart, &l->flen, &l->fprolog, &l->handler,
                             &l->hdata, &l->target_frame, &l->target_ip, &l->retval};
            for (size_t i = 0; i < sizeof f / sizeof f[0]; i++)
                io_u32(s, f[i]);
        }
    }
}

/* XMA: the context array's place, the contexts in use / enabled (their bytes are guest memory: MEM) */
static void sec_xma(x360_machine *x, io *s)
{
    uint32_t array = 0;
    uint8_t used[320], enabled[320];
    if (!s->load)
        x360_xma_state(x, &array, used, enabled, 0);
    io_u32(s, &array);
    io_bytes(s, used, sizeof used);
    io_bytes(s, enabled, sizeof enabled);
    if (s->load && !s->bad)
        x360_xma_state(x, &array, used, enabled, 1);
}

typedef struct {
    char tag[5];
    uint32_t version;
    int required;
    void (*walk)(x360_machine *, io *);
} section;

static const section k_sections[] = {
    {"THRD", 1, 1, sec_threads}, {"KERN", 1, 1, sec_kernel}, {"HEAP", 1, 1, sec_heaps},
    {"MEM ", 1, 1, sec_memory},  {"GPU ", 1, 0, sec_gpu},    {"FILE", 1, 0, sec_files},
    {"PADS", 1, 0, sec_pads},    {"XMA ", 1, 0, sec_xma},    {"SEH ", 1, 0, sec_seh},
};
#define NSEC (sizeof k_sections / sizeof k_sections[0])

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void default_key(x360_machine *x, const char *key, char *out)
{
    if (key && key[0])
        snprintf(out, KEY_BYTES, "%s", key);
    else
        snprintf(out, KEY_BYTES, "X360-%08X", x->image_crc);
}

size_t x360_state_size(x360_machine *x)
{
    size_t n = HEADER_BYTES;
    for (size_t i = 0; i < NSEC; i++) {
        io s = {0};
        k_sections[i].walk(x, &s);
        n += 12 + s.at;
    }
    return n;
}

size_t x360_state_save(x360_machine *x, const char *key, uint8_t *out, size_t cap)
{
    if (!x || !out || x->status == X360_ERROR || x->status == X360_MISSED)
        return 0;
    size_t need = x360_state_size(x);
    if (cap < need)
        return 0;
    memset(out, 0, HEADER_BYTES);
    memcpy(out, STATE_MAGIC, 8);
    memcpy(out + 8, "x360", 4);
    put32(out + 12, X360_STATE_VERSION);
    default_key(x, key, (char *)out + 16);
    put32(out + 16 + KEY_BYTES, (uint32_t)NSEC);
    size_t at = HEADER_BYTES;
    for (size_t i = 0; i < NSEC; i++) {
        io s = {0};
        s.out = out + at + 12;
        k_sections[i].walk(x, &s);
        memcpy(out + at, k_sections[i].tag, 4);
        put32(out + at + 4, k_sections[i].version);
        put32(out + at + 8, (uint32_t)s.at);
        at += 12 + s.at;
    }
    return at;
}

int x360_state_load(x360_machine *x, const char *key, const uint8_t *data, size_t size, char *why, size_t why_n)
{
    char k[KEY_BYTES];
    if (!x || !data || size < HEADER_BYTES || memcmp(data, STATE_MAGIC, 8)) {
        snprintf(why, why_n, "not a save state");
        return X360_STATE_ECORRUPT;
    }
    if (memcmp(data + 8, "x360", 4)) {
        snprintf(why, why_n, "a state of another console");
        return X360_STATE_EGAME;
    }
    if (get32(data + 12) != X360_STATE_VERSION) {
        snprintf(why, why_n, "a state of another format version (%u)", get32(data + 12));
        return X360_STATE_EVERSION;
    }
    default_key(x, key, k);
    if (strncmp((const char *)data + 16, k, KEY_BYTES)) {
        snprintf(why, why_n, "a state of another title (%.48s, this one is %s)", (const char *)data + 16, k);
        return X360_STATE_EGAME;
    }
    /* find each section first: a missing required one refuses the state before anything changes */
    const uint8_t *at[NSEC] = {0};
    size_t len[NSEC] = {0};
    uint32_t n = get32(data + 16 + KEY_BYTES);
    size_t p = HEADER_BYTES;
    for (uint32_t i = 0; i < n; i++) {
        if (p + 12 > size || p + 12 + get32(data + p + 8) > size) {
            snprintf(why, why_n, "truncated state");
            return X360_STATE_ECORRUPT;
        }
        for (size_t j = 0; j < NSEC; j++)
            if (!memcmp(data + p, k_sections[j].tag, 4)) {
                if (get32(data + p + 4) > k_sections[j].version) {
                    snprintf(why, why_n, "section %.4s is newer than this build", k_sections[j].tag);
                    return X360_STATE_EVERSION;
                }
                at[j] = data + p + 12;
                len[j] = get32(data + p + 8);
            }
        p += 12 + get32(data + p + 8);
    }
    for (size_t j = 0; j < NSEC; j++)
        if (k_sections[j].required && !at[j]) {
            snprintf(why, why_n, "section %.4s missing", k_sections[j].tag);
            return X360_STATE_ECORRUPT;
        }
    for (size_t j = 0; j < NSEC; j++) {
        if (!at[j])
            continue;
        io s = {0};
        s.in = at[j];
        s.cap = len[j];
        s.load = 1;
        k_sections[j].walk(x, &s);
        if (s.bad) {
            snprintf(why, why_n, "section %.4s is damaged (the machine is in an undefined state)", k_sections[j].tag);
            x->status = X360_ERROR;
            return X360_STATE_ECORRUPT;
        }
    }
    x->m.cur = x->cur >= 0 ? &x->threads[x->cur].cpu : NULL;
    x360_render_reset(x); /* the eDRAM is not in states: the renderer's surfaces start clear */
    return X360_STATE_OK;
}
