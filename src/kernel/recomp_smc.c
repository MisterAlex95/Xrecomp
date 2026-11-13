/* Code written or run at run time (XBA-118): recomp_smc.h has the rules. */
#include "recomp_smc.h"
#include "recomp_discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;
extern size_t g_xbox_total_ram;
extern int xbox_ReserveHolds(uint32_t va, uint32_t bytes);
extern int xbox_PosixMmioFaultHandlerInstalled(void);
extern uint32_t xbox_SmcGuestEsp(void);         /* xbox_memory_layout.c: this thread's guest esp */

#define SMC_SPAN      (64u * 1024u * 1024u)     /* sections load below XBOX_TOTAL_RAM */
#define SMC_MAX_PAGES (SMC_SPAN / 4096u)
#define SMC_SECTIONS  96

#define SEC_WRITABLE   0x1u
#define SEC_EXECUTABLE 0x4u

enum { PG_NONE = 0, PG_PROTECTED, PG_OPENING, PG_OPEN, PG_FAILED };

static struct {
    uint32_t va, end, init_end, flags;
    char name[12];
} s_sec[SMC_SECTIONS];
static int s_nsec;

static volatile unsigned char s_page[SMC_MAX_PAGES];
static uint32_t s_hp = 4096;                    /* host page size */
static uint32_t s_lo_page, s_hi_page;           /* pages that may be protected: [lo, hi) */
static volatile unsigned s_protected, s_count;
static volatile int s_armed;

static unsigned load_u(volatile unsigned *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

void xbox_SmcNoteSection(uint32_t va, uint32_t vsize, uint32_t raw, uint32_t flags, const char *name)
{
    if (s_nsec >= SMC_SECTIONS || !vsize || va >= SMC_SPAN || vsize > SMC_SPAN - va)
        return;
    s_sec[s_nsec].va = va;
    s_sec[s_nsec].end = va + vsize;
    s_sec[s_nsec].init_end = va + (raw < vsize ? raw : vsize);
    s_sec[s_nsec].flags = flags;
    snprintf(s_sec[s_nsec].name, sizeof s_sec[0].name, "%.8s", name ? name : "?");
    s_nsec++;
}

static const char *section_of(uint32_t va)
{
    for (int i = 0; i < s_nsec; i++)
        if (va >= s_sec[i].va && va < s_sec[i].end)
            return s_sec[i].name;
    return "?";
}

/* ── Writing into code ───────────────────────────────────────────────────── */

#ifndef _WIN32
static int set_prot(uint32_t first_page, uint32_t n, int prot)
{
    uint8_t *base = (uint8_t *)xbox_GetMemoryOffset();
    return mprotect(base + (size_t)first_page * s_hp, (size_t)n * s_hp, prot);
}

/* stderr from the fault handler: one write(2), no stdio lock. */
static void say(const char *line)
{
    size_t n = strlen(line);
    while (n) {
        ssize_t w = write(2, line, n);
        if (w <= 0)
            return;
        line += w;
        n -= (size_t)w;
    }
}
#endif

void xbox_SmcReset(void)
{
#ifndef _WIN32
    /* Open every page still protected: the next image is loaded over them. */
    for (uint32_t p = s_lo_page; p < s_hi_page && p < SMC_MAX_PAGES; p++) {
        if (s_page[p] == PG_PROTECTED || s_page[p] == PG_OPENING)
            (void)set_prot(p, 1, PROT_READ | PROT_WRITE);
        s_page[p] = PG_NONE;
    }
#endif
    s_armed = 0;
    s_nsec = 0;
    s_lo_page = s_hi_page = 0;
    __atomic_store_n(&s_protected, 0, __ATOMIC_RELAXED);
}

/* Host page p may be protected: inside the initialised part of an executable
 * (and, unless wx, non-writable) section, overlapping no other section that
 * is not such code. */
static int page_is_code(uint32_t p, int wx)
{
    uint32_t lo = p * s_hp, hi = lo + s_hp;
    int covered = 0;
    for (int i = 0; i < s_nsec; i++) {
        int code = (s_sec[i].flags & SEC_EXECUTABLE) && (wx || !(s_sec[i].flags & SEC_WRITABLE));
        if (s_sec[i].end <= lo || s_sec[i].va >= hi)
            continue;
        if (!code)
            return 0;
        if (s_sec[i].va <= lo && hi <= s_sec[i].init_end)
            covered = 1;
        else if (hi > s_sec[i].init_end)
            return 0;               /* its BSS tail is in this page */
    }
    return covered;
}

int xbox_SmcArm(void)
{
#ifdef _WIN32
    return 0;
#else
    const char *on = getenv("RECOMP_SMC"), *wxs = getenv("RECOMP_SMC_WX");
    int wx = wxs && *wxs && *wxs != '0';
    long hp = sysconf(_SC_PAGESIZE);
    unsigned n = 0, runs = 0, wx_left = 0;
    uint32_t p, run = 0;

    if (s_armed)
        return (int)load_u(&s_protected);
    if (on && (!*on || *on == '0')) {
        fprintf(stderr, "[SMC] code pages not write-protected (RECOMP_SMC=0)\n");
        return 0;
    }
    if (!xbox_PosixMmioFaultHandlerInstalled()) {
        fprintf(stderr, "[SMC] no fault handler installed: code pages not write-protected\n");
        return 0;
    }
    if (hp < 4096 || (hp & (hp - 1)) || !g_xbox_code_hi)
        return 0;
    s_hp = (uint32_t)hp;
    for (int i = 0; i < s_nsec; i++)
        wx_left += !wx && (s_sec[i].flags & (SEC_EXECUTABLE | SEC_WRITABLE)) == (SEC_EXECUTABLE | SEC_WRITABLE);
    s_lo_page = g_xbox_code_lo / s_hp;
    s_hi_page = (g_xbox_code_hi + s_hp - 1) / s_hp;
    if (s_hi_page > SMC_SPAN / s_hp)
        s_hi_page = SMC_SPAN / s_hp;
    for (p = s_lo_page; p <= s_hi_page; p++) {
        int code = p < s_hi_page && page_is_code(p, wx);
        if (code) {
            if (!run)
                run = p + 1;            /* 1-based: 0 is "no run" */
            s_page[p] = PG_PROTECTED;
            n++;
            continue;
        }
        if (run) {
            if (set_prot(run - 1, p - (run - 1), PROT_READ) != 0) {
                for (uint32_t q = run - 1; q < p; q++)
                    s_page[q] = PG_NONE;
                n -= p - (run - 1);
            } else {
                runs++;
            }
            run = 0;
        }
    }
    __atomic_store_n(&s_protected, n, __ATOMIC_RELAXED);
    __atomic_store_n(&s_armed, 1, __ATOMIC_RELEASE);
    fprintf(stderr, "[SMC] %u code page(s) of %u bytes write-protected in %u run(s)%s (RECOMP_SMC=0: off)\n",
            n, s_hp, runs, wx_left ? ", writable+executable sections left writable (RECOMP_SMC_WX=1)" : "");
    return (int)n;
#endif
}

#ifndef _WIN32
static void opened(uint32_t p, uint32_t at, const char *by)
{
    char line[200];
    uint32_t esp = xbox_SmcGuestEsp(), ret = 0;
    uint32_t page = p * s_hp;

    if (esp >= 0x10000u && esp < g_xbox_total_ram - 4u)
        ret = *(volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + esp);
    __atomic_fetch_add(&s_count, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&s_protected, 1u, __ATOMIC_RELAXED);
    snprintf(line, sizeof line, "[SMC] code written at run time: 0x%08X (page 0x%08X, %s) by %s; "
             "the lifted code there runs the old bytes\n", at, page, section_of(at), by);
    say(line);
    recomp_discovery_smc_write(page, at, section_of(at), by, ret);
}
#endif

int xbox_SmcFault(uintptr_t fault)
{
#ifdef _WIN32
    (void)fault;
    return 0;
#else
    uintptr_t base;
    uint64_t rel;
    uint32_t p;
    unsigned char st;

    if (!__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE))
        return 0;
    base = (uintptr_t)xbox_GetMemoryOffset();
    rel = (uint64_t)fault - (uint64_t)base;
    if (fault < base || rel >= SMC_SPAN)
        return 0;
    p = (uint32_t)(rel / s_hp);
    if (p < s_lo_page || p >= s_hi_page)
        return 0;
    st = __atomic_load_n(&s_page[p], __ATOMIC_ACQUIRE);
    if (st == PG_PROTECTED) {
        unsigned char want = PG_PROTECTED;
        if (__atomic_compare_exchange_n(&s_page[p], &want, PG_OPENING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (set_prot(p, 1, PROT_READ | PROT_WRITE) != 0) {
                __atomic_store_n(&s_page[p], PG_FAILED, __ATOMIC_RELEASE);
                return 0;               /* a real crash, then */
            }
            __atomic_store_n(&s_page[p], PG_OPEN, __ATOMIC_RELEASE);
            opened(p, (uint32_t)rel, "a store");
            return 1;
        }
        st = __atomic_load_n(&s_page[p], __ATOMIC_ACQUIRE);
    }
    /* Another thread is opening it: retry until it is writable. */
    return st == PG_OPENING;
#endif
}

void xbox_SmcHostWrite(const void *ptr, size_t len)
{
#ifndef _WIN32
    uintptr_t base, a = (uintptr_t)ptr;
    uint64_t rel;
    uint32_t p, last;

    if (!len || !__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE))
        return;
    base = (uintptr_t)xbox_GetMemoryOffset();
    rel = (uint64_t)a - (uint64_t)base;
    if (a < base || rel >= SMC_SPAN)
        return;
    if (len > SMC_SPAN - rel)
        len = (size_t)(SMC_SPAN - rel);
    last = (uint32_t)((rel + len - 1) / s_hp);
    for (p = (uint32_t)(rel / s_hp); p <= last; p++) {
        unsigned char want = PG_PROTECTED;
        if (p < s_lo_page || p >= s_hi_page)
            continue;
        if (__atomic_compare_exchange_n(&s_page[p], &want, PG_OPENING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (set_prot(p, 1, PROT_READ | PROT_WRITE) != 0) {
                __atomic_store_n(&s_page[p], PG_FAILED, __ATOMIC_RELEASE);
                continue;
            }
            __atomic_store_n(&s_page[p], PG_OPEN, __ATOMIC_RELEASE);
            opened(p, p * s_hp > rel ? p * s_hp : (uint32_t)rel, "a system call (read into guest memory)");
        }
    }
#else
    (void)ptr; (void)len;
#endif
}

/* ── Running code outside the image ──────────────────────────────────────── */

/* In an executable section (the envelope g_xbox_code_lo..hi also spans the
 * data sections between them, which the call macros send to the lookup). */
static int in_code(uint32_t t)
{
    if (!s_nsec)
        return g_xbox_code_hi && t >= g_xbox_code_lo && t < g_xbox_code_hi;
    for (int i = 0; i < s_nsec; i++)
        if ((s_sec[i].flags & SEC_EXECUTABLE) && t >= s_sec[i].va && t < s_sec[i].end)
            return 1;
    return 0;
}

/* Where a target lies, or NULL when it is no mapped guest memory we can name
 * (a wild pointer). esp: the calling thread's guest stack pointer. */
static const char *where_of(uint32_t t, uint32_t esp, int *readable)
{
    *readable = 0;
    if (t < 0x10000u || t >= 0xFE000000u)
        return NULL;                                    /* null page, kernel thunks */
    if (in_code(t))
        return NULL;                                    /* the image's code */
    if (t < g_xbox_total_ram - 16u) {
        *readable = 1;
        if (esp && t + 0x10000u >= esp && t < esp + 0x100000u)
            return "stack";
        if (t >= g_xbox_image_lo && t < g_xbox_image_hi)
            return "image_data";
        return "heap";
    }
    if (t >= 0x80000000u && t < 0x80100000u) {
        *readable = 1;
        return "kernel";
    }
    if (t >= 0x80000000u && t < 0x80000000u + (uint32_t)g_xbox_total_ram - 16u) {
        *readable = 1;
        return "contiguous";
    }
    if (xbox_ReserveHolds(t, 16)) {
        *readable = 1;
        return "heap";
    }
    return NULL;
}

int xbox_SmcExecOutside(uint32_t target, uint32_t ret)
{
    static uint32_t seen[64];
    static unsigned nseen;
#ifndef _WIN32
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#endif
    char hex[17] = "";
    int readable, fresh = 1;
    const char *where = where_of(target, xbox_SmcGuestEsp(), &readable);

    if (!where)
        return 0;
    if (readable) {
        const uint8_t *b = (const uint8_t *)xbox_GetMemoryOffset() + target;
        for (int i = 0; i < 8; i++)
            snprintf(hex + 2 * i, 3, "%02X", b[i]);
    }
#ifndef _WIN32
    pthread_mutex_lock(&lock);
#endif
    for (unsigned i = 0; i < nseen && i < 64; i++)
        if (seen[i] == target)
            fresh = 0;
    if (fresh) {
        if (nseen < 64)
            seen[nseen] = target;
        nseen++;
        __atomic_fetch_add(&s_count, 1u, __ATOMIC_RELAXED);
    }
#ifndef _WIN32
    pthread_mutex_unlock(&lock);
#endif
    if (fresh && nseen <= 64) {
        fprintf(stderr, "[SMC] code run outside the image: 0x%08X (%s, first bytes %s), return address 0x%08X; "
                "skipped, nothing runs code that was not lifted\n", target, where, *hex ? hex : "?", ret);
        fflush(stderr);
    }
    recomp_discovery_smc_exec(target, ret, where, hex);
    return 1;
}

unsigned xbox_SmcCount(void) { return load_u(&s_count); }
unsigned xbox_SmcProtectedPages(void) { return load_u(&s_protected); }
