/* psp_hle.c -- the PSP spike's minimal HLE kernel (XBA-224): what a PSPSDK debug-screen program imports, by NID.
 *
 * Each import stub runs `jr ra ; syscall N` (psp_load.c), N its index; the handler reads the arguments from a0-a3 /
 * t0-t3 and returns in v0 (v1). Unknown NIDs are logged once and return 0.
 *
 * Threads: a strict-priority scheduler (lower number first, FIFO among equals), switching only at kernel calls
 * (a call that blocks, starts or wakes a thread) and at the run loop's timer / VBlank wake-ups, never in a delay
 * slot. The running thread's context is the CPU itself; the others' is saved in psp_thread. A thread's ra is a
 * kernel stub (PSP_STUB_THREAD_RETURN) that exits it. Time: the CPU's cycle count at PSP_CPU_HZ; a frame is
 * PSP_CYCLES_PER_FRAME cycles, with a VBlank at its end; when every thread waits, time jumps to the next event.
 * Memory: the user partition is a pair of bump pointers (low blocks up from the module's end, high blocks and thread
 * stacks down from 0x0A000000); freeing gives nothing back (the spike's limit).
 *
 * NIDs and structure layouts are facts of the PSP's ABI (the PSPSDK headers and its stub libraries declare them). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp_internal.h"

#define A0 (m->h.r[4])
#define A1 (m->h.r[5])
#define A2 (m->h.r[6])
#define A3 (m->h.r[7])
#define T0 (m->h.r[8])
#define T1 (m->h.r[9])

#define ERR_NOT_FOUND 0x80010002u      /* ENOENT */
#define ERR_UNKNOWN_UID 0x800200CBu
#define ERR_NO_MEMORY 0x800200D9u      /* SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED */
#define ERR_UNKNOWN_THID 0x80020198u
#define ERR_UNKNOWN_SEMID 0x80020199u
#define ERR_NOT_DORMANT 0x800201A4u
#define ERR_NOT_SUPPORTED 0x80020001u

static void ret(psp_machine *m, uint32_t v)
{
    m->h.r[2] = v;
}

/* ---- threads ---- */

static psp_thread *cur_thread(psp_machine *m)
{
    return m->cur >= 0 ? &m->threads[m->cur] : NULL;
}

static psp_thread *thread_of(psp_machine *m, uint32_t uid)
{
    if (uid == 0)
        return cur_thread(m);
    for (int i = 0; i < PSP_MAX_THREADS; i++)
        if (m->threads[i].state != TH_FREE && (uint32_t)m->threads[i].uid == uid)
            return &m->threads[i];
    return NULL;
}

static void save(psp_machine *m, psp_thread *t)
{
    memcpy(t->r, m->h.r, sizeof t->r);
    memcpy(t->f, m->h.f, sizeof t->f);
    t->pc = m->h.pc;
    t->next_pc = m->h.next_pc;
    t->hi = m->h.hi;
    t->lo = m->h.lo;
    t->fcr31 = m->h.fcr31;
}

static void restore(psp_machine *m, psp_thread *t)
{
    memcpy(m->h.r, t->r, sizeof t->r);
    memcpy(m->h.f, t->f, sizeof t->f);
    m->h.pc = t->pc;
    m->h.next_pc = t->next_pc;
    m->h.hi = t->hi;
    m->h.lo = t->lo;
    m->h.fcr31 = t->fcr31;
}

static void make_ready(psp_machine *m, psp_thread *t)
{
    t->state = TH_READY;
    t->wait = WAIT_NONE;
    t->seq = ++m->seq;
}

static psp_thread *best_ready(psp_machine *m)
{
    psp_thread *best = NULL;
    for (int i = 0; i < PSP_MAX_THREADS; i++) {
        psp_thread *t = &m->threads[i];
        if (t->state == TH_READY && (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq)))
            best = t;
    }
    return best;
}

/* No thread running: run the best ready one. False when none is ready. */
bool psp_hle_schedule(psp_machine *m)
{
    if (m->cur >= 0)
        return true;
    psp_thread *t = best_ready(m);
    if (!t) {
        bool alive = false;
        for (int i = 0; i < PSP_MAX_THREADS; i++)
            alive |= m->threads[i].state == TH_WAITING;
        if (!alive)
            m->exited = true; /* no thread left: the program is over */
        return false;
    }
    restore(m, t);
    t->state = TH_RUNNING;
    m->cur = (int)(t - m->threads);
    m->switched = true;
    if (m->verbose >= 2)
        fprintf(stderr, "psp: run thread %d '%s' at 0x%08X\n", t->uid, t->name, m->h.pc);
    return true;
}

/* A ready thread of higher priority than the running one takes the CPU (not inside a delay slot). */
void psp_hle_preempt(psp_machine *m)
{
    psp_thread *c = cur_thread(m), *b = best_ready(m);
    if (!b)
        return;
    if (c) {
        if (b->prio >= c->prio || m->h.next_pc != m->h.pc + 4)
            return;
        save(m, c);
        make_ready(m, c);
        m->cur = -1;
    }
    psp_hle_schedule(m);
}

static void block(psp_machine *m, int wait, int id, uint64_t wake_at)
{
    psp_thread *t = cur_thread(m);
    if (!t)
        return;
    save(m, t);
    t->state = TH_WAITING;
    t->wait = wait;
    t->wait_id = id;
    t->wake_at = wake_at;
    m->cur = -1;
    m->switched = true;
    psp_hle_schedule(m);
}

static void exit_thread(psp_machine *m, uint32_t status, bool del)
{
    psp_thread *t = cur_thread(m);
    if (!t)
        return;
    if (m->verbose >= 2)
        fprintf(stderr, "psp: thread %d '%s' exits (0x%08X)\n", t->uid, t->name, status);
    t->exit_status = status;
    t->state = del ? TH_FREE : TH_DORMANT;
    m->cur = -1;
    m->switched = true;
    psp_hle_schedule(m);
}

uint64_t psp_hle_next_wake(psp_machine *m)
{
    uint64_t w = UINT64_MAX;
    for (int i = 0; i < PSP_MAX_THREADS; i++) {
        psp_thread *t = &m->threads[i];
        if (t->state == TH_WAITING && t->wait == WAIT_DELAY && t->wake_at < w)
            w = t->wake_at;
    }
    return w;
}

void psp_hle_timers(psp_machine *m)
{
    for (int i = 0; i < PSP_MAX_THREADS; i++) {
        psp_thread *t = &m->threads[i];
        if (t->state == TH_WAITING && t->wait == WAIT_DELAY && t->wake_at <= m->h.cycles)
            make_ready(m, t);
    }
}

void psp_hle_vblank(psp_machine *m)
{
    for (int i = 0; i < PSP_MAX_THREADS; i++) {
        psp_thread *t = &m->threads[i];
        if (t->state == TH_WAITING && t->wait == WAIT_VBLANK)
            make_ready(m, t);
    }
}

/* ---- objects and memory ---- */

static psp_object *new_object(psp_machine *m, int kind, uint32_t name_addr)
{
    for (int i = 0; i < PSP_MAX_OBJECTS; i++) {
        psp_object *o = &m->objs[i];
        if (o->kind == OBJ_FREE) {
            memset(o, 0, sizeof *o);
            o->kind = kind;
            o->uid = m->next_uid++;
            for (int k = 0; name_addr && k < 31; k++) {
                uint8_t *p = psp_ptr(m, name_addr + (uint32_t)k, 1);
                if (!p || !*p)
                    break;
                o->name[k] = (char)*p;
            }
            return o;
        }
    }
    return NULL;
}

static psp_object *object_of(psp_machine *m, uint32_t uid, int kind)
{
    for (int i = 0; i < PSP_MAX_OBJECTS; i++)
        if (m->objs[i].kind == kind && (uint32_t)m->objs[i].uid == uid)
            return &m->objs[i];
    return NULL;
}

static uint32_t alloc_user(psp_machine *m, uint32_t size, bool high)
{
    size = (size + 255u) & ~255u;
    if (size > m->heap_hi - m->heap_lo)
        return 0;
    if (high) {
        m->heap_hi -= size;
        return m->heap_hi;
    }
    uint32_t a = m->heap_lo;
    m->heap_lo += size;
    return a;
}

static int create_thread(psp_machine *m, uint32_t name_addr, const char *name, uint32_t entry, int prio,
                         uint32_t stack_size, uint32_t attr)
{
    for (int i = 0; i < PSP_MAX_THREADS; i++) {
        psp_thread *t = &m->threads[i];
        if (t->state != TH_FREE)
            continue;
        uint32_t stack = alloc_user(m, stack_size, true);
        if (!stack)
            return (int)ERR_NO_MEMORY;
        memset(t, 0, sizeof *t);
        t->state = TH_DORMANT;
        t->uid = m->next_uid++;
        if (name)
            snprintf(t->name, sizeof t->name, "%s", name);
        for (int k = 0; name_addr && k < 31; k++) {
            uint8_t *p = psp_ptr(m, name_addr + (uint32_t)k, 1);
            if (!p || !*p)
                break;
            t->name[k] = (char)*p;
        }
        t->entry = entry;
        t->init_prio = t->prio = prio;
        t->stack = stack;
        t->stack_size = (stack_size + 255u) & ~255u;
        t->attr = attr;
        t->gp = m->gp;
        return t->uid;
    }
    return (int)ERR_NO_MEMORY;
}

static int start_thread(psp_machine *m, psp_thread *t, uint32_t arglen, uint32_t argp)
{
    if (t->state != TH_DORMANT)
        return (int)ERR_NOT_DORMANT;
    memset(t->r, 0, sizeof t->r);
    memset(t->f, 0, sizeof t->f);
    t->hi = t->lo = t->fcr31 = 0;
    uint32_t sp = t->stack + t->stack_size;
    uint8_t *stack = psp_ptr(m, t->stack, t->stack_size);
    if (stack)
        memset(stack, 0xFF, t->stack_size); /* as the firmware fills a new stack */
    if (arglen && argp) {
        sp -= (arglen + 15u) & ~15u;
        uint8_t *dst = psp_ptr(m, sp, arglen), *src = psp_ptr(m, argp, arglen);
        if (dst && src)
            memmove(dst, src, arglen);
        t->r[5] = sp;
    }
    t->r[4] = arglen;
    sp -= 64;
    t->r[29] = sp;
    t->r[28] = t->gp;
    t->r[31] = PSP_STUB_THREAD_RETURN;
    t->pc = t->entry;
    t->next_pc = t->entry + 4;
    make_ready(m, t);
    return 0;
}

/* ---- handlers ---- */

static void h_zero(psp_machine *m) { ret(m, 0); }
static void h_unsupported(psp_machine *m) { ret(m, ERR_NOT_SUPPORTED); }
static void h_minus1(psp_machine *m) { ret(m, 0xFFFFFFFFu); }

/* sceDisplay */
static void h_display_set_frame_buf(psp_machine *m)
{
    m->fb_addr = A0;
    m->fb_width = A1;
    m->fb_format = A2;
    ret(m, 0);
}
static void h_wait_vblank(psp_machine *m)
{
    ret(m, 0);
    block(m, WAIT_VBLANK, 0, 0);
}
static void h_ge_edram_addr(psp_machine *m) { ret(m, 0x04000000u); }

/* sceCtrl */
static void h_ctrl_cycle(psp_machine *m)
{
    ret(m, m->ctrl_cycle);
    m->ctrl_cycle = A0;
}
static void h_ctrl_mode(psp_machine *m)
{
    ret(m, m->ctrl_mode);
    m->ctrl_mode = A0;
}
static void h_ctrl_read(psp_machine *m)
{
    uint32_t pad = A0, n = A1;
    for (uint32_t k = 0; k < n && k < 64; k++) {
        uint8_t *p = psp_ptr(m, pad + 16 * k, 16);
        if (!p)
            break;
        uint32_t ts = (uint32_t)(m->h.cycles / PSP_CYCLES_PER_US);
        memcpy(p, &ts, 4);
        memcpy(p + 4, &m->buttons, 4);
        p[8] = p[9] = p[10] = p[11] = 128;
        memset(p + 12, 0, 4);
    }
    ret(m, n);
}

/* sceUtility */
static void h_system_param_int(psp_machine *m)
{
    uint32_t v = A0 == 8 ? 1u : 0u; /* PSP_SYSTEMPARAM_ID_INT_LANGUAGE: English */
    psp_w32(m, A1, v);
    ret(m, 0);
}

/* IoFileMgrForUser: stdout / stderr only, no file system */
static void h_io_write(psp_machine *m)
{
    uint32_t fd = A0, buf = A1, len = A2;
    if (fd != 1 && fd != 2) {
        ret(m, ERR_NOT_FOUND);
        return;
    }
    uint8_t *p = psp_ptr(m, buf, len);
    if (p && len) {
        if (m->out_len + len + 1 > m->out_cap) {
            size_t cap = (m->out_len + len + 1) * 2;
            char *q = realloc(m->out, cap);
            if (q) {
                m->out = q;
                m->out_cap = cap;
            }
        }
        if (m->out_len + len + 1 <= m->out_cap) {
            memcpy(m->out + m->out_len, p, len);
            m->out_len += len;
            m->out[m->out_len] = 0;
        }
        if (m->verbose)
            fwrite(p, 1, len, stderr);
    }
    ret(m, len);
}
static void h_io_lseek(psp_machine *m)
{
    ret(m, 0);
    m->h.r[3] = 0;
}

/* Kernel_Library: LwMutex (workarea: count, owner, attr, waiters, uid) */
static void h_lwmutex_create(psp_machine *m)
{
    psp_object *o = new_object(m, OBJ_LWMUTEX, A1);
    if (!o) {
        ret(m, ERR_NO_MEMORY);
        return;
    }
    psp_w32(m, A0, A3);
    psp_w32(m, A0 + 4, A3 ? (uint32_t)(cur_thread(m) ? cur_thread(m)->uid : 0) : 0);
    psp_w32(m, A0 + 8, A2);
    psp_w32(m, A0 + 12, 0);
    psp_w32(m, A0 + 16, (uint32_t)o->uid);
    ret(m, 0);
}
static void h_lwmutex_delete(psp_machine *m)
{
    psp_object *o = object_of(m, psp_r32(m, A0 + 16), OBJ_LWMUTEX);
    if (o)
        o->kind = OBJ_FREE;
    ret(m, 0);
}
static void h_lwmutex_lock(psp_machine *m)
{
    /* Threads switch only in kernel calls, and nothing here blocks while holding one: never contended. */
    uint32_t me = cur_thread(m) ? (uint32_t)cur_thread(m)->uid : 0;
    uint32_t count = psp_r32(m, A0), owner = psp_r32(m, A0 + 4);
    if (count && owner != me && m->verbose)
        fprintf(stderr, "psp: LwMutex 0x%08X held by thread %u, taken by %u\n", A0, owner, me);
    psp_w32(m, A0, count + A1);
    psp_w32(m, A0 + 4, me);
    ret(m, 0);
}
static void h_lwmutex_unlock(psp_machine *m)
{
    uint32_t count = psp_r32(m, A0);
    count = count > A1 ? count - A1 : 0;
    psp_w32(m, A0, count);
    if (!count)
        psp_w32(m, A0 + 4, 0);
    ret(m, 0);
}

static void h_self_stop_unload(psp_machine *m) { exit_thread(m, A0, true); }

static void h_stdin(psp_machine *m) { ret(m, 0); }
static void h_stdout(psp_machine *m) { ret(m, 1); }
static void h_stderr(psp_machine *m) { ret(m, 2); }

/* SysMemUserForUser */
static void h_max_free_mem(psp_machine *m) { ret(m, m->heap_hi - m->heap_lo); }
static void h_alloc_partition(psp_machine *m)
{
    uint32_t type = A2, size = A3;
    uint32_t a = alloc_user(m, size, type == 1);
    psp_object *o = a ? new_object(m, OBJ_BLOCK, A1) : NULL;
    if (!o) {
        ret(m, ERR_NO_MEMORY);
        return;
    }
    o->addr = a;
    o->size = size;
    ret(m, (uint32_t)o->uid);
}
static void h_free_partition(psp_machine *m)
{
    psp_object *o = object_of(m, A0, OBJ_BLOCK);
    if (o)
        o->kind = OBJ_FREE;
    ret(m, o ? 0 : ERR_UNKNOWN_UID);
}
static void h_block_head(psp_machine *m)
{
    psp_object *o = object_of(m, A0, OBJ_BLOCK);
    ret(m, o ? o->addr : ERR_UNKNOWN_UID);
}

/* ThreadManForUser */
static void h_delay_thread(psp_machine *m)
{
    ret(m, 0);
    block(m, WAIT_DELAY, 0, m->h.cycles + (uint64_t)A0 * PSP_CYCLES_PER_US);
}
static void h_sema_create(psp_machine *m)
{
    psp_object *o = new_object(m, OBJ_SEMA, A0);
    if (!o) {
        ret(m, ERR_NO_MEMORY);
        return;
    }
    o->count = (int32_t)A2;
    o->max = (int32_t)A3;
    ret(m, (uint32_t)o->uid);
}
static void h_sema_delete(psp_machine *m)
{
    psp_object *o = object_of(m, A0, OBJ_SEMA);
    if (o)
        o->kind = OBJ_FREE;
    ret(m, o ? 0 : ERR_UNKNOWN_SEMID);
}
static void h_sema_signal(psp_machine *m)
{
    psp_object *o = object_of(m, A0, OBJ_SEMA);
    if (!o) {
        ret(m, ERR_UNKNOWN_SEMID);
        return;
    }
    o->count += (int32_t)A1;
    if (o->max > 0 && o->count > o->max)
        o->count = o->max;
    ret(m, 0);
    for (;;) { /* wake waiters, best priority first */
        psp_thread *best = NULL;
        for (int i = 0; i < PSP_MAX_THREADS; i++) {
            psp_thread *t = &m->threads[i];
            if (t->state == TH_WAITING && t->wait == WAIT_SEMA && t->wait_id == o->uid && t->wait_count <= o->count
                && (!best || t->prio < best->prio))
                best = t;
        }
        if (!best)
            break;
        o->count -= best->wait_count;
        best->r[2] = 0;
        make_ready(m, best);
    }
    psp_hle_preempt(m);
}
static void h_sema_wait(psp_machine *m)
{
    psp_object *o = object_of(m, A0, OBJ_SEMA);
    if (!o) {
        ret(m, ERR_UNKNOWN_SEMID);
        return;
    }
    int32_t need = (int32_t)A1;
    ret(m, 0);
    if (o->count >= need) {
        o->count -= need;
        return;
    }
    cur_thread(m)->wait_count = need;
    block(m, WAIT_SEMA, o->uid, 0);
}
static void h_create_thread(psp_machine *m)
{
    int uid = create_thread(m, A0, NULL, A1, (int)A2, A3, T0);
    if (m->verbose >= 2)
        fprintf(stderr, "psp: create thread entry 0x%08X prio %d stack 0x%X -> 0x%X\n", A1, (int)A2, A3, uid);
    ret(m, (uint32_t)uid);
}
static void h_delete_thread(psp_machine *m)
{
    psp_thread *t = thread_of(m, A0);
    if (!t || t == cur_thread(m)) {
        ret(m, ERR_UNKNOWN_THID);
        return;
    }
    t->state = TH_FREE;
    ret(m, 0);
}
static void h_start_thread(psp_machine *m)
{
    psp_thread *t = thread_of(m, A0);
    if (!t) {
        ret(m, ERR_UNKNOWN_THID);
        return;
    }
    ret(m, (uint32_t)start_thread(m, t, A1, A2));
    psp_hle_preempt(m);
}
static void h_exit_thread(psp_machine *m) { exit_thread(m, A0, false); }
static void h_exit_delete_thread(psp_machine *m) { exit_thread(m, A0, true); }
static void h_sleep_thread(psp_machine *m)
{
    ret(m, 0);
    block(m, WAIT_SLEEP, 0, 0);
}
static void h_get_thread_id(psp_machine *m)
{
    ret(m, cur_thread(m) ? (uint32_t)cur_thread(m)->uid : 0);
}
static void h_refer_thread_status(psp_machine *m)
{
    psp_thread *t = thread_of(m, A0);
    if (!t) {
        ret(m, ERR_UNKNOWN_THID);
        return;
    }
    uint32_t info = A1, size = psp_r32(m, A1);
    uint32_t f[26] = {0};
    f[0] = size;
    memcpy(&f[1], t->name, 32);
    f[9] = t->attr;
    f[10] = t->state == TH_RUNNING ? 1 : t->state == TH_READY ? 2 : t->state == TH_WAITING ? 4 : 16;
    f[11] = t->entry;
    f[12] = t->stack;
    f[13] = t->stack_size;
    f[14] = t->gp;
    f[15] = (uint32_t)t->init_prio;
    f[16] = (uint32_t)t->prio;
    f[17] = (uint32_t)t->wait;
    f[18] = (uint32_t)t->wait_id;
    f[20] = t->exit_status;
    for (uint32_t k = 0; k < 26 && 4 * k + 4 <= size; k++)
        psp_w32(m, info + 4 * k, f[k]);
    ret(m, 0);
}
static void h_create_callback(psp_machine *m)
{
    psp_object *o = new_object(m, OBJ_CALLBACK, A0);
    ret(m, o ? (uint32_t)o->uid : ERR_NO_MEMORY);
}
static void h_system_time_low(psp_machine *m)
{
    ret(m, (uint32_t)(m->h.cycles / PSP_CYCLES_PER_US));
}

/* LoadExecForUser */
static void h_exit_game(psp_machine *m)
{
    if (m->verbose)
        fprintf(stderr, "psp: sceKernelExitGame\n");
    m->exited = true;
    ret(m, 0);
    block(m, WAIT_SLEEP, 0, 0);
}

typedef struct hle_fn {
    const char *lib;
    uint32_t nid;
    const char *name;
    void (*fn)(psp_machine *m);
} hle_fn;

static const hle_fn table[] = {
    {"sceDisplay", 0x0E20F177u, "sceDisplaySetMode", h_zero},
    {"sceDisplay", 0x289D82FEu, "sceDisplaySetFrameBuf", h_display_set_frame_buf},
    {"sceDisplay", 0x984C27E7u, "sceDisplayWaitVblankStart", h_wait_vblank},
    {"sceDisplay", 0x46F186C3u, "sceDisplayWaitVblankStartCB", h_wait_vblank},
    {"sceGe_user", 0xE47E40E4u, "sceGeEdramGetAddr", h_ge_edram_addr},
    {"sceCtrl", 0x6A2774F3u, "sceCtrlSetSamplingCycle", h_ctrl_cycle},
    {"sceCtrl", 0x1F4011E6u, "sceCtrlSetSamplingMode", h_ctrl_mode},
    {"sceCtrl", 0x1F803938u, "sceCtrlReadBufferPositive", h_ctrl_read},
    {"sceCtrl", 0x3A622550u, "sceCtrlPeekBufferPositive", h_ctrl_read},
    {"sceUtility", 0xA5DA2406u, "sceUtilityGetSystemParamInt", h_system_param_int},
    {"sceNetInet", 0x8D7284EAu, "sceNetInetClose", h_minus1},
    {"sceNetInet", 0xCDA85C99u, "sceNetInetRecv", h_minus1},
    {"sceNetInet", 0x7AA671BCu, "sceNetInetSend", h_minus1},
    {"sceNetInet", 0xFBABE411u, "sceNetInetGetErrno", h_zero},
    {"IoFileMgrForUser", 0x810C4BC3u, "sceIoClose", h_zero},
    {"IoFileMgrForUser", 0x6A638D83u, "sceIoRead", h_zero},
    {"IoFileMgrForUser", 0x42EC03ACu, "sceIoWrite", h_io_write},
    {"IoFileMgrForUser", 0x27EB27B8u, "sceIoLseek", h_io_lseek},
    {"IoFileMgrForUser", 0xB29DDF9Cu, "sceIoDopen", h_unsupported},
    {"IoFileMgrForUser", 0xE3EB004Cu, "sceIoDread", h_zero},
    {"IoFileMgrForUser", 0xEB092469u, "sceIoDclose", h_zero},
    {"IoFileMgrForUser", 0x55F4717Du, "sceIoChdir", h_zero},
    {"IoFileMgrForUser", 0xACE946E8u, "sceIoGetstat", h_unsupported},
    {"IoFileMgrForUser", 0x109F50BCu, "sceIoOpen", h_unsupported},
    {"Kernel_Library", 0xBEA46419u, "sceKernelLockLwMutex", h_lwmutex_lock},
    {"Kernel_Library", 0x15B6446Bu, "sceKernelUnlockLwMutex", h_lwmutex_unlock},
    {"ModuleMgrForUser", 0xD675EBB8u, "sceKernelSelfStopUnloadModule", h_self_stop_unload},
    {"StdioForUser", 0x172D316Eu, "sceKernelStdin", h_stdin},
    {"StdioForUser", 0xA6BAB2E9u, "sceKernelStdout", h_stdout},
    {"StdioForUser", 0xF78BA90Au, "sceKernelStderr", h_stderr},
    {"SysMemUserForUser", 0xA291F107u, "sceKernelMaxFreeMemSize", h_max_free_mem},
    {"SysMemUserForUser", 0x237DBD4Fu, "sceKernelAllocPartitionMemory", h_alloc_partition},
    {"SysMemUserForUser", 0xB6D61D02u, "sceKernelFreePartitionMemory", h_free_partition},
    {"SysMemUserForUser", 0x9D9A5BA1u, "sceKernelGetBlockHeadAddr", h_block_head},
    {"ThreadManForUser", 0xCEADEB47u, "sceKernelDelayThread", h_delay_thread},
    {"ThreadManForUser", 0x68DA9E36u, "sceKernelDelayThreadCB", h_delay_thread},
    {"ThreadManForUser", 0xD6DA4BA1u, "sceKernelCreateSema", h_sema_create},
    {"ThreadManForUser", 0x28B6489Cu, "sceKernelDeleteSema", h_sema_delete},
    {"ThreadManForUser", 0x3F53E640u, "sceKernelSignalSema", h_sema_signal},
    {"ThreadManForUser", 0x4E3A1105u, "sceKernelWaitSema", h_sema_wait},
    {"ThreadManForUser", 0xF0B7DA1Cu, "sceKernelDeleteMsgPipe", h_unsupported},
    {"ThreadManForUser", 0x876DBFADu, "sceKernelSendMsgPipe", h_unsupported},
    {"ThreadManForUser", 0x884C9F90u, "sceKernelTrySendMsgPipe", h_unsupported},
    {"ThreadManForUser", 0x74829B76u, "sceKernelReceiveMsgPipe", h_unsupported},
    {"ThreadManForUser", 0xDF52098Fu, "sceKernelTryReceiveMsgPipe", h_unsupported},
    {"ThreadManForUser", 0x33BE4024u, "sceKernelReferMsgPipeStatus", h_unsupported},
    {"ThreadManForUser", 0x446D8DE6u, "sceKernelCreateThread", h_create_thread},
    {"ThreadManForUser", 0x9FA03CD3u, "sceKernelDeleteThread", h_delete_thread},
    {"ThreadManForUser", 0xF475845Du, "sceKernelStartThread", h_start_thread},
    {"ThreadManForUser", 0xAA73C935u, "sceKernelExitThread", h_exit_thread},
    {"ThreadManForUser", 0x809CE29Bu, "sceKernelExitDeleteThread", h_exit_delete_thread},
    {"ThreadManForUser", 0x9ACE131Eu, "sceKernelSleepThread", h_sleep_thread},
    {"ThreadManForUser", 0x82826F70u, "sceKernelSleepThreadCB", h_sleep_thread},
    {"ThreadManForUser", 0x293B45B8u, "sceKernelGetThreadId", h_get_thread_id},
    {"ThreadManForUser", 0x17C1684Eu, "sceKernelReferThreadStatus", h_refer_thread_status},
    {"ThreadManForUser", 0xE81CAF8Fu, "sceKernelCreateCallback", h_create_callback},
    {"ThreadManForUser", 0x369ED59Du, "sceKernelGetSystemTimeLow", h_system_time_low},
    {"ThreadManForUser", 0x60107536u, "sceKernelDeleteLwMutex", h_lwmutex_delete},
    {"ThreadManForUser", 0x19CFF145u, "sceKernelCreateLwMutex", h_lwmutex_create},
    {"LoadExecForUser", 0x05572A5Fu, "sceKernelExitGame", h_exit_game},
    {"LoadExecForUser", 0x4AC57943u, "sceKernelRegisterExitCallback", h_zero},
};

int psp_hle_handler_of(const char *lib, uint32_t nid)
{
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (table[i].nid == nid && !strcmp(table[i].lib, lib))
            return (int)i;
    return -1;
}

const char *psp_hle_name(int handler)
{
    return handler >= 0 && (size_t)handler < sizeof table / sizeof table[0] ? table[handler].name : "?";
}

void psp_hle_syscall(psp_machine *m, uint32_t code)
{
    if (code == PSP_SC_THREAD_RETURN) {
        exit_thread(m, m->h.r[2], false);
        return;
    }
    if ((int)code >= m->nimports) {
        psp_fail(m, "syscall 0x%05X: no such import", code);
        return;
    }
    psp_import *imp = &m->imports[code];
    if (m->verbose >= 3)
        fprintf(stderr, "psp: %s (%08X %08X %08X %08X) from 0x%08X\n", psp_hle_name(imp->handler), A0, A1, A2, A3,
                m->h.r[31]);
    if (imp->handler < 0) {
        bool seen = false;
        for (int i = 0; i < m->nunimpl; i++)
            seen |= m->unimpl[i] == imp->nid;
        if (!seen && m->nunimpl < 32)
            m->unimpl[m->nunimpl++] = imp->nid;
        if (!seen && m->verbose)
            fprintf(stderr, "psp: %s NID 0x%08X not implemented (returns 0)\n", imp->lib, imp->nid);
        ret(m, 0);
        return;
    }
    table[imp->handler].fn(m);
}

void psp_hle_init(psp_machine *m)
{
    m->cur = -1;
    m->ic = 1; /* interrupts enabled, as a user thread sees them */
    m->next_uid = 0x100;
    m->heap_lo = (m->mod_hi + 255u) & ~255u;
    m->heap_hi = PSP_USER_HI;
    /* the thread return stub: syscall PSP_SC_THREAD_RETURN; nop */
    psp_w32(m, PSP_STUB_THREAD_RETURN, (PSP_SC_THREAD_RETURN << 6) | 0x0Cu);
    psp_w32(m, PSP_STUB_THREAD_RETURN + 4, 0);
}

/* The module's start thread: module_start(args, argp) with argp the module's path, as the firmware calls it. */
int psp_hle_start_module(psp_machine *m)
{
    static const char path[] = "ms0:/PSP/GAME/XBA/EBOOT.PBP";
    uint32_t argp = alloc_user(m, sizeof path, true);
    uint8_t *p = argp ? psp_ptr(m, argp, sizeof path) : NULL;
    if (!p)
        return -1;
    memcpy(p, path, sizeof path);
    int uid = create_thread(m, 0, m->mod_name, m->entry, (int)m->start_prio, m->start_stack, m->start_attr);
    if (uid < 0)
        return -1;
    psp_thread *t = thread_of(m, (uint32_t)uid);
    if (start_thread(m, t, sizeof path, argp))
        return -1;
    return psp_hle_schedule(m) ? 0 : -1;
}
