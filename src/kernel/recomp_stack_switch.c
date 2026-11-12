/* Guest stack switching (XBA-101): recomp_stack_switch.h has the contract,
 * docs/technical/stack-switching.md the design and its limits.
 *
 * Every host thread that switches gets a table of contexts. Its own stack is
 * context 0 (the root); every other one owns a host stack. A context is
 * suspended inside recomp_stack_switch_ret, keyed by the guest esp it left
 * (saved_esp) and the return address its switching function was called with
 * (entry_ret). A switch that loads esp = saved_esp of a suspended context and
 * returns to its entry_ret resumes it; any other one starts a new context at
 * the address the guest ret pops.
 *
 * Two suspended contexts can share both keys: an engine that copies each
 * fiber's stack bytes into one shared area (Fable's loader and scripts) parks
 * every fiber at the same esp and the same yield. Each context also keeps the
 * first SNAP bytes of its guest stack at suspend time, and the one whose bytes
 * match the guest stack best is resumed: the engine copied that fiber's bytes
 * back before switching to it.
 *
 * Contexts never leave the host thread they were made on: lifted code caches
 * the address of the thread's g_cpu across calls, so a frame resumed on
 * another thread would run on the first thread's registers. */
#include "recomp_stack_switch.h"
#include "recomp_discovery.h"
#include "xbox_memory_layout.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

extern ptrdiff_t g_xbox_mem_offset;
#define SS_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define SS_PTR(va)   ((const uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(_WIN32)
#define SS_HOST 1
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ── Host context switch ────────────────────────────────────────────────
 *
 * recomp_ss_switch(save, load): push the callee-saved registers on this
 * stack, store its pointer in *save, load the stack `load` names and pop
 * them there. recomp_ss_boot is where a new stack starts: it calls
 * fn(arg), which never returns. */
#ifdef SS_HOST
void recomp_ss_switch(void **save, void *load) __asm__("recomp_ss_switch");
void recomp_ss_boot(void) __asm__("recomp_ss_boot");

#if defined(__APPLE__)
#define SS_CFI_START ""
#define SS_CFI_UNDEF(r) ""
#define SS_CFI_END ""
#else
/* The return address of the boot frame is undefined, so an unwinder (a
 * pthread_exit from a guest thread running on one of these stacks) stops
 * here instead of reading past the top. */
#define SS_CFI_START "    .cfi_startproc\n"
#define SS_CFI_UNDEF(r) "    .cfi_undefined " r "\n"
#define SS_CFI_END "    .cfi_endproc\n"
#endif

#if defined(__aarch64__)
#define SS_CTX_WORDS 20
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl recomp_ss_switch\n"
    "recomp_ss_switch:\n"
    "    sub  sp, sp, #0xA0\n"
    "    stp  x19, x20, [sp, #0x00]\n"
    "    stp  x21, x22, [sp, #0x10]\n"
    "    stp  x23, x24, [sp, #0x20]\n"
    "    stp  x25, x26, [sp, #0x30]\n"
    "    stp  x27, x28, [sp, #0x40]\n"
    "    stp  x29, x30, [sp, #0x50]\n"
    "    stp  d8,  d9,  [sp, #0x60]\n"
    "    stp  d10, d11, [sp, #0x70]\n"
    "    stp  d12, d13, [sp, #0x80]\n"
    "    stp  d14, d15, [sp, #0x90]\n"
    "    mov  x9, sp\n"
    "    str  x9, [x0]\n"
    "    mov  sp, x1\n"
    "    ldp  x19, x20, [sp, #0x00]\n"
    "    ldp  x21, x22, [sp, #0x10]\n"
    "    ldp  x23, x24, [sp, #0x20]\n"
    "    ldp  x25, x26, [sp, #0x30]\n"
    "    ldp  x27, x28, [sp, #0x40]\n"
    "    ldp  x29, x30, [sp, #0x50]\n"
    "    ldp  d8,  d9,  [sp, #0x60]\n"
    "    ldp  d10, d11, [sp, #0x70]\n"
    "    ldp  d12, d13, [sp, #0x80]\n"
    "    ldp  d14, d15, [sp, #0x90]\n"
    "    add  sp, sp, #0xA0\n"
    "    ret\n"
    ".p2align 2\n"
    ".globl recomp_ss_boot\n"
    "recomp_ss_boot:\n"
    SS_CFI_START
    SS_CFI_UNDEF("x30")
    "    mov  x0, x20\n"
    "    blr  x19\n"
    "    brk  #1\n"
    SS_CFI_END);

static void *ss_make(void *top, void (*fn)(void *), void *arg)
{
    void **sp = (void **)top - SS_CTX_WORDS;
    memset(sp, 0, SS_CTX_WORDS * sizeof *sp);
    sp[0] = (void *)fn;                 /* x19 */
    sp[1] = arg;                        /* x20 */
    sp[11] = (void *)recomp_ss_boot;    /* x30 */
    return sp;
}
#else /* x86-64, System V */
__asm__(
    ".text\n"
    ".globl recomp_ss_switch\n"
    "recomp_ss_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq  %rsp, (%rdi)\n"
    "    movq  %rsi, %rsp\n"
    "    popq  %r15\n"
    "    popq  %r14\n"
    "    popq  %r13\n"
    "    popq  %r12\n"
    "    popq  %rbx\n"
    "    popq  %rbp\n"
    "    ret\n"
    ".globl recomp_ss_boot\n"
    "recomp_ss_boot:\n"
    SS_CFI_START
    SS_CFI_UNDEF("rip")
    "    movq  %r12, %rdi\n"
    "    callq *%r13\n"
    "    ud2\n"
    SS_CFI_END);

static void *ss_make(void *top, void (*fn)(void *), void *arg)
{
    void **sp = (void **)top - 7;       /* r15 r14 r13 r12 rbx rbp, return */
    memset(sp, 0, 7 * sizeof *sp);
    sp[2] = (void *)fn;                 /* r13 */
    sp[3] = arg;                        /* r12 */
    sp[6] = (void *)recomp_ss_boot;
    return sp;
}
#endif
#endif /* SS_HOST */

/* ── Settings ───────────────────────────────────────────────────────── */

static int s_enabled = -1;          /* RECOMP_STACK_SWITCH, read once */
static size_t s_stack_bytes;        /* RECOMP_STACK_SWITCH_STACK_MB */
static unsigned s_max;              /* RECOMP_STACK_SWITCH_MAX */
static volatile unsigned s_started; /* contexts started, every thread */
static volatile uint32_t s_next_id; /* last context id handed out */

static unsigned env_uint(const char *name, unsigned def, unsigned lo, unsigned hi)
{
    const char *v = getenv(name);
    char *end;
    unsigned long n;
    if (!v || !*v)
        return def;
    n = strtoul(v, &end, 10);
    if (*end || n < lo || n > hi) {
        fprintf(stderr, "[STACK] %s=%s ignored (%u to %u)\n", name, v, lo, hi);
        return def;
    }
    return (unsigned)n;
}

int recomp_stack_switch_enabled(void)
{
    if (s_enabled < 0) {
        const char *v = getenv("RECOMP_STACK_SWITCH");
#ifdef SS_HOST
        int on = !(v && !strcmp(v, "0"));
#else
        int on = 0;
        (void)v;
#endif
        s_stack_bytes = (size_t)env_uint("RECOMP_STACK_SWITCH_STACK_MB", 8, 1, 256) << 20;
        s_max = env_uint("RECOMP_STACK_SWITCH_MAX", 1024, 4, 65536);
        s_enabled = on;
    }
    return s_enabled;
}

/* ── Reporting ──────────────────────────────────────────────────────── */

static void ss_blocker(uint32_t site, uint32_t to, const char *why)
{
    static volatile unsigned said;
    if (said++ < 16)
        fprintf(stderr, "[STACK] stack switch at 0x%08X to 0x%08X: %s\n", site, to, why);
    recomp_discovery_stack_switch(site, to, why);
}

void recomp_stack_switch_unhandled(uint32_t site)
{
    ss_blocker(site, 0, "no ret after the esp load in the function");
}

unsigned recomp_stack_switch_started(void)
{
    return s_started;
}

#ifndef SS_HOST

void recomp_stack_switch_ret(uint32_t saved_esp, uint32_t loaded_esp, uint32_t entry_ret,
                             uint32_t ret_imm, uint32_t site)
{
    (void)saved_esp; (void)loaded_esp; (void)entry_ret; (void)ret_imm;
    ss_blocker(site, SS_MEM32(g_esp), "this host has no stack switch");
}

uint32_t recomp_stack_context(void) { return 0; }
unsigned recomp_stack_switch_live(void) { return 0; }

#else /* SS_HOST */

/* ── Contexts ───────────────────────────────────────────────────────── */

#define SNAP 64     /* bytes of guest stack kept to tell twins apart */

enum { SS_FREE, SS_RUNNING, SS_SUSPENDED };

typedef struct ss_ctx {
    uint32_t id;
    int      state;
    uint32_t saved_esp;     /* suspended: the guest esp it left */
    uint32_t entry_ret;     /* suspended: where its switching function returns */
    uint32_t ret_imm;       /* suspended: N of that function's `ret N` */
    uint32_t start_va;      /* new: the guest address it starts at */
    uint32_t start_esp;     /* new: the guest esp it starts with */
    uint64_t last;          /* suspend tick, for eviction */
    void    *map;           /* host stack mapping (guard page + stack); NULL: root */
    void    *sp;            /* host stack pointer while not running */
    uint8_t  snap[SNAP];
} ss_ctx;

typedef struct ss_thread {
    ss_ctx   root;
    ss_ctx **ctx;           /* every context but the root */
    unsigned n, cap;
    ss_ctx  *cur;
    uint64_t tick;
} ss_thread;

static RECOMP_TLS ss_thread *t_ss;
static pthread_key_t s_key;
static pthread_once_t s_key_once = PTHREAD_ONCE_INIT;
static size_t s_page;

/* A thread that ends gives its stacks back, except the one it may be ending
 * on (bionic runs the destructors on the exiting stack). */
static void ss_thread_free(void *p)
{
    ss_thread *t = p;
    for (unsigned i = 0; i < t->n; i++) {
        ss_ctx *c = t->ctx[i];
        if (c != t->cur && c->map)
            munmap(c->map, s_stack_bytes + s_page);
        if (c != t->cur)
            free(c);
    }
    free(t->ctx);
    if (t->cur == &t->root)
        free(t);
}

static void ss_key_init(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    s_page = pg > 0 ? (size_t)pg : 4096;
    pthread_key_create(&s_key, ss_thread_free);
}

static ss_thread *ss_self(void)
{
    if (!t_ss) {
        pthread_once(&s_key_once, ss_key_init);
        t_ss = calloc(1, sizeof *t_ss);
        if (!t_ss) {
            fprintf(stderr, "[STACK] out of memory\n");
            abort();
        }
        t_ss->root.state = SS_RUNNING;
        t_ss->cur = &t_ss->root;
        pthread_setspecific(s_key, t_ss);
    }
    return t_ss;
}

uint32_t recomp_stack_context(void)
{
    return t_ss ? t_ss->cur->id : 0;
}

unsigned recomp_stack_switch_live(void)
{
    unsigned live = 0;
    if (!t_ss)
        return 0;
    for (unsigned i = 0; i < t_ss->n; i++)
        live += t_ss->ctx[i]->state != SS_FREE;
    return live + 1;
}

/* Bytes of guest stack kept from esp: SNAP, but never past the end of esp's
 * guest page. A stack can end at a page boundary with nothing mapped after
 * it (a fiber that yields at once from a freshly allocated stack), and what
 * lies past its top is not the context's anyway. The same esp always gives
 * the same length, so twins compare equal lengths. */
static unsigned ss_snap_len(uint32_t esp)
{
    unsigned to_page_end = 0x1000u - (esp & 0xFFFu);
    return to_page_end < SNAP ? to_page_end : SNAP;
}

static void ss_snap(uint8_t *dst, uint32_t esp)
{
    memcpy(dst, SS_PTR(esp), ss_snap_len(esp));
}

/* Matching leading bytes of a context's snapshot and the guest stack now. */
static unsigned ss_score(const ss_ctx *c, uint32_t esp)
{
    const uint8_t *now = SS_PTR(esp);
    unsigned n = ss_snap_len(esp), k = 0;
    while (k < n && c->snap[k] == now[k])
        k++;
    return k;
}

static ss_ctx *ss_find(ss_thread *t, uint32_t esp, uint32_t ret)
{
    ss_ctx *best = NULL;
    unsigned best_score = 0, twins = 0;

    for (unsigned i = 0; i <= t->n; i++) {
        ss_ctx *c = i < t->n ? t->ctx[i] : &t->root;
        unsigned score;
        if (c->state != SS_SUSPENDED || c->saved_esp != esp || c->entry_ret != ret)
            continue;
        twins++;
        score = ss_score(c, esp);
        if (!best || score > best_score || (score == best_score && c->last > best->last)) {
            best = c;
            best_score = score;
        }
    }
    if (twins > 1 && best_score < ss_snap_len(esp)) {
        static volatile unsigned said;
        if (said++ < 8)
            fprintf(stderr, "[STACK] %u contexts parked at esp 0x%08X returning to 0x%08X; "
                            "resuming the closest (%u of %u bytes match)\n",
                    twins, esp, ret, best_score, ss_snap_len(esp));
    }
    return best;
}

/* A context for a new guest stack: a free one, a new one, or, at the limit,
 * the one suspended longest ago (its host frames are dropped). */
static ss_ctx *ss_alloc(ss_thread *t, uint32_t site)
{
    ss_ctx *c = NULL, *lru = NULL;

    for (unsigned i = 0; i < t->n; i++) {
        ss_ctx *x = t->ctx[i];
        if (x->state == SS_FREE) {
            c = x;
            break;
        }
        if (x->state == SS_SUSPENDED && (!lru || x->last < lru->last))
            lru = x;
    }
    if (!c && t->n + 1 >= s_max && lru) {
        static volatile unsigned said;
        if (said++ < 8)
            fprintf(stderr, "[STACK] %u contexts (RECOMP_STACK_SWITCH_MAX): dropping the one "
                            "parked longest, at esp 0x%08X returning to 0x%08X\n",
                    t->n + 1, lru->saved_esp, lru->entry_ret);
        c = lru;
    }
    if (!c) {
        if (t->n == t->cap) {
            unsigned cap = t->cap ? t->cap * 2 : 16;
            ss_ctx **grown = realloc(t->ctx, cap * sizeof *grown);
            if (!grown)
                return NULL;
            t->ctx = grown;
            t->cap = cap;
        }
        c = calloc(1, sizeof *c);
        if (!c)
            return NULL;
        t->ctx[t->n++] = c;
    }
    if (!c->map) {
        void *m = mmap(NULL, s_stack_bytes + s_page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON
#ifdef MAP_NORESERVE
                       | MAP_NORESERVE
#endif
                       , -1, 0);
        if (m == MAP_FAILED) {
            fprintf(stderr, "[STACK] cannot map a %zu-byte host stack for the switch at 0x%08X\n",
                    s_stack_bytes, site);
            c->state = SS_FREE;
            return NULL;
        }
        mprotect(m, s_page, PROT_NONE);     /* guard page: an overflow faults */
        c->map = m;
    }
    c->id = __atomic_add_fetch(&s_next_id, 1, __ATOMIC_RELAXED);
    return c;
}

static recomp_func_t ss_func(uint32_t va)
{
    recomp_func_t f = recomp_lookup_manual(va);
    return f ? f : recomp_lookup(va);
}

/* The bottom of every new context: the guest ret landed at start_va with
 * esp = start_esp. Run that function, and when it returns, continue where its
 * own ret went (the return address it was entered with), as long as the
 * guest keeps returning into code. A coroutine whose body returns into an
 * exit routine that switches away for good ends that way. */
static void ss_main(void *arg)
{
    ss_ctx *c = arg;
    uint32_t va = c->start_va;

    g_esp = c->start_esp;
    for (;;) {
        recomp_func_t f = ss_func(va);
        uint32_t next;
        if (!f) {
            recomp_discovery_icall(va, c->start_va);
            ss_blocker(c->start_va, va, "a new stack returned into code with no function");
            fflush(stderr);
            abort();
        }
        next = SS_MEM32(g_esp);     /* what its ret pops */
        f();
        va = next;
    }
}

void recomp_stack_switch_ret(uint32_t saved_esp, uint32_t loaded_esp, uint32_t entry_ret,
                             uint32_t ret_imm, uint32_t site)
{
    ss_thread *t;
    ss_ctx *cur, *to;
    uint32_t ret_va;

    if (!recomp_stack_switch_enabled() || loaded_esp == saved_esp)
        return;
    t = ss_self();
    cur = t->cur;
    ret_va = SS_MEM32(g_esp);

    to = ss_find(t, loaded_esp, ret_va);    /* never cur: it is running */
    if (to) {
        /* It pops `ret N` of its own function: leave esp where this one's
         * ret would have left it. */
        g_esp += ret_imm - to->ret_imm;
    } else {
        if (!ss_func(ret_va)) {
            /* Somewhere in the middle of a function, with no context parked
             * there: nothing to resume. The old behaviour, said. */
            recomp_discovery_icall(ret_va, site);
            ss_blocker(site, ret_va, "resumes where no context is parked and no function starts");
            return;
        }
        to = ss_alloc(t, site);
        if (!to) {
            ss_blocker(site, ret_va, "no host stack for a new context");
            return;
        }
        to->start_va = ret_va;
        to->start_esp = g_esp + 4 + ret_imm;
        to->sp = ss_make((char *)to->map + s_page + s_stack_bytes, ss_main, to);
        {
            unsigned n = __atomic_add_fetch(&s_started, 1, __ATOMIC_RELAXED);
            if (n <= 4 || !(n & (n - 1)))
                fprintf(stderr, "[STACK] context %u: new guest stack at esp 0x%08X, starts at "
                                "0x%08X (switch at 0x%08X; %u started)\n",
                        to->id, to->start_esp, ret_va, site, n);
        }
    }

    cur->saved_esp = saved_esp;
    cur->entry_ret = entry_ret;
    cur->ret_imm = ret_imm;
    ss_snap(cur->snap, saved_esp);
    cur->state = SS_SUSPENDED;
    cur->last = ++t->tick;
    to->state = SS_RUNNING;
    t->cur = to;
    recomp_ss_switch(&cur->sp, to->sp);
    /* Resumed: whoever switched here set t->cur to this context and g_esp at
     * the return address this function's caller pops. */
}

#endif /* SS_HOST */
