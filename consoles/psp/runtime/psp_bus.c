/* psp_bus.c -- the PSP spike's memory map (XBA-224): main RAM (32 MB at 0x08000000), VRAM (2 MB at 0x04000000,
 * its mirrors up to 0x04800000 folded on it), the scratchpad (16 KB at 0x00010000). Every segment (KUSEG, the
 * 0x40000000 uncached mirror, KSEG0 / KSEG1) reaches them through the low 29 bits. IO (0x1C000000-0x1FFFFFFF)
 * reads 0 and ignores writes: the spike drives no hardware register, the HLE does. Anything else faults. */
#include <string.h>

#include "psp_internal.h"

uint8_t *psp_ptr(psp_machine *m, uint32_t addr, uint32_t len)
{
    uint32_t p = addr & 0x1FFFFFFFu;
    if (p >= PSP_RAM_BASE && p - PSP_RAM_BASE + len <= PSP_RAM_SIZE)
        return m->h.ram + (p - PSP_RAM_BASE);
    if (p >= PSP_VRAM_BASE && p < PSP_VRAM_BASE + 0x00800000u) {
        uint32_t o = (p - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1);
        return o + len <= PSP_VRAM_SIZE ? m->vram + o : NULL;
    }
    if (p >= PSP_SCRATCH_BASE && p - PSP_SCRATCH_BASE + len <= PSP_SCRATCH_SIZE)
        return m->scratch + (p - PSP_SCRATCH_BASE);
    return NULL;
}

static bool is_io(uint32_t addr)
{
    uint32_t p = addr & 0x1FFFFFFFu;
    return p >= 0x1C000000u;
}

int psp_read(psp_machine *m, uint32_t addr, int width, uint32_t *v)
{
    if (addr & (uint32_t)(width - 1))
        return 1;
    uint8_t *p = psp_ptr(m, addr, (uint32_t)width);
    if (!p) {
        if (!is_io(addr))
            return 1;
        *v = 0;
        return 0;
    }
    uint32_t x = 0;
    memcpy(&x, p, (size_t)width);
    *v = x;
    return 0;
}

int psp_write(psp_machine *m, uint32_t addr, int width, uint32_t v)
{
    if (addr & (uint32_t)(width - 1))
        return 1;
    uint8_t *p = psp_ptr(m, addr, (uint32_t)width);
    if (!p)
        return is_io(addr) ? 0 : 1;
    memcpy(p, &v, (size_t)width);
    return 0;
}

uint32_t psp_r32(psp_machine *m, uint32_t addr)
{
    uint32_t v = 0;
    return psp_read(m, addr, 4, &v) ? 0 : v;
}

void psp_w32(psp_machine *m, uint32_t addr, uint32_t v)
{
    (void)psp_write(m, addr, 4, v);
}

/* Lifted code's slow path (psp_recomp_rt.h). */
int psp_rc_read(psp_machine *m, uint32_t addr, int width, uint32_t *v)
{
    return psp_read(m, addr, width, v);
}

int psp_rc_write(psp_machine *m, uint32_t addr, int width, uint32_t v)
{
    return psp_write(m, addr, width, v);
}

int psp_rc_swlr(psp_machine *m, uint32_t addr, uint32_t rt, int right)
{
    uint32_t a = addr & ~3u, w, sh = addr & 3;
    if (psp_read(m, a, 4, &w))
        return 1;
    if (right)
        w = sh == 0 ? rt : (w & (0xFFFFFFFFu >> (32 - sh * 8))) | (rt << (sh * 8));
    else
        w = sh == 3 ? rt : (w & (0xFFFFFFFFu << ((sh + 1) * 8))) | (rt >> (24 - sh * 8));
    return psp_write(m, a, 4, w);
}

uint32_t psp_peek32(psp_machine *m, uint32_t addr)
{
    return psp_r32(m, addr);
}
