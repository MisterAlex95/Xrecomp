/*
 * Memory regressions: the guest heap and address space, checked against the
 * bugs they had.
 *
 * Runs the real memory layout on the small synthetic XBE from
 * tools/conformance, so no title and no game files are needed. The bridge is
 * called through the thunk dispatcher, the path a title's kernel calls take.
 *
 * One process per mode, because the switches are read once:
 *
 *   default       No switch set. Pins what every title gets today, so a change
 *                 that alters the default shows up here.
 *   heap-reclaim  RECOMP_HEAP_RECLAIM=1. Each check fails without the fix:
 *
 *   1. A small request after a large free takes only what it needs. Reuse used
 *      to hand over the whole block, so a 16-byte request could take 2 MB.
 *   2. Freeing three neighbours in an awkward order merges all three. Merging
 *      left an empty slot behind that later frees did not step over, so the
 *      third block never joined the other two.
 *   3. NtFreeVirtualMemory(MEM_RELEASE) on memory NtAllocateVirtualMemory took
 *      from the heap gives it back. It handed the 32-bit guest slot to the host
 *      VirtualFree as a pointer, which failed, so none of it ever returned.
 *   5. MEM_DECOMMIT then MEM_COMMIT on heap memory gives zeroed pages, as on
 *      the console. Both were no-ops, so the old contents came back.
 *   6. (default too) MmFreeContiguousMemory gives a contiguous block back:
 *      the pages in use drop by exactly that block, and the next request of
 *      that size gets the same address. It used to be kept unless
 *      RECOMP_HEAP_RECLAIM was set, so a title that freed and reloaded a scene
 *      ran the 64 MB window out.
 *   7. (default too) MmAllocateContiguousMemoryEx's physical range is
 *      honoured. A request limited to the low 16 MB still succeeds once
 *      unconstrained requests have filled up to it -- XDK D3D's visibility
 *      test report pages ask for exactly that, and a bump pointer refused
 *      every one -- and a request for a range above the pointer lands in it.
 *
 *   contig-keep   RECOMP_CONTIG_RECLAIM=0: the old behaviour, contiguous
 *                 memory is never reused.
 *   ext-vma       RECOMP_EXT_VMA=1. A title that reserves a specific address
 *                 above the RAM mirrors gets it, can commit inside it and use
 *                 the memory, is told the truth when it asks, and is refused
 *                 cleanly for an address that would alias live memory.
 *   ext-vma-stress  RECOMP_EXT_VMA=1, the layout brought up and down 40 times
 *                 while four threads map and unmap host memory as fast as
 *                 they can (malloc arenas, thread stacks): the tracker must
 *                 come up every time. The range used to be released and
 *                 reserved again at the same fixed host address, and a
 *                 mapping landing in that gap left it off (POSIX).
 *   ext-vma-128   The same switch on a 128 MB map, where the mirrors already
 *                 reach past user space. The tracker must stay out of the way
 *                 rather than reserve a range of negative size.
 *
 *   8. (default, POSIX) A title's large reservation without a map size
 *      (XBA-103). Fable reserves 608 MB for its allocator: the heap cannot
 *      back it, and it used to be clamped to what the heap could (38 MB) unless
 *      the title called xbox_SetMapSize. It is granted whole, from the top of
 *      user space down, not aliasing low RAM; commits, decommits and the
 *      release work; the low mirrors and the contiguous window stay; and
 *      MmQueryStatistics still reports a 64 MB console.
 *   lazy-off      RECOMP_LAZY_RESERVE=0: the clamp again.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "guest_vmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

enum { S_ALLOC, S_FREE, S_QUERY, S_STATS, N_SLOTS };
static const uint32_t ORD[N_SLOTS] = {
    184,   /* NtAllocateVirtualMemory */
    199,   /* NtFreeVirtualMemory */
    217,   /* NtQueryVirtualMemory */
    181,   /* MmQueryStatistics */
};

static uint32_t scratch;                 /* guest VA of a 64 KB block */
#define THUNK_VA (scratch)
#define STACK_VA (scratch + 0x1000)
#define OUT_VA   (scratch + 0x2000)

static uint32_t slot_va[N_SLOTS];
static int failures;

static uint32_t *G(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static uint32_t call(int slot, int nargs, const uint32_t *args)
{
    uint32_t *sp = G(STACK_VA);
    int i;

    sp[0] = 0xBEEF0001u;
    for (i = 0; i < nargs; i++)
        sp[1 + i] = args[i];
    g_esp = STACK_VA;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(slot_va[slot])();
    return g_eax;
}

/* NtAllocateVirtualMemory(&base, 0, &size, type, RW). On return *base and *size
 * hold what the kernel wrote back. */
static uint32_t nt_alloc_at(uint32_t *base, uint32_t *size, uint32_t type)
{
    uint32_t args[5], st;

    *G(OUT_VA) = *base;
    *G(OUT_VA + 4) = *size;
    args[0] = OUT_VA; args[1] = 0; args[2] = OUT_VA + 4;
    args[3] = type; args[4] = 0x04;
    st = call(S_ALLOC, 5, args);
    *base = *G(OUT_VA);
    *size = *G(OUT_VA + 4);
    return st;
}

/* Reserve and commit `size` bytes anywhere. */
static uint32_t nt_alloc(uint32_t size, uint32_t *out_base)
{
    uint32_t base = 0;
    uint32_t st = nt_alloc_at(&base, &size, 0x3000);

    *out_base = base;
    return st;
}

/* NtFreeVirtualMemory(&base, &size = 0, MEM_RELEASE). */
static uint32_t nt_release(uint32_t base)
{
    uint32_t args[3];

    *G(OUT_VA) = base;
    *G(OUT_VA + 4) = 0;
    args[0] = OUT_VA; args[1] = OUT_VA + 4; args[2] = 0x8000;
    return call(S_FREE, 3, args);
}

/* NtQueryVirtualMemory(base, &info); fills mbi[7] with the guest
 * MEMORY_BASIC_INFORMATION fields. */
static uint32_t nt_query(uint32_t base, uint32_t mbi[7])
{
    uint32_t args[2], st, i;

    args[0] = base; args[1] = OUT_VA + 0x100;
    st = call(S_QUERY, 2, args);
    for (i = 0; i < 7; i++)
        mbi[i] = *G(OUT_VA + 0x100 + 4 * i);
    return st;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

#if !defined(_WIN32)
#include <pthread.h>
#include <sys/mman.h>
static volatile int churn_stop;
static void *churn(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u;
    while (!churn_stop) {
        size_t mb = 1 + (seed = seed * 1103515245u + 12345u) % 256;
        void *p = mmap(NULL, mb << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED)
            munmap(p, mb << 20);
    }
    return NULL;
}

static int ext_vma_stress(const unsigned char *xbe, size_t len)
{
    pthread_t th[4];
    int i, up = 0, active = 0;

    set_env("RECOMP_EXT_VMA", "1");
    for (i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, churn, (void *)(uintptr_t)(i + 1));
    for (i = 0; i < 40; i++) {
        if (!xbox_MemoryLayoutInit(xbe, len))
            continue;
        up++;
        active += guest_vmem_active();
        xbox_MemoryLayoutShutdown();
    }
    churn_stop = 1;
    for (i = 0; i < 4; i++)
        pthread_join(th[i], NULL);
    printf("  layout up %d/40, tracker on %d/%d\n", up, active, up);
    return up == 40 && active == 40 ? 0 : 1;
}
#endif

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int reclaim = !strcmp(mode, "heap-reclaim");
    int ext128 = !strcmp(mode, "ext-vma-128");
    int ext = ext128 || !strcmp(mode, "ext-vma");
    int avoid = !strcmp(mode, "contig-avoid-image");
    int overlap = !strcmp(mode, "contig-overlap");
    int keep = !strcmp(mode, "contig-keep");
    int lazy_off = !strcmp(mode, "lazy-off");
    size_t xbe_len = 0;
    unsigned char *xbe;
    char d[160];
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(mode, "ext-vma-stress")) {
#if defined(_WIN32)
        puts("ext-vma-stress: POSIX only");
        return 77;
#else
        int rc;
        xbe = load_file(path, &xbe_len);
        if (!xbe) {
            printf("cannot read %s\n", path);
            return 2;
        }
        rc = ext_vma_stress(xbe, xbe_len);
        free(xbe);
        return rc;
#endif
    }
    if (!reclaim && !ext && !avoid && !overlap && !keep && !lazy_off && strcmp(mode, "default")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }
    if (reclaim)
        set_env("RECOMP_HEAP_RECLAIM", "1");
    if (ext)
        set_env("RECOMP_EXT_VMA", "1");
    if (avoid)
        set_env("RECOMP_CONTIG_AVOID_IMAGE", "1");
    if (overlap)
        set_env("RECOMP_CONTIG_AVOID_IMAGE", "0");
    if (keep)
        set_env("RECOMP_CONTIG_RECLAIM", "0");
    if (lazy_off)
        set_env("RECOMP_LAZY_RESERVE", "0");
    if (ext128)
        xbox_SetTotalRam(XBOX_DEVKIT_RAM);

    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }

    scratch = xbox_HeapAlloc(0x10000, 4096);
    for (i = 0; i < N_SLOTS; i++)
        *G(THUNK_VA + 4 * i) = 0x80000000u | ORD[i];
    xbox_kernel_set_thunk_address(THUNK_VA, N_SLOTS);
    xbox_kernel_bridge_init();
    for (i = 0; i < N_SLOTS; i++) {
        slot_va[i] = *G(THUNK_VA + 4 * i);
        if (!recomp_lookup_kernel(slot_va[i])) {
            printf("ordinal %u did not resolve to a bridge entry point\n", ORD[i]);
            return 2;
        }
    }
    printf("mode: %s\n", mode);

    /* 0. A contiguous block keeps out of the loaded image's physical range
     * (the default, as the console's kernel does); RECOMP_CONTIG_AVOID_IMAGE=0
     * is upstream's bottom-up fill, where they share physical numbers. */
    if (!ext) {
        uint32_t c = xbox_ContiguousAlloc(1024u * 1024u, 4096);
        uint32_t phys = c - XBOX_CONTIG_BASE;
        int overlaps = phys < g_xbox_image_hi && phys + 1024u * 1024u > g_xbox_image_lo;
        snprintf(d, sizeof d, "block 0x%08X, image 0x%08X..0x%08X",
                 c, g_xbox_image_lo, g_xbox_image_hi);
        if (overlap)
            check(c && overlaps, "AVOID_IMAGE=0: a 1 MB block still fills from the bottom", d);
        else
            check(c && !overlaps, "a 1 MB block lands outside the image (the default)", d);
        xbox_ContiguousFree(c);
    }

    /* 1. A small request after a large free. */
    if (!ext) {
        const uint32_t big = 2u * 1024 * 1024;
        uint32_t a = xbox_HeapAlloc(big, 4096);
        uint32_t guard = xbox_HeapAlloc(4096, 16);
        uint32_t s1, s2;

        (void)guard;
        xbox_HeapFree(a);
        s1 = xbox_HeapAlloc(16, 16);
        s2 = xbox_HeapAlloc(16, 16);

        if (reclaim) {
            snprintf(d, sizeof d, "block is %u bytes, expected 16", xbox_HeapBlockSize(s1));
            check(xbox_HeapBlockSize(s1) == 16, "16-byte request takes 16 bytes of a freed 2 MB", d);
            snprintf(d, sizeof d, "second request at 0x%08X, outside 0x%08X", s2, a);
            check(s2 >= a && s2 < a + big, "the rest of the block is still available", d);
        } else {
            snprintf(d, sizeof d, "block is %u bytes", xbox_HeapBlockSize(s1));
            check(s1 == a && xbox_HeapBlockSize(s1) == big,
                  "default: reuse hands over the whole freed block", d);
            check(s2 < a || s2 >= a + big,
                  "default: the second request cannot share it", NULL);
        }
    }

    /* 2. Three neighbours freed so that the third meets an empty slot. */
    if (reclaim) {
        uint32_t x = xbox_HeapAlloc(64, 16), y = xbox_HeapAlloc(64, 16);
        uint32_t z = xbox_HeapAlloc(64, 16), w = xbox_HeapAlloc(64, 16);
        uint32_t m;

        (void)w;
        xbox_HeapFree(x);
        xbox_HeapFree(y);        /* merges into x; y's slot is left empty */
        xbox_HeapFree(z);        /* must step over that slot to reach x */
        m = xbox_HeapAlloc(192, 16);
        snprintf(d, sizeof d, "got 0x%08X, expected 0x%08X", m, x);
        check(y == x + 64 && z == y + 64 && m == x,
              "three freed neighbours merge into one 192-byte block", d);
    }

    /* 3. Releasing NtAllocateVirtualMemory memory returns it. */
    if (reclaim) {
        const uint32_t size = 2u * 1024 * 1024;
        uint32_t b1, b2, st;

        st = nt_alloc(size, &b1);
        check(st == 0 && b1 != 0, "NtAllocateVirtualMemory takes 2 MB from the heap", NULL);
        st = nt_release(b1);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == 0, "NtFreeVirtualMemory(MEM_RELEASE) succeeds", d);
        check(xbox_HeapBlockSize(b1) == 0, "the block is no longer live", NULL);
        st = nt_alloc(size, &b2);
        snprintf(d, sizeof d, "got 0x%08X, expected 0x%08X", b2, b1);
        check(st == 0 && b2 == b1, "the next 2 MB request reuses it", d);
    }

    /* 5. Decommitted heap memory comes back zeroed. */
    if (reclaim) {
        const uint32_t size = 0x10000;
        uint32_t b1, base, sz, args[3], st;

        nt_alloc(size, &b1);
        memset(G(b1), 0xA5, size);
        *G(OUT_VA) = b1 + 0x1000;            /* decommit the second page only */
        *G(OUT_VA + 4) = 0x1000;
        args[0] = OUT_VA; args[1] = OUT_VA + 4; args[2] = 0x4000;   /* MEM_DECOMMIT */
        st = call(S_FREE, 3, args);
        base = b1 + 0x1000; sz = 0x1000;
        nt_alloc_at(&base, &sz, 0x1000);     /* MEM_COMMIT it again */
        snprintf(d, sizeof d, "status 0x%08X, page 0x%08X, neighbour 0x%08X",
                 st, *G(b1 + 0x1000), *G(b1 + 0x2000));
        check(st == 0 && *G(b1 + 0x1000) == 0 && *G(b1 + 0x1FFC) == 0 &&
              *G(b1) == 0xA5A5A5A5u && *G(b1 + 0x2000) == 0xA5A5A5A5u,
              "a decommitted page comes back zeroed, its neighbours kept", d);
    }

    /* 6. Contiguous memory is given back, once, unless RECOMP_CONTIG_RECLAIM=0. */
    if (!ext) {
        uint32_t c1 = xbox_ContiguousAlloc(0x20000, 4096);
        uint32_t before = xbox_ContiguousInUseBytes(), after, c2;
        int said = xbox_ContiguousFree(c1);

        after = xbox_ContiguousInUseBytes();
        c2 = xbox_ContiguousAlloc(0x20000, 4096);
        if (!keep) {
            snprintf(d, sizeof d, "free said %d, in use %u -> %u bytes", said,
                     before, after);
            check(c1 && said == 1 && before - after == 0x20000,
                  "a contiguous free releases exactly that block", d);
            snprintf(d, sizeof d, "got 0x%08X, expected 0x%08X", c2, c1);
            check(c2 == c1, "the next request of that size reuses its address", d);
        } else {
            snprintf(d, sizeof d, "free said %d, in use %u -> %u bytes, got 0x%08X",
                     said, before, after, c2);
            check(c1 && said == 1 && after == before && c2 != c1,
                  "contig-keep: contiguous memory is never reused", d);
        }
    }

    /* 7. The physical range of MmAllocateContiguousMemoryEx is honoured. */
    if (!ext) {
        const uint32_t MB = 1024u * 1024u;
        uint32_t c = 0, lo, hi;
        int k;

        /* Unconstrained 1 MB blocks until one lands at or above 16 MB. */
        for (k = 0; k < 32; k++) {
            c = xbox_ContiguousAlloc(MB, 4096);
            if (!c || c - XBOX_CONTIG_BASE >= 16 * MB)
                break;
        }
        lo = xbox_ContiguousAllocEx(0x1000, 4096, 0, 0x00FFFFFFu);
        snprintf(d, sizeof d, "last unconstrained 0x%08X, low request 0x%08X", c, lo);
        check(c && lo && lo + 0x1000 - XBOX_CONTIG_BASE <= 16 * MB,
              "a request limited to the low 16 MB still fits", d);

        hi = xbox_ContiguousAllocEx(0x10000, 0x10000, 0x02800000u, 0x02FFFFFFu);
        snprintf(d, sizeof d, "got 0x%08X", hi);
        check(hi && hi - XBOX_CONTIG_BASE >= 0x02800000u &&
              hi + 0x10000 - XBOX_CONTIG_BASE <= 0x03000000u &&
              (hi & 0xFFFFu) == 0,
              "a request for a range above the fill lands in it, aligned", d);

        /* A caller-placed block that overlaps hi's last page still claims
         * the free pages after it, so they are not handed out again. */
        if (hi) {
            uint32_t after = hi + 0x10000, again;
            xbox_ContiguousClaim(after - 0x1000, 0x3000);
            again = xbox_ContiguousAllocEx(0x1000, 4096, after - XBOX_CONTIG_BASE,
                                           after - XBOX_CONTIG_BASE + 0x1FFF);
            snprintf(d, sizeof d, "got 0x%08X", again);
            check(again == 0, "a partly overlapped claim keeps its free pages", d);
        }
    }

    /* 4. Above the RAM mirrors: [0x74000000, 0x7FFE0000) at 64 MB. */
    {
        const uint32_t hi = 0x76000000u;
        uint32_t base, size, st, mbi[7];

        if (ext128) {
            check(!guest_vmem_active(),
                  "128 MB: the mirrors reach past user space, tracker stays off", NULL);
        } else if (!ext && !reclaim) {
            base = hi; size = 0x100000;
            st = nt_alloc_at(&base, &size, 0x2000);
            snprintf(d, sizeof d, "status 0x%08X, base 0x%08X", st, base);
            check(base != hi, "default: an explicit high base is not honoured", d);
        } else if (ext) {
            /* The layout's own reservations (the Windows placeholder window,
             * the POSIX span) cover this range; it must hand it over, or the
             * tracker finds nothing free and stays off. */
            check(guest_vmem_active(),
                  "the tracker is on: the layout handed its range over",
                  "\"could not reserve host memory\": the placeholder window "
                  "or the span still held the range");
            /* And the windows above it are still mapped: on Linux a span
             * grown to make room pushed the contiguous window onto a
             * neighbouring mapping. Writing to it faults if it is missing. */
            {
                uint32_t c = xbox_ContiguousAlloc(0x1000, 4096);
                if (c)
                    *G(c) = 0x5A5A5A5Au;
                check(c && *G(c) == 0x5A5A5A5Au,
                      "the contiguous window is mapped beside the tracker", NULL);
            }
            /* Reserve the exact address. */
            base = hi; size = 0x100000;
            st = nt_alloc_at(&base, &size, 0x2000);
            snprintf(d, sizeof d, "status 0x%08X, base 0x%08X", st, base);
            check(st == 0 && base == hi && size == 0x100000,
                  "reserve at 0x76000000 returns that address", d);

            /* Commit the first 64 KB and use it. */
            base = hi; size = 0x10000;
            st = nt_alloc_at(&base, &size, 0x1000);
            check(st == 0 && base == hi, "commit inside the reservation succeeds", NULL);
            *G(hi) = 0xC0FFEE01u;
            *G(hi + 0xFFFC) = 0xC0FFEE02u;
            check(*G(hi) == 0xC0FFEE01u && *G(hi + 0xFFFC) == 0xC0FFEE02u,
                  "the committed memory is real", NULL);

            /* Ask about both halves. */
            st = nt_query(hi, mbi);
            snprintf(d, sizeof d, "state 0x%X size 0x%X base 0x%X", mbi[4], mbi[3], mbi[1]);
            check(st == 0 && mbi[0] == hi && mbi[1] == hi && mbi[3] == 0x10000 &&
                  mbi[4] == 0x1000, "query: the committed part is MEM_COMMIT, 64 KB", d);
            st = nt_query(hi + 0x10000, mbi);
            snprintf(d, sizeof d, "state 0x%X size 0x%X base 0x%X", mbi[4], mbi[3], mbi[1]);
            check(st == 0 && mbi[0] == hi + 0x10000 && mbi[1] == hi &&
                  mbi[3] == 0xF0000 && mbi[4] == 0x2000,
                  "query: the rest is MEM_RESERVE, 960 KB, same allocation base", d);

            /* The same range again is a conflict, not a second grant. */
            base = hi; size = 0x100000;
            st = nt_alloc_at(&base, &size, 0x2000);
            snprintf(d, sizeof d, "status 0x%08X", st);
            check(st == 0xC0000018u, "reserving it again is STATUS_CONFLICTING_ADDRESSES", d);

            /* An address inside the mirrors would alias live memory. */
            base = 0x10000000u; size = 0x100000;
            st = nt_alloc_at(&base, &size, 0x2000);
            snprintf(d, sizeof d, "status 0x%08X", st);
            check(st == 0xC0000018u, "an address inside the mirrors is refused", d);
            st = nt_query(0x10000000u, mbi);
            snprintf(d, sizeof d, "state 0x%X", mbi[4]);
            check(st == 0 && mbi[4] == 0x2000,
                  "query: the mirrors read as reserved, so a scanner moves on", d);

            /* base = 0 still comes from the heap, below the mirrors. */
            st = nt_alloc(0x10000, &base);
            snprintf(d, sizeof d, "status 0x%08X, base 0x%08X", st, base);
            check(st == 0 && base && base < 0x04000000u,
                  "base = 0 still comes from the heap", d);

            /* Release gives the range back. */
            st = nt_release(hi);
            snprintf(d, sizeof d, "status 0x%08X", st);
            check(st == 0, "release of the reservation succeeds", d);
            st = nt_query(hi, mbi);
            check(st == 0 && mbi[4] == 0x10000, "query: it reads as MEM_FREE again", NULL);
            base = hi; size = 0x100000;
            st = nt_alloc_at(&base, &size, 0x3000);
            check(st == 0 && base == hi && *G(hi) == 0,
                  "reserving it again works, and the memory is zero", NULL);
        }
    }

    /* 9. NtQueryVirtualMemory ends at the top of user space, so a scanner
     *    walking region by region from 0 stops instead of wrapping round
     *    the 4 GB space for ever. */
    {
        uint32_t mbi[7], st, a = 0, n = 0, last = 0;

        st = nt_query(0x80000000u, mbi);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == 0xC000000Du, "query above user space is STATUS_INVALID_PARAMETER", d);
        st = nt_query(0xFFFFF000u, mbi);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == 0xC000000Du, "query at the top page is STATUS_INVALID_PARAMETER", d);
        for (;;) {
            st = nt_query(a, mbi);
            if (st >= 0x80000000u || !mbi[3] || n > 4000000u)
                break;
            last = a;
            n++;
            if ((uint64_t)a + mbi[3] > 0xFFFFFFFFu)
                break;
            a = mbi[0] + mbi[3];
        }
        snprintf(d, sizeof d, "%u queries, last 0x%08X, status 0x%08X", n, last, st);
        check(st == 0xC000000Du && last < GUEST_VMEM_TOP,
              "a region walk from 0 ends at the top of user space", d);
    }

    /* 8. A 608 MB reservation without a map size (XBA-103). */
    if (lazy_off) {
        const uint32_t want = 608u << 20;
        uint32_t base = 0, size = want, st;

        st = nt_alloc_at(&base, &size, 0x2000);
        snprintf(d, sizeof d, "status 0x%08X, base 0x%08X, size %u", st, base, size);
        check(st == 0 && size < want && base < 0x04000000u,
              "lazy-off: a 608 MB reserve is clamped to the heap", d);
    }
#if !defined(_WIN32)
    if (!strcmp(mode, "default")) {
        const uint32_t MB = 1024u * 1024u, want = 608u * MB, ram = 64u * MB;
        uint32_t base = 0, size = want, st, b2 = 0, s2 = 304u * MB, cb, cs;
        uint32_t low_before, mirror_probe = scratch + 0x3000, args[3];

        *G(mirror_probe) = 0x6D697272u;
        st = nt_alloc_at(&base, &size, 0x2000);
        snprintf(d, sizeof d, "status 0x%08X, base 0x%08X, size %u", st, base, size);
        check(st == 0 && size == want, "a 608 MB reserve is granted whole", d);
        check(st == 0 && base >= ram && (uint64_t)base + size <= GUEST_VMEM_TOP,
              "it sits above RAM, below the end of user space", d);
        if (st == 0 && size == want) {
            low_before = *G(base % ram);
            *G(base) = ~low_before;
            *G(base + size - 4) = 0xE4D0F608u;
            snprintf(d, sizeof d, "guest 0x%08X now 0x%08X", base % ram, *G(base % ram));
            check(*G(base % ram) == low_before && *G(base + size - 4) == 0xE4D0F608u,
                  "a write in it does not reach low RAM", d);
            check(xbox_ReserveHolds(base, size) && !xbox_ReserveHolds(base - 0x1000, 0x1000),
                  "xbox_ReserveHolds covers exactly the reservation", NULL);

            /* MEM_COMMIT inside: the pages are already there. */
            cb = base + 0x100000; cs = 0x10000;
            st = nt_alloc_at(&cb, &cs, 0x1000);
            *G(cb) = 0xC0117u;
            check(st == 0 && cb == base + 0x100000 && *G(cb) == 0xC0117u,
                  "commit inside it succeeds and the memory is real", NULL);

            /* A second one goes below the first. */
            st = nt_alloc_at(&b2, &s2, 0x2000);
            snprintf(d, sizeof d, "status 0x%08X, base 0x%08X, size %u", st, b2, s2);
            check(st == 0 && s2 == 304u * MB && b2 >= ram && b2 + s2 <= base,
                  "a second 304 MB reserve lands below it", d);

            /* Decommit one page: zero, its neighbours kept. */
            memset(G(base + 0x200000), 0xA5, 0x3000);
            *G(OUT_VA) = base + 0x201000;
            *G(OUT_VA + 4) = 0x1000;
            args[0] = OUT_VA; args[1] = OUT_VA + 4; args[2] = 0x4000;
            st = call(S_FREE, 3, args);
            snprintf(d, sizeof d, "status 0x%08X, page 0x%08X, neighbours 0x%08X 0x%08X",
                     st, *G(base + 0x201000), *G(base + 0x200000), *G(base + 0x202000));
            check(st == 0 && *G(base + 0x201000) == 0 && *G(base + 0x201FFC) == 0
                  && *G(base + 0x200000) == 0xA5A5A5A5u
                  && *G(base + 0x202000) == 0xA5A5A5A5u,
                  "a decommitted page reads zero, its neighbours kept", d);

            /* MmQueryStatistics: still a 64 MB console. */
            args[0] = OUT_VA + 0x200;
            memset(G(OUT_VA + 0x200), 0, 0x40);
            *G(OUT_VA + 0x200) = 0x24;
            st = call(S_STATS, 1, args);
            snprintf(d, sizeof d, "status 0x%08X, total %u pages, available %u",
                     st, *G(OUT_VA + 0x204), *G(OUT_VA + 0x208));
            check(st == 0 && *G(OUT_VA + 0x204) == 16384u
                  && *G(OUT_VA + 0x208) <= 16384u,
                  "MmQueryStatistics still reports 64 MB of RAM", d);

            /* The low mirrors and the contiguous window are still there. */
            check(*G(ram + mirror_probe) == 0x6D697272u,
                  "the first RAM mirror still aliases low RAM", NULL);
            {
                uint32_t c = xbox_ContiguousAlloc(0x1000, 4096);
                if (c)
                    *G(c) = 0x5A5A5A5Au;
                check(c && *G(c) == 0x5A5A5A5Au,
                      "the contiguous window is still mapped", NULL);
            }

            /* Release: the whole reservation, zero afterwards. */
            st = nt_release(base + 0x1000);
            check(st == 0xC000009Fu, "a release inside it is refused (not at the base)", NULL);
            st = nt_release(base);
            snprintf(d, sizeof d, "status 0x%08X, size %u", st, *G(OUT_VA + 4));
            check(st == 0 && *G(OUT_VA + 4) == want, "the release gives back 608 MB", d);
            check(*G(base) == 0 && *G(base + size - 4) == 0 && *G(cb) == 0,
                  "released pages read zero", NULL);
        }
    }
#endif

    xbox_MemoryLayoutShutdown();
    free(xbe);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
