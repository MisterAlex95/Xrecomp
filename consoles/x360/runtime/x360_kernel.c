/* x360_kernel.c -- a minimal high-level emulation of xboxkrnl.exe and xam.xex (XBA-270).
 *
 * An import thunk lands here with the arguments in r3..r10 (the Xbox 360 calling convention: 64-bit registers,
 * 32-bit pointers) and returns in r3. Ordinals come from x360_exports.inc (Xenia's tables); the behaviour follows
 * the Windows NT / Xbox kernel APIs these exports are, as Xenia documents them. Guest structures (dispatcher headers,
 * critical sections, ANSI strings) are read and written in guest memory, big-endian, so code the title inlines on
 * them agrees with the HLE. An import without an HLE is logged once and returns 0 (STATUS_SUCCESS).
 *
 * What is here: DbgPrint (and the C library formatters the kernel exports), memory (NtAllocateVirtualMemory /
 * NtFreeVirtualMemory / NtQueryVirtualMemory, MmAllocatePhysicalMemoryEx / MmFreePhysicalMemory, the pools),
 * threads (ExCreateThread, ExTerminateThread, suspend / resume, priorities and affinity accepted, TLS, delays,
 * yields), synchronisation (events, semaphores, mutants, timers, by handle and by object; waits on one or several;
 * critical sections; spin locks and IRQLs), time, the module and XEX header queries a CRT start-up makes, strings,
 * and the title ending (HalReturnToFirmware, XamLoaderTerminateTitle, KeBugCheck). */
#include <stdlib.h>
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define A9(n) ((uint32_t)x360_stack_arg(x, c, (n))) /* arguments 9 and up (n from 8) */

/* Argument n (0-based) of the caller when n >= 8: the caller's parameter area, 8 bytes per argument, the ninth's at
 * r1 + 0x50 (Xenia's reading of the console's convention). */
uint64_t x360_stack_arg(x360_machine *x, xe_cpu *c, int n)
{
    return x360_r64(x, (uint32_t)c->gpr[1] + 0x50 + 8 * (uint32_t)(n - 8));
}
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

typedef void (*hle_fn)(x360_machine *x, x360_thread *t, xe_cpu *c);

typedef struct {
    uint8_t lib;
    uint16_t ordinal;
    hle_fn fn;
} hle_entry;

typedef struct {
    uint8_t lib;
    uint16_t ordinal;
    const char *name;
} export_name;

static const export_name EXPORT_NAMES[] = {
#define X360_EXPORT(lib, ord, name, kind) {X360_LIB_##lib, ord, #name},
#define X360_LIB_xboxkrnl X360_LIB_XBOXKRNL
#define X360_LIB_xam X360_LIB_XAM
#include "x360_exports.inc"
#undef X360_EXPORT
};

const char *x360_export_name(int lib, unsigned ordinal)
{
    for (size_t k = 0; k < sizeof EXPORT_NAMES / sizeof EXPORT_NAMES[0]; k++)
        if (EXPORT_NAMES[k].lib == lib && EXPORT_NAMES[k].ordinal == ordinal)
            return EXPORT_NAMES[k].name;
    return NULL;
}

/* ---- guest strings and formatting ---- */

static size_t gstr(x360_machine *x, uint32_t a, char *out, size_t n)
{
    size_t k = 0;
    if (!a) {
        snprintf(out, n, "(null)");
        return strlen(out);
    }
    while (k + 1 < n) {
        char ch = (char)x360_r8(x, a + (uint32_t)k);
        if (!ch)
            break;
        out[k++] = ch;
    }
    out[k] = 0;
    return k;
}

static size_t gwstr(x360_machine *x, uint32_t a, char *out, size_t n)
{
    size_t k = 0;
    if (!a) {
        snprintf(out, n, "(null)");
        return strlen(out);
    }
    while (k + 1 < n) {
        uint16_t ch = x360_r16(x, a + 2 * (uint32_t)k);
        if (!ch)
            break;
        out[k++] = ch < 128 ? (char)ch : '?';
    }
    out[k] = 0;
    return k;
}

/* printf over guest arguments: from consecutive GPRs from r[first], then the stack (the ninth argument's 8-byte
 * slot at r1 + 0x50, the next ones after it: x360_stack_arg); variadic doubles come in GPRs too on the console. */
typedef struct {
    x360_machine *x;
    xe_cpu *c;
    int gpr, fpr;
    uint32_t va; /* a va_list in guest memory (vsprintf), 0 = registers */
} gargs;

static uint64_t next_int(gargs *g)
{
    if (g->va) {
        uint64_t v = x360_r64(g->x, g->va);
        g->va += 8;
        return v;
    }
    if (g->gpr <= 10)
        return g->c->gpr[g->gpr++];
    return x360_stack_arg(g->x, g->c, g->gpr++ - 3);
}

static double next_double(gargs *g)
{
    uint64_t v = next_int(g);
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static size_t gformat(x360_machine *x, gargs *g, uint32_t fmt, char *out, size_t n)
{
    size_t o = 0;
    char spec[32];
    char tmp[512];
#define PUT(s, len)                                                                                                    \
    do {                                                                                                               \
        size_t l_ = (len);                                                                                             \
        if (o + l_ >= n)                                                                                               \
            l_ = n - 1 - o;                                                                                            \
        memcpy(out + o, (s), l_);                                                                                      \
        o += l_;                                                                                                       \
    } while (0)
    for (uint32_t p = fmt; o + 1 < n; p++) {
        char ch = (char)x360_r8(x, p);
        if (!ch)
            break;
        if (ch != '%') {
            out[o++] = ch;
            continue;
        }
        size_t s = 0;
        spec[s++] = '%';
        int lng = 0, wide = 0, hh = 0;
        for (;;) {
            ch = (char)x360_r8(x, ++p);
            if (!ch)
                break;
            if (strchr("-+ #0123456789.", ch)) {
                if (s < sizeof spec - 4)
                    spec[s++] = ch;
                continue;
            }
            if (ch == '*') {
                int v = (int)(int32_t)next_int(g);
                s += (size_t)snprintf(spec + s, sizeof spec - s - 4, "%d", v);
                continue;
            }
            if (ch == 'l') {
                lng++;
                continue;
            }
            if (ch == 'h') {
                hh++;
                continue;
            }
            if (ch == 'w') {
                wide = 1;
                continue;
            }
            if (ch == 'I') { /* I64 / I32 */
                char a = (char)x360_r8(x, p + 1), b = (char)x360_r8(x, p + 2);
                if (a == '6' && b == '4')
                    lng = 2;
                p += 2;
                continue;
            }
            break;
        }
        if (!ch)
            break;
        size_t len = 0;
        switch (ch) {
        case 'd':
        case 'i': {
            uint64_t v = next_int(g);
            spec[s++] = 'l';
            spec[s++] = 'l';
            spec[s++] = 'd';
            spec[s] = 0;
            long long sv = lng >= 2 ? (long long)v : (long long)(int32_t)v;
            if (hh == 1)
                sv = (int16_t)sv;
            len = (size_t)snprintf(tmp, sizeof tmp, spec, sv);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v = next_int(g);
            spec[s++] = 'l';
            spec[s++] = 'l';
            spec[s++] = ch;
            spec[s] = 0;
            unsigned long long uv = lng >= 2 ? v : (uint32_t)v;
            if (hh == 1)
                uv = (uint16_t)uv;
            len = (size_t)snprintf(tmp, sizeof tmp, spec, uv);
            break;
        }
        case 'p':
            len = (size_t)snprintf(tmp, sizeof tmp, "%08X", (uint32_t)next_int(g));
            break;
        case 'c':
            tmp[0] = (char)next_int(g);
            len = 1;
            break;
        case 's':
        case 'S': {
            char str[256];
            uint32_t a = (uint32_t)next_int(g);
            if ((ch == 'S') != (wide || lng))
                gwstr(x, a, str, sizeof str);
            else
                gstr(x, a, str, sizeof str);
            spec[s++] = 's';
            spec[s] = 0;
            len = (size_t)snprintf(tmp, sizeof tmp, spec, str);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            spec[s++] = ch;
            spec[s] = 0;
            len = (size_t)snprintf(tmp, sizeof tmp, spec, next_double(g));
            break;
        case '%':
            tmp[0] = '%';
            len = 1;
            break;
        default:
            tmp[0] = '%';
            tmp[1] = ch;
            len = 2;
            break;
        }
        if (len > sizeof tmp - 1)
            len = sizeof tmp - 1;
        PUT(tmp, len);
    }
    out[o] = 0;
    return o;
#undef PUT
}

/* ---- debug output ---- */

static void k_DbgPrint(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    char buf[2048];
    gargs g = {x, c, 4, 1, 0};
    gformat(x, &g, A(0), buf, sizeof buf);
    fputs(buf, x->dbg ? x->dbg : stdout);
    fflush(x->dbg ? x->dbg : stdout);
    RET(0);
}

static void k_sprintf(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    char buf[2048];
    gargs g = {x, c, 5, 1, 0};
    size_t n = gformat(x, &g, A(1), buf, sizeof buf);
    memcpy(x->base + A(0), buf, n + 1);
    RET(n);
}

static void k__snprintf(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    char buf[2048];
    gargs g = {x, c, 6, 1, 0};
    size_t n = gformat(x, &g, A(2), buf, sizeof buf);
    uint32_t cap = A(1);
    if (cap) {
        size_t w = n < cap ? n + 1 : cap;
        memcpy(x->base + A(0), buf, w);
    }
    RET(n < cap ? n : (uint32_t)-1);
}

static void k_vsprintf(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    char buf[2048];
    gargs g = {x, c, 0, 1, A(2)};
    size_t n = gformat(x, &g, A(1), buf, sizeof buf);
    memcpy(x->base + A(0), buf, n + 1);
    RET(n);
}

static void k__vsnprintf(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    char buf[2048];
    gargs g = {x, c, 0, 1, A(3)};
    size_t n = gformat(x, &g, A(2), buf, sizeof buf);
    uint32_t cap = A(1);
    if (cap) {
        size_t w = n < cap ? n + 1 : cap;
        memcpy(x->base + A(0), buf, w);
    }
    RET(n < cap ? n : (uint32_t)-1);
}

static void k_DbgBreakPoint(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t, (void)c;
    x360_logf(x, "x360: DbgBreakPoint (ignored)\n");
}

static void k_KeBugCheckEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_fail(x, "KeBugCheck(%08X, %08X, %08X, %08X, %08X)", A(0), A(1), A(2), A(3), A(4));
}

static void k_RtlRaiseException(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t rec = A(0);
    x360_fail(x, "RtlRaiseException: code %08X at %08X", rec ? x360_r32(x, rec) : 0,
              rec ? x360_r32(x, rec + 12) : 0);
}

static void k_HalReturnToFirmware(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x->status = X360_EXITED;
    x->exit_code = A(0);
}

static void x_XamLoaderTerminateTitle(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t, (void)c;
    x->status = X360_EXITED;
    x->exit_code = 0;
}

/* ---- memory ---- */

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_LARGE_PAGES 0x20000000u
#define MEM_16MB_PAGES 0x80000000u

static void k_NtAllocateVirtualMemory(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t base_ptr = A(0), size_ptr = A(1), type = A(2);
    uint32_t want = x360_r32(x, base_ptr), size = x360_r32(x, size_ptr);
    x360_heap *h = type & MEM_LARGE_PAGES ? &x->heap_v64k : &x->heap_v4k;
    if (!size) {
        RET(X_STATUS_INVALID_PARAMETER);
        return;
    }
    uint32_t a;
    if (want) {
        x360_heap *hh = x360_heap_of(x, want);
        uint32_t aligned = want & ~(hh ? hh->page - 1 : 0xFFFu);
        if (hh && hh != &x->heap_phys && x360_heap_all_used(hh, aligned, size + (want - aligned))) {
            a = aligned; /* committing pages of an earlier reservation */
        } else if (hh && hh != &x->heap_phys && x360_heap_alloc_at(x, hh, aligned, size + (want - aligned)) == 0) {
            a = aligned;
        } else {
            RET(X_STATUS_CONFLICTING_ADDRESSES);
            return;
        }
        h = hh;
    } else {
        a = x360_heap_alloc(x, h, size, 0);
        if (!a) {
            RET(X_STATUS_NO_MEMORY);
            return;
        }
    }
    x360_w32(x, base_ptr, a);
    x360_w32(x, size_ptr, (size + h->page - 1) & ~(h->page - 1));
    RET(X_STATUS_SUCCESS);
}

static void k_NtFreeVirtualMemory(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t base_ptr = A(0), size_ptr = A(1), type = A(2);
    uint32_t a = x360_r32(x, base_ptr);
    x360_heap *h = x360_heap_of(x, a);
    if (!h || h == &x->heap_phys) {
        RET(X_STATUS_INVALID_PARAMETER);
        return;
    }
    if (type & MEM_DECOMMIT) {
        RET(X_STATUS_SUCCESS); /* pages stay */
        return;
    }
    uint32_t n = x360_heap_free(x, h, a);
    if (size_ptr)
        x360_w32(x, size_ptr, n);
    RET(n ? X_STATUS_SUCCESS : X_STATUS_INVALID_PARAMETER);
}

static void k_NtQueryVirtualMemory(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t a = A(0), info = A(1);
    x360_heap *h = x360_heap_of(x, a);
    uint32_t page = h ? h->page : 0x1000;
    uint32_t base = a & ~(page - 1);
    uint32_t size = h ? x360_heap_size_of(h, base) : 0;
    /* MEMORY_BASIC_INFORMATION: base, allocation base, allocation protect, region size, state, protect, type */
    x360_w32(x, info + 0, base);
    x360_w32(x, info + 4, base);
    x360_w32(x, info + 8, size ? 4 : 0);
    x360_w32(x, info + 12, size ? size : page);
    x360_w32(x, info + 16, size ? MEM_COMMIT : 0x10000u);
    x360_w32(x, info + 20, size ? 4 : 1);
    x360_w32(x, info + 24, size ? 0x20000u : 0);
    RET(X_STATUS_SUCCESS);
}

static void k_MmAllocatePhysicalMemoryEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t size = A(1), protect = A(2), align = A(5);
    uint32_t view = protect & MEM_16MB_PAGES ? 0xC0000000u : protect & MEM_LARGE_PAGES ? 0xA0000000u : 0xE0000000u;
    uint32_t page = view == 0xC0000000u ? 0x1000000u : view == 0xA0000000u ? 0x10000u : 0x1000u;
    if (align < page)
        align = page;
    uint32_t a = x360_heap_alloc(x, &x->heap_phys, (size + page - 1) & ~(page - 1), align);
    RET(a ? view + a : 0);
}

static void k_MmAllocatePhysicalMemory(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    c->gpr[8] = 0;
    c->gpr[5] = c->gpr[5]; /* (flags, size, protect) -> the Ex form with no bounds */
    c->gpr[6] = 0;
    c->gpr[7] = 0xFFFFFFFFu;
    k_MmAllocatePhysicalMemoryEx(x, t, c);
}

static void k_MmFreePhysicalMemory(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_heap_free(x, &x->heap_phys, A(1) & 0x1FFFFFFFu);
    RET(0);
}

static void k_MmGetPhysicalAddress(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    uint32_t a = A(0);
    RET(a >= 0xA0000000u ? a & 0x1FFFFFFFu : a);
}

static void k_MmQueryAllocationSize(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t a = A(0);
    x360_heap *h = x360_heap_of(x, a);
    RET(h ? x360_heap_size_of(h, h == &x->heap_phys ? a & 0x1FFFFFFFu : a) : 0);
}

static void k_MmQueryAddressProtect(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(4); /* PAGE_READWRITE */
}

static void k_ExAllocatePoolTypeWithTag(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t a = x360_heap_alloc(x, &x->heap_v4k, A(0), 0);
    RET(a);
}

static void k_ExAllocatePool(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    k_ExAllocatePoolTypeWithTag(x, t, c);
}

static void k_ExFreePool(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_heap_free(x, &x->heap_v4k, A(0));
    RET(0);
}

/* ---- threads ---- */

static void k_ExCreateThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t handle_ptr = A(0), stack = A(1), id_ptr = A(2), startup = A(3), start = A(4), ctx = A(5),
             flags = A(6);
    if (!stack)
        stack = x->stack_size;
    x360_thread *n = startup ? x360_thread_create(x, stack, startup, start, ctx, flags & 1)
                             : x360_thread_create(x, stack, start, ctx, 0, flags & 1);
    if (!n) {
        RET(X_STATUS_NO_MEMORY);
        return;
    }
    if (handle_ptr)
        x360_w32(x, handle_ptr, n->handle);
    if (id_ptr)
        x360_w32(x, id_ptr, n->id);
    if (x->verbose)
        x360_logf(x, "x360: thread %u created at %08X (context %08X)%s\n", n->id, startup ? startup : start, ctx,
                  flags & 1 ? ", suspended" : "");
    RET(X_STATUS_SUCCESS);
}

static void k_ExTerminateThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    x360_thread_exit(x, t, A(0));
    x->m.slice = -1;
}

static x360_thread *thread_of_handle(x360_machine *x, uint32_t h)
{
    return x360_thread_by_kthread(x, x360_handle_obj(x, h));
}

static void resume(x360_thread *n, xe_cpu *c)
{
    uint32_t prev = (uint32_t)n->suspend;
    if (n->suspend > 0 && --n->suspend == 0 && n->state == TH_SUSPENDED)
        n->state = TH_READY;
    RET(prev);
}

static void k_NtResumeThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_thread *n = thread_of_handle(x, A(0));
    uint32_t out = A(1);
    if (!n) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    resume(n, c);
    if (out)
        x360_w32(x, out, (uint32_t)c->gpr[3]);
    RET(X_STATUS_SUCCESS);
}

static void k_KeResumeThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_thread *n = x360_thread_by_kthread(x, A(0));
    if (!n) {
        RET(0);
        return;
    }
    resume(n, c);
}

static void k_NtSuspendThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    x360_thread *n = thread_of_handle(x, A(0));
    uint32_t out = A(1);
    if (!n) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (out)
        x360_w32(x, out, (uint32_t)n->suspend);
    n->suspend++;
    if (n->state == TH_READY)
        n->state = TH_SUSPENDED;
    if (n == t)
        x->m.slice = -1;
    RET(X_STATUS_SUCCESS);
}

static void k_KeSetAffinityThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (KTHREAD, affinity mask, previous*): threads are cooperative on one host thread here, but the hardware thread
     * a title asked for shows where it looks (the PCR's and KTHREAD's CPU byte: the lowest bit of the mask) */
    uint32_t prev_ptr = A(2), mask = A(1) & 0x3F;
    x360_thread *n = x360_thread_by_kthread(x, A(0));
    if (prev_ptr) /* the console stores the previous affinity there and returns a status (unlike NT) */
        x360_w32(x, prev_ptr, n ? 1u << x360_r8(x, n->kthread + 0xBF) : 0x3F);
    if (n && mask) {
        uint32_t cpu = (uint32_t)__builtin_ctz(mask);
        x360_w8(x, n->kthread + 0xBF, cpu);
        x360_w8(x, n->pcr + 0x10C, cpu);
    }
    RET(X_STATUS_SUCCESS);
}

static void k_KeSetBasePriorityThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}

static void k_KeGetCurrentProcessType(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(1); /* a title */
}

static void k_KeDelayExecutionThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    x360_wait(x, t, WAIT_DELAY, NULL, 0, 0, A(2));
    RET(X_STATUS_SUCCESS);
}

static void k_NtYieldExecution(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x->m.slice = -1;
    RET(0);
}

static void k_KeTlsAlloc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    for (int k = 0; k < X360_TLS_SLOTS; k++)
        if (!(x->tls_used >> k & 1)) {
            x->tls_used |= 1ull << k;
            for (int n = 0; n < x->nthreads; n++)
                if (x->threads[n].state != TH_FREE)
                    x360_w32(x, x->threads[n].tls_slots + 4 * (uint32_t)k, 0);
            RET(k);
            return;
        }
    RET(0xFFFFFFFFu);
}

static void k_KeTlsFree(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t k = A(0);
    if (k < X360_TLS_SLOTS)
        x->tls_used &= ~(1ull << k);
    RET(k < X360_TLS_SLOTS);
}

static void k_KeTlsGetValue(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t k = A(0);
    RET(k < X360_TLS_SLOTS ? x360_r32(x, t->tls_slots + 4 * k) : 0);
}

static void k_KeTlsSetValue(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t k = A(0);
    if (k < X360_TLS_SLOTS)
        x360_w32(x, t->tls_slots + 4 * k, A(1));
    RET(k < X360_TLS_SLOTS);
}

/* ---- dispatcher objects ---- */

static uint32_t new_object(x360_machine *x, uint8_t type, int32_t state, uint32_t size)
{
    uint32_t o = x360_kalloc(x, size);
    if (o) {
        x360_w8(x, o, type);
        x360_w8(x, o + 2, (uint8_t)(size / 4));
        x360_w32(x, o + 4, (uint32_t)state);
        x360_w32(x, o + 8, o + 8); /* empty wait list */
        x360_w32(x, o + 12, o + 8);
    }
    return o;
}

static uint32_t create_handle(x360_machine *x, xe_cpu *c, uint32_t handle_ptr, uint32_t obj)
{
    uint32_t h = obj ? x360_handle_new(x, obj) : 0;
    if (!h) {
        RET(X_STATUS_NO_MEMORY);
        return 0;
    }
    x360_w32(x, handle_ptr, h);
    RET(X_STATUS_SUCCESS);
    return h;
}

static void k_NtCreateEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle*, attributes, type: 0 notification / 1 synchronization, initial state) */
    create_handle(x, c, A(0), new_object(x, A(2) ? X_OBJ_EVENT_SYNC : X_OBJ_EVENT_NOTIFICATION, A(3) ? 1 : 0, 16));
}

static void k_KeInitializeEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = A(0);
    x360_w8(x, o, A(1) ? X_OBJ_EVENT_SYNC : X_OBJ_EVENT_NOTIFICATION);
    x360_w32(x, o + 4, A(2) ? 1 : 0);
    x360_w32(x, o + 8, o + 8);
    x360_w32(x, o + 12, o + 8);
}

static uint32_t set_state(x360_machine *x, uint32_t o, uint32_t v)
{
    uint32_t prev = x360_r32(x, o + 4);
    x360_w32(x, o + 4, v);
    return prev;
}

static void k_NtSetEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = x360_handle_obj(x, A(0));
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t prev = set_state(x, o, 1);
    if (A(1))
        x360_w32(x, A(1), prev);
    RET(X_STATUS_SUCCESS);
}

static void k_NtClearEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = x360_handle_obj(x, A(0));
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    set_state(x, o, 0);
    RET(X_STATUS_SUCCESS);
}

/* a pulse wakes the threads waiting now and leaves the event clear: done by waking them directly */
static void pulse(x360_machine *x, uint32_t o)
{
    int sync = x360_r8(x, o) == X_OBJ_EVENT_SYNC;
    for (int k = 0; k < x->nthreads; k++) {
        x360_thread *w = &x->threads[k];
        if (w->state != TH_WAIT || w->wait_kind != WAIT_OBJECTS || w->wait_all)
            continue;
        for (int j = 0; j < w->wait_count; j++)
            if (w->wait_obj[j] == o) {
                w->cpu.gpr[3] = (uint32_t)j;
                w->state = w->suspend ? TH_SUSPENDED : TH_READY;
                break;
            }
        if (sync && w->state != TH_WAIT)
            break;
    }
    x360_w32(x, o + 4, 0);
}

static void k_NtPulseEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = x360_handle_obj(x, A(0));
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t prev = x360_r32(x, o + 4);
    pulse(x, o);
    if (A(1))
        x360_w32(x, A(1), prev);
    RET(X_STATUS_SUCCESS);
}

static void k_KeSetEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(set_state(x, A(0), 1));
}

static void k_KeResetEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(set_state(x, A(0), 0));
}

static void k_KePulseEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t prev = x360_r32(x, A(0) + 4);
    pulse(x, A(0));
    RET(prev);
}

static void k_NtCreateSemaphore(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = new_object(x, X_OBJ_SEMAPHORE, (int32_t)A(2), 20);
    if (o)
        x360_w32(x, o + 16, A(3));
    create_handle(x, c, A(0), o);
}

static void k_KeInitializeSemaphore(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = A(0);
    x360_w8(x, o, X_OBJ_SEMAPHORE);
    x360_w32(x, o + 4, A(1));
    x360_w32(x, o + 8, o + 8);
    x360_w32(x, o + 12, o + 8);
    x360_w32(x, o + 16, A(2));
}

static uint32_t release_sem(x360_machine *x, uint32_t o, uint32_t n, int *over)
{
    uint32_t prev = x360_r32(x, o + 4), limit = x360_r32(x, o + 16);
    *over = limit && prev + n > limit;
    if (!*over)
        x360_w32(x, o + 4, prev + n);
    return prev;
}

static void k_NtReleaseSemaphore(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = x360_handle_obj(x, A(0));
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    int over;
    uint32_t prev = release_sem(x, o, A(1), &over);
    if (A(2))
        x360_w32(x, A(2), prev);
    RET(over ? X_STATUS_SEMAPHORE_LIMIT_EXCEEDED : X_STATUS_SUCCESS);
}

static void k_KeReleaseSemaphore(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int over;
    RET(release_sem(x, A(0), A(2), &over));
}

static void k_NtCreateMutant(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t o = new_object(x, X_OBJ_MUTANT, A(2) ? 0 : 1, 32);
    if (o && A(2))
        x360_w32(x, o + 0x18, t->kthread);
    create_handle(x, c, A(0), o);
}

static void k_NtReleaseMutant(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t o = x360_handle_obj(x, A(0));
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (x360_r32(x, o + 0x18) != t->kthread) {
        RET(X_STATUS_MUTANT_NOT_OWNED);
        return;
    }
    int32_t prev = (int32_t)x360_r32(x, o + 4);
    x360_w32(x, o + 4, (uint32_t)(prev + 1));
    if (prev + 1 > 0)
        x360_w32(x, o + 0x18, 0);
    RET(X_STATUS_SUCCESS); /* the second argument is not a previous-count pointer (Xenia: 0 in every title) */
}

static void k_NtCreateTimer(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    create_handle(x, c, A(0), new_object(x, A(2) ? X_OBJ_TIMER_SYNC : X_OBJ_TIMER_NOTIFICATION, 0, 40));
}

static void k_NtSetTimerEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle, due time*, apc routine, apc mode, apc context, resume, period ms, previous state*) */
    uint32_t o = x360_handle_obj(x, A(0));
    if (!o || !A(1)) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (A(2))
        x360_logf(x, "x360: NtSetTimerEx with an APC routine (%08X): not called\n", A(2));
    int64_t due = (int64_t)x360_r64(x, A(1));
    uint64_t now = x360_now(x);
    uint64_t at = due < 0 ? now + (uint64_t)(-due) : (uint64_t)due;
    x360_timer_set(x, o, at, A(6));
    RET(X_STATUS_SUCCESS);
}

static void k_NtCancelTimer(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = x360_handle_obj(x, A(0));
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (A(1))
        x360_w32(x, A(1), x360_r32(x, o + 4));
    x360_timer_cancel(x, o);
    RET(X_STATUS_SUCCESS);
}

static void k_NtWaitForSingleObjectEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t o = x360_handle_obj(x, A(0));
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    x360_wait(x, t, WAIT_OBJECTS, &o, 1, 0, A(3));
}

static void k_NtWaitForMultipleObjectsEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t n = A(0), arr = A(1), objs[X360_MAX_WAIT];
    if (!n || n > X360_MAX_WAIT) {
        RET(X_STATUS_INVALID_PARAMETER);
        return;
    }
    for (uint32_t k = 0; k < n; k++)
        if (!(objs[k] = x360_handle_obj(x, x360_r32(x, arr + 4 * k)))) {
            RET(X_STATUS_INVALID_HANDLE);
            return;
        }
    x360_wait(x, t, WAIT_OBJECTS, objs, (int)n, A(2) == 0, A(5));
}

static void k_KeWaitForSingleObject(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t o = A(0);
    x360_wait(x, t, WAIT_OBJECTS, &o, 1, 0, A(4));
}

static void k_KeWaitForMultipleObjects(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t n = A(0), arr = A(1), objs[X360_MAX_WAIT];
    if (!n || n > X360_MAX_WAIT) {
        RET(X_STATUS_INVALID_PARAMETER);
        return;
    }
    for (uint32_t k = 0; k < n; k++)
        objs[k] = x360_r32(x, arr + 4 * k);
    x360_wait(x, t, WAIT_OBJECTS, objs, (int)n, A(2) == 0, A(6));
}

static void k_NtClose(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_file_close(x, A(0));
    RET(x360_handle_close(x, A(0)) ? X_STATUS_INVALID_HANDLE : X_STATUS_SUCCESS);
}

static void k_NtDuplicateObject(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t src = A(0), out = A(1), options = A(2), o = x360_handle_obj(x, src);
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (!out) { /* no new handle: only closes the source (Xenia: titles do that) */
        if (options & 1)
            x360_handle_close(x, src);
        RET(X_STATUS_SUCCESS);
        return;
    }
    if (create_handle(x, c, out, o) && (options & 1)) /* DUPLICATE_CLOSE_SOURCE */
        x360_handle_close(x, src);
}

static void k_ObReferenceObjectByHandle(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = x360_handle_obj(x, A(0));
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (A(2))
        x360_w32(x, A(2), o);
    RET(X_STATUS_SUCCESS);
}

static void k_ObDereferenceObject(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}

/* ---- critical sections (X_RTL_CRITICAL_SECTION: header 0x10, lock count 0x10, recursion 0x14, owner 0x18) ---- */

static void k_RtlInitializeCriticalSection(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t cs = A(0);
    x360_w8(x, cs, X_OBJ_EVENT_SYNC);
    x360_w32(x, cs + 4, 0);
    x360_w32(x, cs + 8, cs + 8);
    x360_w32(x, cs + 12, cs + 8);
    x360_w32(x, cs + 0x10, 0xFFFFFFFFu);
    x360_w32(x, cs + 0x14, 0);
    x360_w32(x, cs + 0x18, 0);
    RET(0);
}

static void k_RtlEnterCriticalSection(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t cs = A(0), owner = x360_r32(x, cs + 0x18);
    x360_w32(x, cs + 0x10, x360_r32(x, cs + 0x10) + 1);
    if (owner == t->kthread) {
        x360_w32(x, cs + 0x14, x360_r32(x, cs + 0x14) + 1);
    } else if (!owner) {
        x360_w32(x, cs + 0x18, t->kthread);
        x360_w32(x, cs + 0x14, 1);
    } else {
        x360_wait(x, t, WAIT_CRITSEC, &cs, 1, 0, 0);
        return;
    }
    RET(0);
}

static void k_RtlTryEnterCriticalSection(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t cs = A(0), owner = x360_r32(x, cs + 0x18);
    if (owner && owner != t->kthread) {
        RET(0);
        return;
    }
    x360_w32(x, cs + 0x10, x360_r32(x, cs + 0x10) + 1);
    x360_w32(x, cs + 0x18, t->kthread);
    x360_w32(x, cs + 0x14, x360_r32(x, cs + 0x14) + 1);
    RET(1);
}

static void k_RtlLeaveCriticalSection(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t cs = A(0);
    if (x360_r32(x, cs + 0x18) != t->kthread) {
        RET(0);
        return;
    }
    x360_w32(x, cs + 0x10, x360_r32(x, cs + 0x10) - 1);
    uint32_t r = x360_r32(x, cs + 0x14) - 1;
    x360_w32(x, cs + 0x14, r);
    if (!r)
        x360_w32(x, cs + 0x18, 0); /* a waiter takes it at its next check */
    RET(0);
}

static void k_KfAcquireSpinLock(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    x360_w32(x, A(0), t->kthread);
    RET(0);
}

static void k_KfReleaseSpinLock(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_w32(x, A(0), 0);
    RET(0);
}

static void k_nop0(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}

/* ---- time ---- */


static void k_KeQueryPerformanceFrequency(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    c->gpr[3] = 50000000u;
}

static void k_KeQuerySystemTime(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0))
        x360_w64(x, A(0), X360_EPOCH + x360_now(x));
    RET(0);
}

static void k_KeQueryInterruptTime(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    c->gpr[3] = x360_now(x);
}

/* ---- module, XEX header, configuration ---- */

static void k_XexGetModuleHandle(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t name = A(0), out = A(1);
    char s[64];
    gstr(x, name, s, sizeof s);
    if (name && strncmp(s, "default", 7) && strcmp(s, "xboxkrnl.exe") && strcmp(s, "xam.xex")) {
        if (out)
            x360_w32(x, out, 0);
        RET(X_STATUS_NOT_FOUND);
        return;
    }
    if (out)
        x360_w32(x, out, x->module);
    RET(X_STATUS_SUCCESS);
}

static void k_XexGetProcedureAddress(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(2))
        x360_w32(x, A(2), 0);
    RET(X_STATUS_NOT_FOUND);
}

static void k_RtlImageXexHeaderField(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t h = A(0), key = A(1);
    uint32_t count = x360_r32(x, h + 20);
    for (uint32_t k = 0; k < count && k < 256; k++) {
        uint32_t kk = x360_r32(x, h + 0x18 + 8 * k), v = x360_r32(x, h + 0x1C + 8 * k);
        if (kk == key) {
            RET((key & 0xFF) <= 1 ? v : h + v);
            return;
        }
    }
    RET(0);
}

static void k_ExGetXConfigSetting(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t buf = A(2), size = A(3), req = A(4);
    if (buf && size <= 0x1000)
        memset(x->base + buf, 0, size);
    if (req)
        x360_w16(x, req, (uint16_t)size);
    RET(X_STATUS_SUCCESS);
}

static void k_RtlInitAnsiString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t dst = A(0), src = A(1);
    char s[1024];
    size_t n = src ? gstr(x, src, s, sizeof s) : 0;
    x360_w16(x, dst, (uint16_t)n);
    x360_w16(x, dst + 2, (uint16_t)(src ? n + 1 : 0));
    x360_w32(x, dst + 4, src);
    RET(0);
}

static void k_RtlInitUnicodeString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t dst = A(0), src = A(1);
    uint32_t n = 0;
    while (src && n < 0x7FFF && x360_r16(x, src + 2 * n))
        n++;
    x360_w16(x, dst, (uint16_t)(2 * n));
    x360_w16(x, dst + 2, (uint16_t)(src ? 2 * n + 2 : 0));
    x360_w32(x, dst + 4, src);
    RET(0);
}

static void k_RtlNtStatusToDosError(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    uint32_t s = A(0);
    RET(s == 0 ? 0 : s == X_STATUS_TIMEOUT ? 1460 : s == X_STATUS_NO_MEMORY ? 8 : s == X_STATUS_INVALID_HANDLE ? 6
                                                                                                             : 317);
}

static void k_RtlFillMemoryUlong(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    for (uint32_t k = 0; k + 4 <= A(1); k += 4)
        x360_w32(x, A(0) + k, A(2));
    RET(0);
}

static void k_RtlCompareMemoryUlong(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t k = 0;
    while (k + 4 <= A(1) && x360_r32(x, A(0) + k) == A(2))
        k += 4;
    RET(k);
}

static void k_XexCheckExecutablePrivilege(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}

/* ---- video mode, sections, misc ---- */

static void k_VdQueryVideoMode(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* X_VIDEO_MODE (Xenia's xbox.h): 1280 x 720 progressive, wide, HD, 60 Hz, NTSC */
    uint32_t m = A(0);
    float hz = 60.0f;
    uint32_t hzbits;
    memcpy(&hzbits, &hz, 4);
    memset(x->base + m, 0, 48);
    x360_w32(x, m, 1280);
    x360_w32(x, m + 4, 720);
    x360_w32(x, m + 12, 1);
    x360_w32(x, m + 16, 1);
    x360_w32(x, m + 20, hzbits);
    x360_w32(x, m + 24, 1);
    x360_w32(x, m + 28, 0x4A);
    x360_w32(x, m + 32, 0x01);
}

static void k_VdQueryVideoFlags(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(1 | 2); /* widescreen, at least 1024 wide */
}

static void k_XexGetModuleSection(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* the PE section table of the image (little-endian, as in any PE) */
    char want[9] = "";
    for (int k = 0; k < 8; k++)
        if (!(want[k] = (char)x360_r8(x, A(1) + (uint32_t)k)))
            break;
    const uint8_t *pe = x->base + x->image_base;
    uint32_t lfanew = pe[0x3C] | pe[0x3D] << 8 | pe[0x3E] << 16 | (uint32_t)pe[0x3F] << 24;
    if (pe[0] != 'M' || lfanew > 0x800) {
        RET(X_STATUS_NOT_FOUND);
        return;
    }
    const uint8_t *nt = pe + lfanew;
    unsigned nsec = nt[6] | nt[7] << 8, opt = nt[20] | nt[21] << 8;
    const uint8_t *s = nt + 24 + opt;
    for (unsigned k = 0; k < nsec && k < 96; k++, s += 40) {
        if (strncmp((const char *)s, want, 8))
            continue;
        uint32_t vsize = s[8] | s[9] << 8 | s[10] << 16 | (uint32_t)s[11] << 24;
        uint32_t va = s[12] | s[13] << 8 | s[14] << 16 | (uint32_t)s[15] << 24;
        if (A(2))
            x360_w32(x, A(2), x->image_base + va);
        if (A(3))
            x360_w32(x, A(3), vsize);
        RET(X_STATUS_SUCCESS);
        return;
    }
    RET(X_STATUS_NOT_FOUND);
}

static void k_XAudioGetSpeakerConfig(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0))
        x360_w32(x, A(0), 0x00010001u);
    RET(0);
}

static void k_KeTryToAcquireSpinLockAtRaisedIrql(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    if (x360_r32(x, A(0))) {
        RET(0);
        return;
    }
    x360_w32(x, A(0), t->kthread);
    RET(1);
}

static void k_NtSignalAndWaitForSingleObjectEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (signal handle, wait handle, alertable, unused, timeout*) */
    uint32_t so = x360_handle_obj(x, A(0)), wo = x360_handle_obj(x, A(1));
    if (!so || !wo) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    uint8_t type = x360_r8(x, so);
    if (type == X_OBJ_SEMAPHORE)
        x360_w32(x, so + 4, x360_r32(x, so + 4) + 1);
    else if (type == X_OBJ_MUTANT && x360_r32(x, so + 0x18) == t->kthread) {
        int32_t v = (int32_t)x360_r32(x, so + 4) + 1;
        x360_w32(x, so + 4, (uint32_t)v);
        if (v > 0)
            x360_w32(x, so + 0x18, 0);
    } else
        x360_w32(x, so + 4, 1);
    x360_wait(x, t, WAIT_OBJECTS, &wo, 1, 0, A(4));
}

static void k_ObLookupThreadByThreadId(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    for (int k = 0; k < x->nthreads; k++)
        if (x->threads[k].state != TH_FREE && x->threads[k].id == A(0)) {
            if (A(1))
                x360_w32(x, A(1), x->threads[k].kthread);
            RET(X_STATUS_SUCCESS);
            return;
        }
    RET(X_STATUS_INVALID_PARAMETER);
}

static void k_NtQueryEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = x360_handle_obj(x, A(0));
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (A(1)) { /* EVENT_BASIC_INFORMATION: type, state */
        x360_w32(x, A(1), x360_r8(x, o));
        x360_w32(x, A(1) + 4, x360_r32(x, o + 4));
    }
    RET(X_STATUS_SUCCESS);
}

static void k_RtlUnwind(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_fail(x, "RtlUnwind(%08X, %08X): structured exception unwinding is not emulated yet", A(0), A(1));
}

/* FILETIME (100 ns since 1601-01-01) <-> TIME_FIELDS (year, month, day, hour, minute, second, ms, weekday) */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468; /* days since 1970-01-01 */
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

#define DAYS_1601_1970 134774

static void k_RtlTimeToTimeFields(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint64_t ft = x360_r64(x, A(0));
    uint64_t ms = ft / 10000;
    int64_t days = (int64_t)(ms / 86400000) - DAYS_1601_1970;
    uint32_t rem = (uint32_t)(ms % 86400000);
    int64_t y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    uint32_t f = A(1);
    x360_w16(x, f, (uint32_t)y);
    x360_w16(x, f + 2, m);
    x360_w16(x, f + 4, d);
    x360_w16(x, f + 6, rem / 3600000);
    x360_w16(x, f + 8, rem / 60000 % 60);
    x360_w16(x, f + 10, rem / 1000 % 60);
    x360_w16(x, f + 12, rem % 1000);
    x360_w16(x, f + 14, (uint32_t)((days % 7 + 11) % 7)); /* 1970-01-01 was a Thursday (4) */
}

static void k_RtlTimeFieldsToTime(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t f = A(0);
    unsigned m = x360_r16(x, f + 2), d = x360_r16(x, f + 4);
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        RET(0);
        return;
    }
    int64_t days = days_from_civil(x360_r16(x, f), m, d) + DAYS_1601_1970;
    uint64_t ms = (uint64_t)days * 86400000 + x360_r16(x, f + 6) * 3600000ull + x360_r16(x, f + 8) * 60000ull +
                  x360_r16(x, f + 10) * 1000ull + x360_r16(x, f + 12);
    x360_w64(x, A(1), ms * 10000);
    RET(1);
}

/* ---- the table ---- */

#define K(name) {X360_LIB_XBOXKRNL, X360_xboxkrnl_##name, k_##name}
#define KA(name, fn) {X360_LIB_XBOXKRNL, X360_xboxkrnl_##name, fn}
#define XM(name, fn) {X360_LIB_XAM, X360_xam_##name, fn}

static const hle_entry HLE[] = {
    K(DbgPrint), K(DbgBreakPoint), KA(DbgBreakPointWithStatus, k_DbgBreakPoint), K(sprintf), K(_snprintf),
    K(vsprintf), K(_vsnprintf), KA(KeBugCheck, k_KeBugCheckEx), K(KeBugCheckEx), K(RtlRaiseException),
    K(HalReturnToFirmware),
    K(NtAllocateVirtualMemory), K(NtFreeVirtualMemory), K(NtQueryVirtualMemory), K(MmAllocatePhysicalMemoryEx),
    K(MmAllocatePhysicalMemory), K(MmFreePhysicalMemory), K(MmGetPhysicalAddress), K(MmQueryAllocationSize),
    K(MmQueryAddressProtect), K(ExAllocatePool), KA(ExAllocatePoolWithTag, k_ExAllocatePool),
    K(ExAllocatePoolTypeWithTag), K(ExFreePool),
    K(ExCreateThread), K(ExTerminateThread), K(NtResumeThread), K(KeResumeThread), K(NtSuspendThread),
    K(KeSetAffinityThread), K(KeSetBasePriorityThread), KA(KeSetPriorityThread, k_KeSetBasePriorityThread),
    KA(KeQueryBasePriorityThread, k_nop0), K(KeGetCurrentProcessType), K(KeDelayExecutionThread),
    K(NtYieldExecution), K(KeTlsAlloc), K(KeTlsFree), K(KeTlsGetValue), K(KeTlsSetValue),
    K(NtCreateEvent), K(KeInitializeEvent), K(NtSetEvent), K(NtClearEvent), K(NtPulseEvent), K(KeSetEvent),
    K(KeResetEvent), K(KePulseEvent), K(NtCreateSemaphore), K(KeInitializeSemaphore), K(NtReleaseSemaphore),
    K(KeReleaseSemaphore), K(NtCreateMutant), K(NtReleaseMutant), K(NtCreateTimer), K(NtSetTimerEx),
    K(NtCancelTimer), K(NtWaitForSingleObjectEx), K(NtWaitForMultipleObjectsEx), K(KeWaitForSingleObject),
    K(KeWaitForMultipleObjects), K(NtClose), K(NtDuplicateObject), K(ObReferenceObjectByHandle),
    K(ObDereferenceObject),
    K(RtlInitializeCriticalSection), KA(RtlInitializeCriticalSectionAndSpinCount, k_RtlInitializeCriticalSection),
    K(RtlEnterCriticalSection), K(RtlTryEnterCriticalSection), K(RtlLeaveCriticalSection),
    K(KfAcquireSpinLock), K(KfReleaseSpinLock), KA(KeAcquireSpinLockAtRaisedIrql, k_KfAcquireSpinLock),
    KA(KeReleaseSpinLockFromRaisedIrql, k_KfReleaseSpinLock), KA(KeRaiseIrqlToDpcLevel, k_nop0),
    KA(KfLowerIrql, k_nop0), KA(KeEnterCriticalRegion, k_nop0), KA(KeLeaveCriticalRegion, k_nop0),
    K(KeQueryPerformanceFrequency), K(KeQuerySystemTime), K(KeQueryInterruptTime),
    K(XexGetModuleHandle), K(XexGetProcedureAddress), K(RtlImageXexHeaderField), K(ExGetXConfigSetting),
    K(XexCheckExecutablePrivilege), K(RtlInitAnsiString), K(RtlInitUnicodeString), K(RtlNtStatusToDosError),
    K(RtlFillMemoryUlong), K(RtlCompareMemoryUlong),
    XM(XamLoaderTerminateTitle, x_XamLoaderTerminateTitle),
#define X360_FILE_ENTRY(name) {X360_LIB_XBOXKRNL, X360_xboxkrnl_##name, x360_k_##name},
    X360_FILE_CALLS(X360_FILE_ENTRY)
#define X360_GPU_ALIAS(name, fn) {X360_LIB_XBOXKRNL, X360_xboxkrnl_##name, x360_k_##fn},
    X360_GPU_CALLS(X360_FILE_ENTRY)
    X360_CRYPT_CALLS(X360_FILE_ENTRY)
    X360_GPU_ALIASES(X360_GPU_ALIAS)
#define X360_XAM_ENTRY(name) {X360_LIB_XAM, X360_xam_##name, x360_x_##name},
    X360_XAM_CALLS(X360_XAM_ENTRY)
#define X360_XAM_ALIAS(name, fn) {X360_LIB_XAM, X360_xam_##name, x360_x_##fn},
    X360_XAM_ALIASES(X360_XAM_ALIAS)
    K(VdQueryVideoMode), K(VdQueryVideoFlags), K(XexGetModuleSection), KA(KeLockL2, k_nop0),
    KA(KeUnlockL2, k_nop0), K(XAudioGetSpeakerConfig), K(KeTryToAcquireSpinLockAtRaisedIrql),
    K(NtSignalAndWaitForSingleObjectEx), K(ObLookupThreadByThreadId), K(RtlTimeToTimeFields),
    K(RtlTimeFieldsToTime), K(NtQueryEvent), KA(KeSetDisableBoostThread, k_nop0), K(RtlUnwind),
};

void x360_hle_call(x360_machine *x, const x360_thunk *th)
{
    x360_thread *t = x360_cur(x);
    xe_cpu *c = &t->cpu;
    for (size_t k = 0; k < sizeof HLE / sizeof HLE[0]; k++)
        if (HLE[k].lib == th->lib && HLE[k].ordinal == th->ordinal) {
            if (x->verbose)
                x360_logf(x, "x360: [t%u] %s(%08X, %08X, %08X, %08X)\n", t->id,
                          x360_export_name(th->lib, th->ordinal) ?: "?", A(0), A(1), A(2), A(3));
            HLE[k].fn(x, t, c);
            return;
        }
    if (x->strict_imports) {
        x360_fail(x, "%s!%s (ordinal %u) has no HLE (stopping: strict imports)", th->lib ? "xam" : "xboxkrnl",
                  x360_export_name(th->lib, th->ordinal) ?: "?", th->ordinal);
        return;
    }
    if (th->lib < 2 && !x->unimpl_seen[th->lib][th->ordinal & 4095]) {
        x->unimpl_seen[th->lib][th->ordinal & 4095] = 1;
        x->st.unimplemented++;
        x360_logf(x, "x360: %s!%s (ordinal %u) has no HLE: returns 0\n", th->lib ? "xam" : "xboxkrnl",
                  x360_export_name(th->lib, th->ordinal) ?: "?", th->ordinal);
    }
    RET(0);
}

/* the variable exports this runtime fills in, before the loader binds the imports */
static uint32_t var(x360_machine *x, int lib, unsigned ordinal, uint32_t size)
{
    uint32_t a = x360_kalloc(x, size);
    x->vars[lib][ordinal] = a;
    return a;
}

int x360_kernel_init(x360_machine *x)
{
    uint32_t a = var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_XboxHardwareInfo, 16);
    x360_w32(x, a, 0x00000020u); /* flags (Xenia: 0x20, storage initialised) */
    x360_w8(x, a + 4, 6);        /* hardware threads */
    a = var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_XboxKrnlVersion, 8);
    x360_w16(x, a, 2);
    x360_w16(x, a + 2, 0);
    x360_w16(x, a + 4, 17559);
    x360_w16(x, a + 6, 0);
    x->timestamp_bundle = var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_KeTimeStampBundle, 24);
    /* the executable module: XexExecutableModuleHandle points at a pointer to an LDR_DATA_TABLE_ENTRY-like block
     * whose +0x58 is the XEX header (filled once the XEX is loaded, x360_create) */
    x->module = x360_kalloc(x, 0x100);
    a = var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_XexExecutableModuleHandle, 16);
    x360_w32(x, a, x->module);
    a = var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_ExLoadedImageName, 64);
    memcpy(x->base + a, "\\Device\\Cdrom0\\default.xex", 27);
    a = var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_ExLoadedCommandLine, 64);
    memcpy(x->base + a, "\"default.xex\"", 14);
    var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_KeDebugMonitorData, 64);
    var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_ExThreadObjectType, 16);
    var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_ExEventObjectType, 16);
    var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_ExSemaphoreObjectType, 16);
    return 0;
}
