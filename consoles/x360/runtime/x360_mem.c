/* x360_mem.c -- the guest address space (XBA-270).
 *
 * One 4 GB host reservation (PROT_NONE, MAP_NORESERVE): guest address a is host base + a, so lifted code reaches
 * memory with no translation and no bounds check. The map follows the console (and Xenia's description of it):
 *   0x00000000-0x3FFFFFFF  virtual, 4 KB pages   (NtAllocateVirtualMemory)
 *   0x40000000-0x6FFFFFFF  virtual, 64 KB pages  (NtAllocateVirtualMemory with MEM_LARGE_PAGES)
 *   0x70000000-0x7EFFFFFF  thread stacks
 *   0x7F000000-0x7FBFFFFF  kernel objects of this runtime (KTHREAD, PCR, TLS, events, export variables)
 *   0x7FC80000-0x7FC8FFFF  the GPU's registers (x360_gpu.c: plain memory the scheduler reads)
 *   0x80000000-0x9FFFFFFF  the XEX image
 *   0xA0000000-0xBFFFFFFF  physical memory (512 MB), 64 KB pages view
 *   0xC0000000-0xDFFFFFFF  the same physical memory, 16 MB pages view
 *   0xE0000000-0xFFFFFFFF  the same physical memory, 4 KB pages view
 * The three physical views map one memfd (a POSIX shared memory object, unlinked at once, on macOS: no memfd_create
 * there), so a write through one is seen through the others. As on the console, the
 * 0xE0000000 view starts one page into physical memory (virtual 0xE0000000 is physical 0x1000; its last page is not
 * mapped): x360_v2p / x360_p2v (x360_internal.h) are the only place that knows it. The physical heap is indexed by
 * physical address and the runtime itself reads physical memory through the 0xA0000000 view. Virtual ranges are
 * committed (mprotect read / write) as the heaps hand them out; a guest access to an uncommitted page faults in the
 * host.
 *
 * The host's page size. The guest's pages are 4 KB, the host's may be larger (16 KB on recent Android devices, up to
 * 64 KB on some arm64 kernels); mmap and mprotect work in host pages:
 * - a commit is widened to whole host pages (a neighbour of a committed guest page may then be readable where the
 *   console would fault; never the other way round);
 * - a view 0x1000 into the file cannot be mapped where the page is larger, and no placement of the views in one
 *   linear reservation makes the three alias (their distances are not multiples of the page). There the 4 KB view
 *   is mapped from the start of the file and the address of a guest pointer into it is 0x1000 higher than
 *   reservation + address: xe_ptr (xenon_rt.h) adds xe_machine.view4k_skew, for lifted code and, through x360_p,
 *   for the runtime. Guest 0xE0000000 + v is then host reservation + 0xE0001000 + v, file offset 0x1000 + v, the
 *   byte guest 0xA0001000 + v names. The view's last guest page lands in the host page past the 4 GB, reserved and
 *   never mapped. xe_ptr does so when built with XE_VIEW4K_DYNAMIC (xenon_rt.h): the runtime always is, a pack is
 *   by default except on x86 outside Android (4 KB pages), where its accesses stay reservation + address. A pack that
 *   is not (or made before this) cannot run where the view moved (XE_RC_FEAT_VIEW4K, x360_bind).
 * Every mapping call goes through map_view / protect_rw, which refuse what a host of that page size would refuse.
 * X360_HOST_PAGE (tests only) makes the runtime behave as on a host of a larger page: a power of two, at least the
 * real size, at most 64 KB. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "x360_internal.h"

#define PHYS_SIZE 0x20000000u

static int heap_init(x360_heap *h, const char *name, uint32_t base, uint32_t size, uint32_t page, int physical)
{
    h->name = name;
    h->base = base;
    h->size = size;
    h->page = page;
    h->pages = size / page;
    h->len = calloc(h->pages, sizeof *h->len);
    h->used = calloc(h->pages, 1);
    h->physical = physical;
    return h->len && h->used ? 0 : -1;
}

static uint32_t host_page_size(void)
{
    long real = sysconf(_SC_PAGESIZE);
    uint32_t page = real > 0 ? (uint32_t)real : 0x1000u;
    const char *e = getenv("X360_HOST_PAGE");
    if (e && *e) {
        unsigned long v = strtoul(e, NULL, 0);
        if (v >= page && v <= 0x10000u && !(v & (v - 1)))
            page = (uint32_t)v;
    }
    return page;
}

/* mmap of the physical file at a guest address, as a host of x->host_page would take it */
static int map_view(x360_machine *x, uint64_t at, size_t len, uint32_t file_off)
{
    if ((at | file_off) & (x->host_page - 1)) {
        errno = EINVAL;
        return -1;
    }
    void *got = mmap(x->reservation + at, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, x->memfd, file_off);
    return got == MAP_FAILED ? -1 : 0;
}

static int protect_rw(x360_machine *x, uint64_t at, uint64_t len)
{
    if ((at | len) & (x->host_page - 1)) {
        errno = EINVAL;
        return -1;
    }
    return mprotect(x->reservation + at, (size_t)len, PROT_READ | PROT_WRITE);
}

int x360_mem_init(x360_machine *x, char *why, size_t n)
{
    x->host_page = host_page_size();
    x->m.view4k_skew = x->host_page > X360_VIEW_4K_SKEW ? X360_VIEW_4K_SKEW : 0;
    x->memfd = -1;
    if (x->m.view4k_skew && !XE_VIEW4K_DYNAMIC) { /* x360_p would not follow the view */
        snprintf(why, n, "this runtime was built for hosts of 4 KB pages (XE_VIEW4K_DYNAMIC=0); this one's are %u",
                 (unsigned)x->host_page);
        return -1;
    }
    /* one host page past the 4 GB: the skewed view's last guest page ends there, and must fault */
    size_t size = (size_t)(1ull << 32) + x->host_page;
    void *b = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (b == MAP_FAILED) {
        snprintf(why, n, "cannot reserve the 4 GB guest address space: %s", strerror(errno));
        return -1;
    }
    x->reservation = b;
    x->reserved = size;
    x->m.mem = b;
#ifdef __APPLE__
    {
        char name[64]; /* unique, gone from the namespace at once: only the descriptor keeps it */
        snprintf(name, sizeof name, "/x360-physical-%d-%08x", (int)getpid(), (unsigned)arc4random());
        x->memfd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (x->memfd >= 0) {
            shm_unlink(name);
            fcntl(x->memfd, F_SETFD, FD_CLOEXEC);
        }
    }
#else
    x->memfd = memfd_create("x360-physical", MFD_CLOEXEC);
#endif
    if (x->memfd < 0 || ftruncate(x->memfd, PHYS_SIZE) != 0) {
        snprintf(why, n, "cannot create the physical memory: %s", strerror(errno));
        return -1;
    }
    static const uint32_t views[] = {X360_VIEW_64K, X360_VIEW_16M, X360_VIEW_4K};
    for (unsigned k = 0; k < 3; k++) {
        uint32_t first = x360_v2p(views[k]); /* the physical address the view starts at */
        int r;
        if (first && x->m.view4k_skew) /* the whole file, from the view's base: its guest addresses sit a skew higher */
            r = map_view(x, views[k], PHYS_SIZE, 0);
        else
            r = map_view(x, views[k], PHYS_SIZE - first, first);
        if (r) {
            snprintf(why, n, "cannot map the physical memory at %08X (host pages of %u bytes): %s",
                     (unsigned)views[k], (unsigned)x->host_page, strerror(errno));
            return -1;
        }
    }
    if (heap_init(&x->heap_v4k, "virtual 4K", 0x00010000u, 0x40000000u - 0x00010000u, 0x1000, 0) ||
        heap_init(&x->heap_v64k, "virtual 64K", 0x40000000u, 0x30000000u, 0x10000, 0) ||
        heap_init(&x->heap_stack, "stacks", 0x70000000u, 0x0F000000u, 0x1000, 0) ||
        heap_init(&x->heap_kernel, "kernel", 0x7F000000u, 0x00C00000u, 0x1000, 0) || /* below the GPU page */
        heap_init(&x->heap_phys, "physical", 0x00000000u, PHYS_SIZE, 0x1000, 1)) {
        snprintf(why, n, "out of host memory");
        return -1;
    }
    /* the first physical page stays unused: a physical address of 0 reads as NULL */
    x360_heap_alloc_at(x, &x->heap_phys, 0, 0x10000);
    return 0;
}

void x360_mem_free(x360_machine *x)
{
    x360_heap *hs[] = {&x->heap_v4k, &x->heap_v64k, &x->heap_stack, &x->heap_kernel, &x->heap_phys};
    for (unsigned k = 0; k < sizeof hs / sizeof hs[0]; k++) {
        free(hs[k]->len);
        free(hs[k]->used);
    }
    if (x->reservation)
        munmap(x->reservation, x->reserved);
    if (x->memfd >= 0)
        close(x->memfd);
}

/* Makes [addr, addr + size) readable and writable: whole host pages, so more than was asked where they are larger
 * than the guest's. The physical views are always mapped. */
int x360_commit(x360_machine *x, uint32_t addr, uint32_t size)
{
    uint64_t page = x->host_page;
    uint64_t a = addr & ~(page - 1);
    uint64_t e = ((uint64_t)addr + size + page - 1) & ~(page - 1);
    if (addr >= X360_VIEW_64K)
        return 0; /* physical: always mapped */
    if (e > X360_VIEW_64K)
        e = X360_VIEW_64K;
    return protect_rw(x, a, e - a);
}

static void mark(x360_heap *h, uint32_t first, uint32_t n)
{
    h->len[first] = n;
    memset(h->used + first, 1, n);
}

static uint32_t heap_addr(x360_heap *h, uint32_t page)
{
    return h->base + page * h->page;
}

uint32_t x360_heap_alloc(x360_machine *x, x360_heap *h, uint32_t size, uint32_t align)
{
    if (!size)
        size = 1;
    uint32_t n = (uint32_t)(((uint64_t)size + h->page - 1) / h->page);
    uint32_t step = align > h->page ? align / h->page : 1;
    for (uint32_t p = 0; p + n <= h->pages; p += step) {
        if (h->used[p])
            continue;
        uint32_t k = 0;
        while (k < n && !h->used[p + k])
            k++;
        if (k == n) {
            uint32_t a = heap_addr(h, p);
            if (!h->physical && x360_commit(x, a, n * h->page))
                return 0; /* the host refused: nothing to hand out (and nothing to clear) */
            mark(h, p, n);
            memset(x360_p(x, h->physical ? x360_p2v(X360_VIEW_64K, a) : a), 0, (size_t)n * h->page);
            return a;
        }
        p += k / step * step; /* skip the free run that was too short */
    }
    return 0;
}

int x360_heap_alloc_at(x360_machine *x, x360_heap *h, uint32_t addr, uint32_t size)
{
    if (addr < h->base || (uint64_t)addr + size > (uint64_t)h->base + h->size)
        return -1;
    uint32_t first = (addr - h->base) / h->page;
    uint32_t last = (uint32_t)(((uint64_t)addr + size - h->base + h->page - 1) / h->page);
    for (uint32_t p = first; p < last; p++)
        if (h->used[p])
            return -1;
    if (!h->physical && x360_commit(x, heap_addr(h, first), (last - first) * h->page))
        return -1;
    mark(h, first, last - first);
    return 0;
}

uint32_t x360_heap_size_of(x360_heap *h, uint32_t addr)
{
    if (addr < h->base || addr - h->base >= h->size)
        return 0;
    uint32_t p = (addr - h->base) / h->page;
    return h->len[p] * h->page;
}

int x360_heap_all_used(x360_heap *h, uint32_t addr, uint32_t size)
{
    if (addr < h->base || (uint64_t)addr + size > (uint64_t)h->base + h->size || !size)
        return 0;
    uint32_t first = (addr - h->base) / h->page;
    uint32_t last = (uint32_t)(((uint64_t)addr + size - h->base + h->page - 1) / h->page);
    for (uint32_t p = first; p < last; p++)
        if (!h->used[p])
            return 0;
    return 1;
}

uint32_t x360_heap_free(x360_machine *x, x360_heap *h, uint32_t addr)
{
    (void)x;
    if (addr < h->base || addr - h->base >= h->size)
        return 0;
    uint32_t p = (addr - h->base) / h->page;
    uint32_t n = h->len[p];
    if (!n)
        return 0;
    h->len[p] = 0;
    memset(h->used + p, 0, n);
    return n * h->page; /* the pages stay committed (their contents are cleared on the next allocation) */
}

x360_heap *x360_heap_of(x360_machine *x, uint32_t addr)
{
    x360_heap *hs[] = {&x->heap_v4k, &x->heap_v64k, &x->heap_stack, &x->heap_kernel};
    for (unsigned k = 0; k < 4; k++)
        if (addr >= hs[k]->base && addr - hs[k]->base < hs[k]->size)
            return hs[k];
    if (addr >= X360_VIEW_64K)
        return &x->heap_phys; /* indexed by physical address: x360_v2p(addr) */
    return NULL;
}

/* small kernel allocations: carved from kernel pages 16 bytes at a time (never freed: objects live as long as the
 * title in this runtime) */
uint32_t x360_kalloc(x360_machine *x, uint32_t size)
{
    const uint32_t chunk = 0x10000;
    size = (size + 15) & ~15u;
    uint32_t *cur = &x->kalloc_cur, *end = &x->kalloc_end;
    if (size > chunk)
        return x360_heap_alloc(x, &x->heap_kernel, size, 0);
    if (!*cur || *cur + size > *end) {
        uint32_t a = x360_heap_alloc(x, &x->heap_kernel, chunk, 0);
        if (!a)
            return 0;
        *cur = a;
        *end = a + chunk;
    }
    uint32_t a = *cur;
    *cur += size;
    memset(x360_p(x, a), 0, size);
    return a;
}
