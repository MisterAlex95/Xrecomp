/* ctr_mem.c -- the 3DS spike's guest address space (XBA-228): one 4 GB host reservation (PROT_NONE), with the
 * process's regions mapped into it at their guest addresses. A user process has no I/O, so every access, in the
 * interpreter and in lifted code, is `mem + address`; an access outside a mapped region faults on the host (ctr_run
 * reports the guest address). Read-only regions (code, rodata, configuration memory) are read-only on the host too:
 * the code the lifter saw cannot change under it. */
#include <string.h>
#include <sys/mman.h>

#include "ctr_internal.h"

#define CTR_SPACE (0x100000000ull + 0x10000ull) /* plus a guard: an access at 0xFFFFFFFF + 3 stays inside */

static uint8_t *g_space; /* the last machine's space, for ctr_guest_address */

static int host_prot(uint32_t perm)
{
    int p = 0;
    if (perm & 1)
        p |= PROT_READ;
    if (perm & 2)
        p |= PROT_READ | PROT_WRITE;
    return p ? p : PROT_NONE;
}

int ctr_mem_init(ctr_machine *m)
{
    if (sizeof(void *) < 8)
        return -1;
    void *p = mmap(NULL, CTR_SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    m->h.mem = p;
    g_space = p;
    return 0;
}

void ctr_mem_free(ctr_machine *m)
{
    if (m->h.mem) {
        munmap(m->h.mem, CTR_SPACE);
        if (g_space == m->h.mem)
            g_space = NULL;
    }
    m->h.mem = NULL;
}

int64_t ctr_guest_address(const void *host)
{
    const uint8_t *p = host;
    if (!g_space || p < g_space || p >= g_space + 0x100000000ull)
        return -1;
    return (int64_t)(p - g_space);
}

/* Maps fresh zeroed pages (page-aligned base and size). */
int ctr_mem_map(ctr_machine *m, uint32_t base, uint32_t size, uint32_t perm, uint32_t state)
{
    if ((base | size) & 0xFFF || !size || m->nregions >= CTR_MAX_REGIONS)
        return -1;
    if ((uint64_t)base + size > 0x100000000ull)
        return -1;
    for (int i = 0; i < m->nregions; i++) {
        const ctr_region *r = &m->regions[i];
        if (base < r->base + r->size && r->base < base + size)
            return -1; /* overlaps */
    }
    void *p = mmap(m->h.mem + base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    if (host_prot(perm) != (PROT_READ | PROT_WRITE))
        mprotect(p, size, host_prot(perm));
    ctr_region *r = &m->regions[m->nregions++];
    r->base = base;
    r->size = size;
    r->perm = perm;
    r->state = state;
    return 0;
}

void ctr_mem_unmap(ctr_machine *m, uint32_t base, uint32_t size)
{
    for (int i = 0; i < m->nregions; i++) {
        ctr_region *r = &m->regions[i];
        if (r->base == base && r->size == size) {
            mmap(m->h.mem + base, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
            m->regions[i] = m->regions[--m->nregions];
            return;
        }
    }
}

void ctr_mem_protect(ctr_machine *m, uint32_t base, uint32_t size, uint32_t perm)
{
    mprotect(m->h.mem + base, size, host_prot(perm));
    for (int i = 0; i < m->nregions; i++)
        if (m->regions[i].base == base)
            m->regions[i].perm = perm;
}

const ctr_region *ctr_mem_region(const ctr_machine *m, uint32_t addr)
{
    for (int i = 0; i < m->nregions; i++) {
        const ctr_region *r = &m->regions[i];
        if (addr >= r->base && addr - r->base < r->size)
            return r;
    }
    return NULL;
}

bool ctr_mem_mapped(const ctr_machine *m, uint32_t addr, uint32_t size)
{
    while (size) {
        const ctr_region *r = ctr_mem_region(m, addr);
        if (!r)
            return false;
        uint32_t left = r->base + r->size - addr;
        if (left >= size)
            return true;
        addr += left;
        size -= left;
    }
    return true;
}

uint32_t ctr_peek32(ctr_machine *m, uint32_t addr)
{
    if (!ctr_mem_mapped(m, addr, 4))
        return 0;
    return ctr_r32(m, addr);
}
