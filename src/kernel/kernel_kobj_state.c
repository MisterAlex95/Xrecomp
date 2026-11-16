/*
 * kernel_kobj_state.c - the kernel's guest-visible objects in a save state
 * (XBA-246). See kernel_kobj_state.h for what is carried and how a load
 * binds running threads.
 *
 * The blob:
 *
 *   "XKOB" u32 version
 *   records: u16 type, u16 version, u32 length, payload (little-endian)
 *     OBJECT records first (one per host object, its id and state), then the
 *     tables that name objects by id, then END.
 *
 * Host objects are read through win32_compat's w32_state_read, which looks at
 * an object without changing it (an auto-reset event's state is not consumed
 * by reading it), and made again with w32_state_make; files through the file
 * layer (xbox_FileStateRead / Reopen).
 */
#include "kernel.h"
#include "kernel_kobj_state.h"
#include "kernel_ksync.h"
#include "guest_clock.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Byte buffers ─────────────────────────────────────────────────────────── */

void xbox_kobj_put(XboxKobjBuf *b, const void *data, size_t n)
{
    if (b->oom)
        return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        uint8_t *p;
        while (cap < b->len + n)
            cap *= 2;
        p = (uint8_t *)realloc(b->p, cap);
        if (!p) {
            b->oom = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

void xbox_kobj_u8(XboxKobjBuf *b, uint8_t v) { xbox_kobj_put(b, &v, 1); }

void xbox_kobj_u16(XboxKobjBuf *b, uint16_t v)
{
    uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    xbox_kobj_put(b, x, 2);
}

void xbox_kobj_u32(XboxKobjBuf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    xbox_kobj_put(b, x, 4);
}

void xbox_kobj_u64(XboxKobjBuf *b, uint64_t v)
{
    xbox_kobj_u32(b, (uint32_t)v);
    xbox_kobj_u32(b, (uint32_t)(v >> 32));
}

void xbox_kobj_str(XboxKobjBuf *b, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > 0xFFFF)
        n = 0xFFFF;
    xbox_kobj_u16(b, (uint16_t)n);
    xbox_kobj_put(b, s ? s : "", n);
}

static int rd_need(XboxKobjRd *r, size_t n)
{
    if (r->bad || r->len - r->pos < n) {
        r->bad = 1;
        return 0;
    }
    return 1;
}

uint8_t xbox_kobj_get8(XboxKobjRd *r)
{
    if (!rd_need(r, 1)) return 0;
    return r->p[r->pos++];
}

uint16_t xbox_kobj_get16(XboxKobjRd *r)
{
    uint16_t v;
    if (!rd_need(r, 2)) return 0;
    v = (uint16_t)(r->p[r->pos] | (r->p[r->pos + 1] << 8));
    r->pos += 2;
    return v;
}

uint32_t xbox_kobj_get32(XboxKobjRd *r)
{
    uint32_t v;
    if (!rd_need(r, 4)) return 0;
    v = (uint32_t)r->p[r->pos] | ((uint32_t)r->p[r->pos + 1] << 8)
      | ((uint32_t)r->p[r->pos + 2] << 16) | ((uint32_t)r->p[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

uint64_t xbox_kobj_get64(XboxKobjRd *r)
{
    uint64_t lo = xbox_kobj_get32(r);
    return lo | ((uint64_t)xbox_kobj_get32(r) << 32);
}

void xbox_kobj_getstr(XboxKobjRd *r, char *out, size_t cap)
{
    size_t n = xbox_kobj_get16(r), k;
    if (!rd_need(r, n)) {
        if (cap) out[0] = 0;
        return;
    }
    k = n < cap ? n : (cap ? cap - 1 : 0);
    if (cap) {
        memcpy(out, r->p + r->pos, k);
        out[k] = 0;
    }
    r->pos += n;
}

/* ── Save ─────────────────────────────────────────────────────────────────── */

typedef struct { void *h; uint32_t id; } kobj_map;
typedef struct { uint32_t tid, kthread; } kobj_thr;

struct XboxKobjSaveCtx {
    unsigned          flags;
    XboxKobjBuf       objs;       /* OBJECT records */
    XboxKobjBuf       recs;       /* everything else */
    size_t            rec_at;     /* where the open record's header is */
    kobj_map         *map;
    size_t            nmap, capmap;
    kobj_thr         *thr;        /* host thread id -> KTHREAD */
    size_t            nthr, capthr;
    XboxKobjRefusals *why;
    int               refusals;
};

unsigned xbox_kobj_flags(const XboxKobjSaveCtx *c) { return c->flags; }

void xbox_kobj_refuse(XboxKobjSaveCtx *c, uint32_t token, uint32_t guest_va,
                      const char *what, const char *fmt, ...)
{
    if (c->why && c->refusals < XBOX_KOBJ_REFUSALS_MAX) {
        XboxKobjRefusal *f = &c->why->r[c->refusals];
        va_list ap;
        f->token = token;
        f->guest_va = guest_va;
        snprintf(f->what, sizeof f->what, "%s", what ? what : "object");
        va_start(ap, fmt);
        vsnprintf(f->reason, sizeof f->reason, fmt, ap);
        va_end(ap);
    }
    c->refusals++;
    if (c->why)
        c->why->count = c->refusals;
}

XboxKobjBuf *xbox_kobj_rec_begin(XboxKobjSaveCtx *c, uint16_t type)
{
    c->rec_at = c->recs.len;
    xbox_kobj_u16(&c->recs, type);
    xbox_kobj_u16(&c->recs, 1);
    xbox_kobj_u32(&c->recs, 0);
    return &c->recs;
}

void xbox_kobj_rec_end(XboxKobjSaveCtx *c)
{
    uint32_t n;
    if (c->recs.oom)
        return;
    n = (uint32_t)(c->recs.len - c->rec_at - 8);
    c->recs.p[c->rec_at + 4] = (uint8_t)n;
    c->recs.p[c->rec_at + 5] = (uint8_t)(n >> 8);
    c->recs.p[c->rec_at + 6] = (uint8_t)(n >> 16);
    c->recs.p[c->rec_at + 7] = (uint8_t)(n >> 24);
}

#ifndef _WIN32
static int kobj_resume_known(uint32_t tid);
static uint32_t kobj_kthread_of(XboxKobjSaveCtx *c, uint32_t tid)
{
    size_t i;
    for (i = 0; tid && i < c->nthr; i++)
        if (c->thr[i].tid == tid)
            return c->thr[i].kthread;
    return 0;
}

/* xbox_bridge_kobj_handles: a thread object behind a guest VA names the
 * KTHREAD of its host thread. */
static void kobj_note_thread(void *ctx, uint32_t guest_va, void *h)
{
    XboxKobjSaveCtx *c = (XboxKobjSaveCtx *)ctx;
    w32_state ws;
    if (!guest_va || w32_state_read((HANDLE)h, &ws) != W32_STATE_THREAD
            || kobj_kthread_of(c, ws.tid))
        return;
    if (c->nthr == c->capthr) {
        size_t cap = c->capthr ? c->capthr * 2 : 32;
        kobj_thr *t = (kobj_thr *)realloc(c->thr, cap * sizeof *t);
        if (!t)
            return;
        c->thr = t;
        c->capthr = cap;
    }
    c->thr[c->nthr].tid = ws.tid;
    c->thr[c->nthr].kthread = guest_va;
    c->nthr++;
}

static void kobj_object(XboxKobjSaveCtx *c, void *h, uint32_t id, uint32_t token,
                        uint32_t guest_va)
{
    XboxKobjBuf *b = &c->objs;
    size_t at;
    w32_state ws;
    int kind = w32_state_read((HANDLE)h, &ws);
    uint16_t xk = 0;

    switch (kind) {
    case W32_STATE_EVENT:     xk = XKOB_K_EVENT; break;
    case W32_STATE_SEMAPHORE: xk = XKOB_K_SEMAPHORE; break;
    case W32_STATE_MUTEX:     xk = XKOB_K_MUTANT; break;
    case W32_STATE_THREAD:    xk = XKOB_K_THREAD; break;
    case W32_STATE_TIMER:     xk = XKOB_K_TIMER; break;
    case W32_STATE_FILE:      xk = XKOB_K_FILE; break;
    case 0:
        xbox_kobj_refuse(c, token, guest_va, "handle", "names no live host object (%p)", h);
        return;
    default:
        xbox_kobj_refuse(c, token, guest_va, "host object",
                         "a file mapping or host timer: not carried");
        return;
    }

    at = b->len;
    xbox_kobj_u16(b, XKOB_R_OBJECT);
    xbox_kobj_u16(b, 1);
    xbox_kobj_u32(b, 0);
    xbox_kobj_u32(b, id);
    xbox_kobj_u16(b, xk);
    xbox_kobj_u32(b, (uint32_t)ws.refcount);   /* informative */
    switch (xk) {
    case XKOB_K_EVENT:
        if (ws.grants)
            xbox_kobj_refuse(c, token, guest_va, "event",
                             "a set is on its way to a blocked thread (retry the save)");
        xbox_kobj_u8(b, (uint8_t)ws.manual_reset);
        xbox_kobj_u8(b, (uint8_t)ws.signaled);
        break;
    case XKOB_K_SEMAPHORE:
        xbox_kobj_u32(b, (uint32_t)ws.count);
        xbox_kobj_u32(b, (uint32_t)ws.maximum);
        break;
    case XKOB_K_MUTANT:
        if (ws.owner_tid && (c->flags & XBOX_KOBJ_ACROSS_RUNS) && !(c->flags & XBOX_KOBJ_RESUME_THREADS))
            xbox_kobj_refuse(c, token, guest_va, "mutant",
                             "owned by host thread %u, which another run will not have",
                             (unsigned)ws.owner_tid);
        xbox_kobj_u32(b, ws.owner_tid);
        xbox_kobj_u32(b, kobj_kthread_of(c, ws.owner_tid));
        xbox_kobj_u32(b, (uint32_t)ws.recursion);
        break;
    case XKOB_K_THREAD:
        if (!ws.exited && (c->flags & XBOX_KOBJ_ACROSS_RUNS) && !(c->flags & XBOX_KOBJ_RESUME_THREADS))
            xbox_kobj_refuse(c, token, guest_va, "thread",
                             "running: its host stack is rebuilt only by resume entries (XBA-250)");
        else if (!ws.exited && (c->flags & XBOX_KOBJ_RESUME_THREADS) && !kobj_resume_known(ws.tid))
            xbox_kobj_refuse(c, token, guest_va, "thread",
                             "running (host thread %u) but not one of the threads the state holds",
                             (unsigned)ws.tid);
        xbox_kobj_u32(b, ws.tid);
        xbox_kobj_u32(b, kobj_kthread_of(c, ws.tid));
        xbox_kobj_u8(b, (uint8_t)ws.exited);
        xbox_kobj_u32(b, ws.exit_code);
        xbox_kobj_u32(b, (uint32_t)ws.suspend_count);
        xbox_kobj_u32(b, (uint32_t)ws.priority);
        break;
    case XKOB_K_TIMER:
        xbox_kobj_u8(b, (uint8_t)ws.manual_reset);
        xbox_kobj_u8(b, (uint8_t)ws.signaled);
        xbox_kobj_u8(b, (uint8_t)ws.armed);
        xbox_kobj_u64(b, (uint64_t)ws.due_in_ns);
        xbox_kobj_u32(b, (uint32_t)ws.period_ms);
        break;
    case XKOB_K_FILE: {
        XboxFileState fs;
        char why[104];
        if (xbox_FileStateRead(h, &fs, why, sizeof why) != 1) {
            xbox_kobj_refuse(c, token, guest_va, "file", "%s", why);
            memset(&fs, 0, sizeof fs);
        }
        xbox_kobj_str(b, fs.path);
        xbox_kobj_u32(b, (uint32_t)fs.flags);
        xbox_kobj_u8(b, (uint8_t)fs.is_dir);
        xbox_kobj_u8(b, (uint8_t)fs.disc);
        xbox_kobj_u8(b, (uint8_t)fs.overwrite_pending);
        xbox_kobj_u64(b, (uint64_t)fs.offset);
        xbox_kobj_u32(b, fs.disc_next);
        xbox_kobj_u8(b, (uint8_t)fs.disc_scanning);
        xbox_kobj_str(b, fs.disc_pattern);
        break;
    }
    default:
        break;
    }
    if (!b->oom) {
        uint32_t n = (uint32_t)(b->len - at - 8);
        b->p[at + 4] = (uint8_t)n;
        b->p[at + 5] = (uint8_t)(n >> 8);
        b->p[at + 6] = (uint8_t)(n >> 16);
        b->p[at + 7] = (uint8_t)(n >> 24);
    }
}
#endif /* !_WIN32 */

uint32_t xbox_kobj_ref(XboxKobjSaveCtx *c, void *h, uint32_t token, uint32_t guest_va)
{
    size_t i;
    uint32_t id;

    if (!h)
        return 0;
    for (i = 0; i < c->nmap; i++)
        if (c->map[i].h == h)
            return c->map[i].id;
    if (c->nmap == c->capmap) {
        size_t cap = c->capmap ? c->capmap * 2 : 256;
        kobj_map *m = (kobj_map *)realloc(c->map, cap * sizeof *m);
        if (!m) {
            c->objs.oom = 1;
            return 0;
        }
        c->map = m;
        c->capmap = cap;
    }
    id = (uint32_t)c->nmap + 1;
    c->map[c->nmap].h = h;
    c->map[c->nmap].id = id;
    c->nmap++;
#ifndef _WIN32
    kobj_object(c, h, id, token, guest_va);
#else
    (void)token; (void)guest_va;
#endif
    return id;
}

#ifndef _WIN32
/* The host-side state ksync keeps for I/O completion ports, and the clock. */
static void kobj_save_misc(XboxKobjSaveCtx *c)
{
    XboxKobjBuf *b;
    int n, i;

    n = ks_port_list(NULL, 0);
    if (n > 0) {
        ks_port_info *pi = (ks_port_info *)calloc((size_t)n, sizeof *pi);
        if (pi) {
            n = ks_port_list(pi, n);
            for (i = 0; i < n; i++) {
                int k = ks_port_packets(pi[i].ident, NULL, 0), j;
                ks_packet *pk = k > 0 ? (ks_packet *)calloc((size_t)k, sizeof *pk) : NULL;
                uint32_t id = xbox_kobj_ref(c, pi[i].ident, 0, 0);
                if (pk) k = ks_port_packets(pi[i].ident, pk, k); else k = 0;
                b = xbox_kobj_rec_begin(c, XKOB_R_PORT);
                xbox_kobj_u32(b, id);
                xbox_kobj_u32(b, (uint32_t)pi[i].slot);
                xbox_kobj_u32(b, pi[i].gen);
                xbox_kobj_u32(b, (uint32_t)pi[i].max);
                xbox_kobj_u32(b, (uint32_t)pi[i].cur);
                xbox_kobj_u32(b, (uint32_t)k);
                for (j = 0; j < k; j++) {
                    xbox_kobj_u32(b, pk[j].key);
                    xbox_kobj_u32(b, pk[j].ctx);
                    xbox_kobj_u32(b, pk[j].status);
                    xbox_kobj_u32(b, pk[j].info);
                }
                xbox_kobj_rec_end(c);
                free(pk);
            }
            free(pi);
        }
    }
    n = ks_passoc_list(NULL, 0);
    if (n > 0) {
        ks_passoc_info *pa = (ks_passoc_info *)calloc((size_t)n, sizeof *pa);
        if (pa) {
            n = ks_passoc_list(pa, n);
            for (i = 0; i < n; i++) {
                uint32_t f = xbox_kobj_ref(c, pa[i].file, 0, 0);
                uint32_t p = xbox_kobj_ref(c, pa[i].port, 0, 0);
                b = xbox_kobj_rec_begin(c, XKOB_R_PASSOC);
                xbox_kobj_u32(b, f);
                xbox_kobj_u32(b, p);
                xbox_kobj_u32(b, pa[i].key);
                xbox_kobj_rec_end(c);
            }
            free(pa);
        }
    }
    b = xbox_kobj_rec_begin(c, XKOB_R_APCPEND);
    xbox_kobj_u32(b, (uint32_t)ks_apc_pending_get());
    xbox_kobj_rec_end(c);

    /* The guest clock: kernel timers are stored relative to it; this says
     * where it stood (KeTickCount, ms). */
    b = xbox_kobj_rec_begin(c, XKOB_R_CLOCK);
    xbox_kobj_u64(b, xbox_GuestTickCount64());
    xbox_kobj_rec_end(c);
}

#endif /* !_WIN32 */

static void kobj_save_free(XboxKobjSaveCtx *c)
{
    free(c->objs.p);
    free(c->recs.p);
    free(c->map);
    free(c->thr);
}

/* Runs the whole save; *out is set only when nothing refused. */
static int kobj_run_save(unsigned flags, XboxKobjRefusals *why, void **out, size_t *len)
{
    XboxKobjSaveCtx c;
    XboxKobjBuf all;
    int refusals;

    memset(&c, 0, sizeof c);
    memset(&all, 0, sizeof all);
    c.flags = flags;
    c.why = why;
    if (why)
        memset(why, 0, sizeof *why);
#ifdef _WIN32
    xbox_kobj_refuse(&c, 0, 0, "kernel", "this host's kernel objects are Win32 objects, "
                     "whose state cannot be read (POSIX hosts only)");
#else
    xbox_bridge_kobj_handles(kobj_note_thread, &c);
    xbox_bridge_kobj_save(&c);
    xbox_rtl_kobj_save(&c);
    kobj_save_misc(&c);
#endif
    if (c.objs.oom || c.recs.oom)
        xbox_kobj_refuse(&c, 0, 0, "kernel", "out of memory");
    refusals = c.refusals;
    if (!refusals && out) {
        static const uint8_t magic[4] = { 'X', 'K', 'O', 'B' };
        xbox_kobj_put(&all, magic, 4);
        xbox_kobj_u32(&all, XBOX_KOBJ_VERSION);
        if (c.objs.len) xbox_kobj_put(&all, c.objs.p, c.objs.len);
        if (c.recs.len) xbox_kobj_put(&all, c.recs.p, c.recs.len);
        xbox_kobj_u16(&all, XKOB_R_END);
        xbox_kobj_u16(&all, 1);
        xbox_kobj_u32(&all, 0);
        if (all.oom) {
            free(all.p);
            refusals = 1;
            if (why) {
                why->count = 1;
                snprintf(why->r[0].what, sizeof why->r[0].what, "kernel");
                snprintf(why->r[0].reason, sizeof why->r[0].reason, "out of memory");
            }
        } else {
            *out = all.p;
            *len = all.len;
        }
    }
    kobj_save_free(&c);
    return refusals;
}

int xbox_KobjCheck(unsigned flags, XboxKobjRefusals *why)
{
    return kobj_run_save(flags, why, NULL, NULL);
}

int xbox_KobjSave(void **out, size_t *len, unsigned flags, XboxKobjRefusals *why)
{
    if (!out || !len)
        return 0;
    *out = NULL;
    *len = 0;
    return kobj_run_save(flags, why, out, len) == 0;
}

/* ── Reset ────────────────────────────────────────────────────────────────── */

void xbox_KobjReset(void)
{
    ks_port_reset();               /* before the handles that name the ports go */
    xbox_bridge_kobj_reset();
    xbox_rtl_kobj_reset();
    ks_apc_pending_set(0);
}

/* ── Load ─────────────────────────────────────────────────────────────────── */

typedef struct { uint32_t id; void *h; } kobj_made;
typedef struct { uint32_t tid; void *h; int used; } kobj_live;

struct XboxKobjLoadCtx {
    unsigned   flags;
    kobj_made *made;
    size_t     nmade, capmade;
    kobj_live *live;
    size_t     nlive, caplive;
    char      *err;
    size_t     errlen;
    int        failed;
};

unsigned xbox_kobj_load_flags(const XboxKobjLoadCtx *c) { return c->flags; }
int xbox_kobj_failed(const XboxKobjLoadCtx *c) { return c->failed; }

void xbox_kobj_fail(XboxKobjLoadCtx *c, const char *fmt, ...)
{
    if (!c->failed && c->err && c->errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, c->errlen, fmt, ap);
        va_end(ap);
    }
    c->failed = 1;
}

void *xbox_kobj_obj(XboxKobjLoadCtx *c, uint32_t id)
{
    size_t i;
    HANDLE dup = NULL;
    if (!id)
        return NULL;
    for (i = 0; i < c->nmade; i++)
        if (c->made[i].id == id) {
            if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)c->made[i].h, GetCurrentProcess(),
                                 &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return NULL;
            return dup;
        }
    return NULL;
}

/* The threads a resume across runs made for the saved ones (XBA-250): saved host thread id -> new thread. */
static struct { uint32_t tid; void *h; } s_resume_thr[64];
static int s_resume_nthr;

void xbox_KobjResumeThreads(const uint32_t *tids, void *const *handles, int n)
{
    int i;
    s_resume_nthr = 0;
    for (i = 0; tids && handles && i < n && i < 64; i++) {
        s_resume_thr[s_resume_nthr].tid = tids[i];
        s_resume_thr[s_resume_nthr].h = handles[i];
        s_resume_nthr++;
    }
}

#ifndef _WIN32
/* A save with XBOX_KOBJ_RESUME_THREADS: the running thread is one the state holds (its tid named). */
static int kobj_resume_known(uint32_t tid)
{
    int i;
    for (i = 0; i < s_resume_nthr; i++)
        if (s_resume_thr[i].tid == tid)
            return 1;
    return 0;
}

/* The new thread standing for saved host thread `tid` (one more reference), or NULL. */
static HANDLE kobj_resume_thread(uint32_t tid)
{
    int i;
    HANDLE dup = NULL;
    for (i = 0; tid && i < s_resume_nthr; i++)
        if (s_resume_thr[i].tid == tid && s_resume_thr[i].h
            && DuplicateHandle(GetCurrentProcess(), (HANDLE)s_resume_thr[i].h, GetCurrentProcess(), &dup, 0,
                               FALSE, DUPLICATE_SAME_ACCESS))
            return dup;
    return NULL;
}

/* Its host thread id now. */
static uint32_t kobj_resume_tid(uint32_t tid)
{
    int i;
    w32_state ws;
    for (i = 0; tid && i < s_resume_nthr; i++)
        if (s_resume_thr[i].tid == tid && s_resume_thr[i].h
            && w32_state_read((HANDLE)s_resume_thr[i].h, &ws) == W32_STATE_THREAD)
            return ws.tid;
    return 0;
}
#endif

void *xbox_kobj_live_thread(XboxKobjLoadCtx *c, uint32_t tid)
{
    size_t i;
    HANDLE dup = NULL;
    for (i = 0; tid && i < c->nlive; i++)
        if (c->live[i].tid == tid) {
            c->live[i].used = 1;
            if (DuplicateHandle(GetCurrentProcess(), (HANDLE)c->live[i].h, GetCurrentProcess(),
                                &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return dup;
        }
    return NULL;
}

#ifndef _WIN32
/* Before the reset: every running guest thread a table names, by host
 * thread id, with a reference of our own so the reset cannot free it. */
static void kobj_collect_live(void *ctx, uint32_t guest_va, void *h)
{
    XboxKobjLoadCtx *c = (XboxKobjLoadCtx *)ctx;
    w32_state ws;
    HANDLE dup = NULL;
    size_t i;
    (void)guest_va;
    if (w32_state_read((HANDLE)h, &ws) != W32_STATE_THREAD || ws.exited)
        return;
    for (i = 0; i < c->nlive; i++)
        if (c->live[i].tid == ws.tid)
            return;
    if (c->nlive == c->caplive) {
        size_t cap = c->caplive ? c->caplive * 2 : 32;
        kobj_live *l = (kobj_live *)realloc(c->live, cap * sizeof *l);
        if (!l)
            return;
        c->live = l;
        c->caplive = cap;
    }
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)h, GetCurrentProcess(), &dup, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return;
    c->live[c->nlive].tid = ws.tid;
    c->live[c->nlive].h = dup;
    c->live[c->nlive].used = 0;
    c->nlive++;
}

/* Before the reset: a thread blocked in a host wait on an object the reset
 * closes would wake on freed memory. The threads must be out of their waits
 * first (XBA-249 brings parked threads out and back in). Events count their
 * blocked waiters; that is the check that can be made. */
static void kobj_find_blocked(void *ctx, uint32_t guest_va, void *h)
{
    XboxKobjLoadCtx *c = (XboxKobjLoadCtx *)ctx;
    w32_state ws;
    if (w32_state_read((HANDLE)h, &ws) == W32_STATE_EVENT && ws.waiters)
        xbox_kobj_fail(c, "%u thread(s) blocked on an event (guest 0x%08X): bring them out of "
                          "their waits before a load", ws.waiters, guest_va);
}

static void kobj_load_object(XboxKobjLoadCtx *c, XboxKobjRd *r)
{
    uint32_t id = xbox_kobj_get32(r);
    uint16_t kind = xbox_kobj_get16(r);
    HANDLE h = NULL;
    w32_state ws;
    char why[160] = "";

    (void)xbox_kobj_get32(r);   /* the reference count at the save */
    memset(&ws, 0, sizeof ws);
    switch (kind) {
    case XKOB_K_EVENT:
        ws.kind = W32_STATE_EVENT;
        ws.manual_reset = xbox_kobj_get8(r);
        ws.signaled = xbox_kobj_get8(r);
        h = w32_state_make(&ws);
        break;
    case XKOB_K_SEMAPHORE:
        ws.kind = W32_STATE_SEMAPHORE;
        ws.count = (LONG)xbox_kobj_get32(r);
        ws.maximum = (LONG)xbox_kobj_get32(r);
        h = w32_state_make(&ws);
        break;
    case XKOB_K_MUTANT:
        ws.kind = W32_STATE_MUTEX;
        ws.owner_tid = xbox_kobj_get32(r);
        (void)xbox_kobj_get32(r);   /* the owner's KTHREAD, informative */
        ws.recursion = (int)xbox_kobj_get32(r);
        /* Across runs (XBA-250): owned by the thread that stands for its saved owner now. */
        if (ws.owner_tid && (c->flags & XBOX_KOBJ_ACROSS_RUNS) && !(ws.owner_tid = kobj_resume_tid(ws.owner_tid))) {
            snprintf(why, sizeof why, "mutant owned by a thread that is not made again");
            break;
        }
        h = w32_state_make(&ws);
        break;
    case XKOB_K_THREAD:
        ws.kind = W32_STATE_THREAD;
        ws.tid = xbox_kobj_get32(r);
        {
            uint32_t kt = xbox_kobj_get32(r);
            ws.exited = xbox_kobj_get8(r);
            ws.exit_code = xbox_kobj_get32(r);
            ws.suspend_count = (int)xbox_kobj_get32(r);
            ws.priority = (int)xbox_kobj_get32(r);
            if (ws.exited) {
                h = w32_state_make(&ws);
            } else if (c->flags & XBOX_KOBJ_ACROSS_RUNS) {
                /* Made again by the resume (XBA-250): the new host thread of the saved one. */
                if (!(h = kobj_resume_thread(ws.tid)))
                    snprintf(why, sizeof why, "thread (KTHREAD 0x%08X) was running and is not made "
                             "again in this run", kt);
            } else if (!(h = xbox_kobj_live_thread(c, ws.tid))) {
                snprintf(why, sizeof why, "thread %u (KTHREAD 0x%08X) is not running now",
                         (unsigned)ws.tid, kt);
            }
        }
        break;
    case XKOB_K_TIMER:
        ws.kind = W32_STATE_TIMER;
        ws.manual_reset = xbox_kobj_get8(r);
        ws.signaled = xbox_kobj_get8(r);
        ws.armed = xbox_kobj_get8(r);
        ws.due_in_ns = (long long)xbox_kobj_get64(r);
        ws.period_ms = (LONG)xbox_kobj_get32(r);
        h = w32_state_make(&ws);
        break;
    case XKOB_K_FILE: {
        XboxFileState fs;
        memset(&fs, 0, sizeof fs);
        xbox_kobj_getstr(r, fs.path, sizeof fs.path);
        fs.flags = (int)xbox_kobj_get32(r);
        fs.is_dir = xbox_kobj_get8(r);
        fs.disc = xbox_kobj_get8(r);
        fs.overwrite_pending = xbox_kobj_get8(r);
        fs.offset = (int64_t)xbox_kobj_get64(r);
        fs.disc_next = xbox_kobj_get32(r);
        fs.disc_scanning = xbox_kobj_get8(r);
        xbox_kobj_getstr(r, fs.disc_pattern, sizeof fs.disc_pattern);
        if (!r->bad)
            h = xbox_FileStateReopen(&fs, why, sizeof why);
        break;
    }
    default:
        snprintf(why, sizeof why, "object %u of unknown kind %u", id, kind);
        break;
    }
    if (r->bad) {
        xbox_kobj_fail(c, "object %u: record truncated", id);
    } else if (!h) {
        xbox_kobj_fail(c, "object %u: %s", id, why[0] ? why : "could not be made");
        return;
    }
    if (!h)
        return;
    if (c->nmade == c->capmade) {
        size_t cap = c->capmade ? c->capmade * 2 : 256;
        kobj_made *m = (kobj_made *)realloc(c->made, cap * sizeof *m);
        if (!m) {
            CloseHandle(h);
            xbox_kobj_fail(c, "out of memory");
            return;
        }
        c->made = m;
        c->capmade = cap;
    }
    c->made[c->nmade].id = id;
    c->made[c->nmade].h = h;
    c->nmade++;
}

static void kobj_load_misc(XboxKobjLoadCtx *c, uint16_t type, XboxKobjRd *r)
{
    if (type == XKOB_R_PORT) {
        ks_port_info pi;
        ks_packet *pk = NULL;
        uint32_t id = xbox_kobj_get32(r), n, j;
        memset(&pi, 0, sizeof pi);
        pi.slot = (int)xbox_kobj_get32(r);
        pi.gen = xbox_kobj_get32(r);
        pi.max = (int32_t)xbox_kobj_get32(r);
        pi.cur = (int32_t)xbox_kobj_get32(r);
        n = xbox_kobj_get32(r);
        if (n > (r->len - r->pos) / 16) {
            xbox_kobj_fail(c, "port record truncated");
            return;
        }
        if (n && !(pk = (ks_packet *)calloc(n, sizeof *pk))) {
            xbox_kobj_fail(c, "out of memory");
            return;
        }
        for (j = 0; j < n; j++) {
            pk[j].key = xbox_kobj_get32(r);
            pk[j].ctx = xbox_kobj_get32(r);
            pk[j].status = xbox_kobj_get32(r);
            pk[j].info = xbox_kobj_get32(r);
        }
        pi.ident = xbox_kobj_obj(c, id);
        /* The port names its handle without a reference of its own: the
         * token's keeps it alive. */
        if (!pi.ident || !ks_port_restore(&pi, pk, (int)n))
            xbox_kobj_fail(c, "I/O completion port (object %u, slot %d) not made", id, pi.slot);
        if (pi.ident)
            CloseHandle((HANDLE)pi.ident);
        free(pk);
    } else if (type == XKOB_R_PASSOC) {
        uint32_t f = xbox_kobj_get32(r), p = xbox_kobj_get32(r), key = xbox_kobj_get32(r);
        HANDLE fh = (HANDLE)xbox_kobj_obj(c, f), ph = (HANDLE)xbox_kobj_obj(c, p);
        if (!fh || !ph || ks_port_associate(fh, ph, key) != 0)
            xbox_kobj_fail(c, "file %u not joined to port %u again", f, p);
        if (fh) CloseHandle(fh);
        if (ph) CloseHandle(ph);
    } else if (type == XKOB_R_APCPEND) {
        ks_apc_pending_set((long)xbox_kobj_get32(r));
    }
}
#endif /* !_WIN32 */

int xbox_KobjLoad(const void *blob, size_t len, unsigned flags, char *err, size_t errlen)
{
    XboxKobjLoadCtx c;
    XboxKobjRd r;
    size_t i;

    memset(&c, 0, sizeof c);
    c.flags = flags;
    c.err = err;
    c.errlen = errlen;
    if (err && errlen)
        err[0] = 0;

    memset(&r, 0, sizeof r);
    r.p = (const uint8_t *)blob;
    r.len = blob ? len : 0;
    if (r.len < 8 || memcmp(r.p, "XKOB", 4) != 0) {
        xbox_kobj_fail(&c, "not a kernel object section");
        return 0;
    }
    r.pos = 4;
    if (xbox_kobj_get32(&r) != XBOX_KOBJ_VERSION) {
        xbox_kobj_fail(&c, "kernel object section version %u, this build reads %u",
                       (unsigned)(r.p[4] | r.p[5] << 8), XBOX_KOBJ_VERSION);
        return 0;
    }
    /* The records must be whole before anything is torn down. */
    {
        XboxKobjRd s = r;
        int ended = 0;
        while (!s.bad && s.pos < s.len) {
            uint16_t type = xbox_kobj_get16(&s);
            uint32_t n;
            (void)xbox_kobj_get16(&s);
            n = xbox_kobj_get32(&s);
            if (s.bad || n > s.len - s.pos) { s.bad = 1; break; }
            s.pos += n;
            if (type == XKOB_R_END) { ended = 1; break; }
        }
        if (s.bad || !ended) {
            xbox_kobj_fail(&c, "kernel object section truncated");
            return 0;
        }
    }
#ifdef _WIN32
    xbox_kobj_fail(&c, "kernel object states are not supported on this host");
    return 0;
#else
    xbox_bridge_kobj_handles(kobj_find_blocked, &c);
    if (c.failed)
        return 0;
    if (!(flags & XBOX_KOBJ_ACROSS_RUNS))
        xbox_bridge_kobj_handles(kobj_collect_live, &c);
    xbox_KobjReset();

    while (!c.failed && r.pos < r.len) {
        uint16_t type = xbox_kobj_get16(&r);
        XboxKobjRd rec;
        uint32_t n;
        (void)xbox_kobj_get16(&r);   /* record version: all are 1 so far */
        n = xbox_kobj_get32(&r);
        if (type == XKOB_R_END)
            break;
        memset(&rec, 0, sizeof rec);
        rec.p = r.p + r.pos;
        rec.len = n;
        r.pos += n;
        if (type == XKOB_R_OBJECT)
            kobj_load_object(&c, &rec);
        else if (!xbox_bridge_kobj_load(&c, type, &rec)
                 && !xbox_rtl_kobj_load(&c, type, &rec))
            kobj_load_misc(&c, type, &rec);   /* unknown types: skipped */
        if (rec.bad)
            xbox_kobj_fail(&c, "record %u truncated", type);
    }

    /* The tables own their references now; ours go. */
    for (i = 0; i < c.nmade; i++)
        CloseHandle((HANDLE)c.made[i].h);
    for (i = 0; i < c.nlive; i++)
        CloseHandle((HANDLE)c.live[i].h);
    free(c.made);
    free(c.live);
    if (c.failed) {
        xbox_KobjReset();
        return 0;
    }
    return 1;
#endif
}
