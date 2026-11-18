/* x360_mem.c -- the guest address space (XBA-270).
 *
 * One 4 GB host reservation (PROT_NONE, MAP_NORESERVE): guest address a is host base + a, so lifted code reaches
 * memory with no translation and no bounds check. The map follows the console (and Xenia's description of it):
 *   0x00000000-0x3FFFFFFF  virtual, 4 KB pages   (NtAllocateVirtualMemory)
 *   0x40000000-0x6FFFFFFF  virtual, 64 KB pages  (NtAllocateVirtualMemory with MEM_LARGE_PAGES)
 *   0x70000000-0x7EFFFFFF  thread stacks
 *   0x7F000000-0x7FFFFFFF  kernel objects of this runtime (KTHREAD, PCR, TLS, events, export variables)
 *   0x80000000-0x9FFFFFFF  the XEX image
 *   0xA0000000-0xBFFFFFFF  physical memory (512 MB), 64 KB pages view
 *   0xC0000000-0xDFFFFFFF  the same physical memory, 16 MB pages view
 *   0xE0000000-0xFFFFFFFF  the same physical memory, 4 KB pages view
 * The three physical views map one memfd, so a write through one is seen through the others (Xenia offsets the
 * 0xE0000000 view by one page; not done here). Virtual ranges are committed (mprotect read / write) as the heaps
 * hand them out; a guest access to an uncommitted page faults in the host. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
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

int x360_mem_init(x360_machine *x, char *why, size_t n)
{
    void *b = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (b == MAP_FAILED) {
        snprintf(why, n, "cannot reserve the 4 GB guest address space: %s", strerror(errno));
        return -1;
    }
    x->base = b;
    x->m.mem = b;
    x->memfd = memfd_create("x360-physical", MFD_CLOEXEC);
    if (x->memfd < 0 || ftruncate(x->memfd, PHYS_SIZE) != 0) {
        snprintf(why, n, "cannot create the physical memory: %s", strerror(errno));
        return -1;
    }
    for (uint64_t a = 0xA0000000u; a < 0x100000000ull; a += PHYS_SIZE)
        if (mmap(x->base + a, PHYS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, x->memfd, 0) == MAP_FAILED) {
            snprintf(why, n, "cannot map the physical memory at %08X: %s", (unsigned)a, strerror(errno));
            return -1;
        }
    if (heap_init(&x->heap_v4k, "virtual 4K", 0x00010000u, 0x40000000u - 0x00010000u, 0x1000, 0) ||
        heap_init(&x->heap_v64k, "virtual 64K", 0x40000000u, 0x30000000u, 0x10000, 0) ||
        heap_init(&x->heap_stack, "stacks", 0x70000000u, 0x0F000000u, 0x1000, 0) ||
        heap_init(&x->heap_kernel, "kernel", 0x7F000000u, 0x01000000u, 0x1000, 0) ||
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
    if (x->base)
        munmap(x->base, 1ull << 32);
    if (x->memfd >= 0)
        close(x->memfd);
}

int x360_commit(x360_machine *x, uint32_t addr, uint32_t size)
{
    uint32_t a = addr & ~0xFFFu;
    uint64_t e = ((uint64_t)addr + size + 0xFFF) & ~0xFFFull;
    if (a >= 0xA0000000u)
        return 0; /* physical: always mapped */
    return mprotect(x->base + a, (size_t)(e - a), PROT_READ | PROT_WRITE);
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
            mark(h, p, n);
            uint32_t a = heap_addr(h, p);
            if (!h->physical)
                x360_commit(x, a, n * h->page);
            memset(x->base + (h->physical ? 0xA0000000u + a : a), 0, (size_t)n * h->page);
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
    mark(h, first, last - first);
    if (!h->physical)
        x360_commit(x, heap_addr(h, first), (last - first) * h->page);
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
    if (addr >= 0xA0000000u)
        return &x->heap_phys;
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
    memset(x->base + a, 0, size);
    return a;
}
