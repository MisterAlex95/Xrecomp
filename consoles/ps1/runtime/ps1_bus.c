/* PS1 memory map (P-4, XBA-162). KUSEG (0x00000000), KSEG0 (0x80000000, cached) and KSEG1 (0xA0000000, uncached)
 * mirror the same 512 MB physical space; KSEG2 holds only the cache control register (0xFFFE0130).
 *   0x00000000  2 MB main RAM, mirrored four times up to 8 MB
 *   0x1F000000  expansion 1 (open bus: reads 0xFF.., writes ignored)
 *   0x1F800000  1 KB scratchpad (data cache used as RAM; not reachable through KSEG1)
 *   0x1F801000  IO ports (ps1_io.c), 0x1F802000 expansion 2 (POST, debug UART), 0x1FA00000 expansion 3 (open bus)
 *   0x1FC00000  512 KB BIOS ROM
 * Everything else is a bus error. With SR.IsC set (cache isolated, the BIOS cache flush), stores do not reach memory. */
#include "ps1_internal.h"

#include <string.h>

static inline uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

enum { REG_NONE, REG_RAM, REG_SCRATCH, REG_IO, REG_BIOS, REG_EXP1, REG_CACHECTL };

/* Decodes a virtual address. Returns the region and the physical address; -1 = address error (user mode touching
 * a kernel segment). */
static int decode(ps1_machine *m, uint32_t addr, uint32_t *phys)
{
    uint32_t seg = addr >> 29;
    if (seg >= 4 && (m->cop0[C0_SR] & SR_KUC))
        return -1;
    if (seg == 7) {
        *phys = addr;
        return addr == 0xFFFE0130u ? REG_CACHECTL : REG_NONE;
    }
    if (seg == 6 || (seg >= 1 && seg <= 3))
        return REG_NONE;
    uint32_t p = addr & 0x1FFFFFFFu;
    *phys = p;
    if (p < 0x00800000u)
        return REG_RAM;
    if (p >= 0x1F000000u && p < 0x1F800000u)
        return REG_EXP1;
    if (p >= 0x1F800000u && p < 0x1F800400u)
        return seg == 5 ? REG_NONE : REG_SCRATCH;
    if (p >= 0x1F801000u && p < 0x1F803000u)
        return REG_IO;
    if (p >= 0x1FC00000u && p < 0x1FC80000u)
        return REG_BIOS;
    if (p >= 0x1FA00000u && p < 0x1FC00000u)
        return REG_EXP1; /* expansion 3: open bus too */
    return REG_NONE;
}

static int read_any(ps1_machine *m, uint32_t addr, uint32_t *v, int width)
{
    uint32_t p = 0;
    int r = decode(m, addr, &p);
    const uint8_t *src;
    switch (r) {
    case -1:
        return EXC_ADEL;
    case REG_RAM:
        src = m->ram + (p & 0x1FFFFFu);
        break;
    case REG_SCRATCH:
        src = m->scratch + (p & 0x3FFu);
        break;
    case REG_BIOS:
        src = m->bios + (p & 0x7FFFFu);
        break;
    case REG_IO:
        *v = ps1_io_read(m, p, width);
        return 0;
    case REG_EXP1:
        *v = width == 4 ? 0xFFFFFFFFu : width == 2 ? 0xFFFFu : 0xFFu;
        return 0;
    case REG_CACHECTL:
        *v = m->cache_ctrl;
        return 0;
    default:
        return EXC_DBE;
    }
    if (width == 4)
        *v = rd32le(src);
    else if (width == 2)
        *v = (uint32_t)src[0] | (uint32_t)src[1] << 8;
    else
        *v = src[0];
    return 0;
}

static int write_any(ps1_machine *m, uint32_t addr, uint32_t v, int width)
{
    uint32_t p = 0;
    if (m->cop0[C0_SR] & SR_ISC)
        return 0; /* cache isolated: the store only reaches the (unmodelled) cache */
    int r = decode(m, addr, &p);
    uint8_t *dst;
    switch (r) {
    case -1:
        return EXC_ADES;
    case REG_RAM:
        dst = m->ram + (p & 0x1FFFFFu);
        break;
    case REG_SCRATCH:
        dst = m->scratch + (p & 0x3FFu);
        break;
    case REG_IO:
        ps1_io_write(m, p, v, width);
        return 0;
    case REG_BIOS:
    case REG_EXP1:
        return 0;
    case REG_CACHECTL:
        m->cache_ctrl = v;
        return 0;
    default:
        return EXC_DBE;
    }
    if (r == REG_RAM)
        PS1_RAM_TOUCH(m, p);
    if (width == 4) {
        wr32le(dst, v);
    } else if (width == 2) {
        dst[0] = (uint8_t)v;
        dst[1] = (uint8_t)(v >> 8);
    } else {
        dst[0] = (uint8_t)v;
    }
    return 0;
}

int ps1_bus_fault(ps1_machine *m, uint32_t addr, int store)
{
    uint32_t p = 0;
    if (store && (m->cop0[C0_SR] & SR_ISC))
        return 0;
    int r = decode(m, addr, &p);
    if (r == -1)
        return store ? EXC_ADES : EXC_ADEL;
    return r == REG_NONE ? EXC_DBE : 0;
}

int ps1_read32(ps1_machine *m, uint32_t addr, uint32_t *v) { return read_any(m, addr, v, 4); }
int ps1_read16(ps1_machine *m, uint32_t addr, uint32_t *v) { return read_any(m, addr, v, 2); }
int ps1_read8(ps1_machine *m, uint32_t addr, uint32_t *v) { return read_any(m, addr, v, 1); }
int ps1_write32(ps1_machine *m, uint32_t addr, uint32_t v) { return write_any(m, addr, v, 4); }
int ps1_write16(ps1_machine *m, uint32_t addr, uint32_t v) { return write_any(m, addr, v, 2); }
int ps1_write8(ps1_machine *m, uint32_t addr, uint32_t v) { return write_any(m, addr, v, 1); }

/* ---- Side-effect-free accessors ---- */
static const uint8_t *peek_ptr(ps1_machine *m, uint32_t addr)
{
    uint32_t seg = addr >> 29;
    if (seg == 7 || seg == 6 || (seg >= 1 && seg <= 3))
        return NULL;
    uint32_t p = addr & 0x1FFFFFFFu;
    if (p < 0x00800000u)
        return m->ram + (p & 0x1FFFFFu);
    if (p >= 0x1F800000u && p < 0x1F800400u)
        return m->scratch + (p & 0x3FFu);
    if (p >= 0x1FC00000u && p < 0x1FC80000u)
        return m->bios + (p & 0x7FFFFu);
    return NULL;
}

uint8_t ps1_peek8(ps1_machine *m, uint32_t addr)
{
    const uint8_t *p = peek_ptr(m, addr);
    return p ? *p : 0;
}

/* The bytes of an aligned halfword / word lie in one region (each is a multiple of 1 KB, aligned): one lookup
 * (XBA-277: the HLE BIOS reads its tables through these). An unaligned one byte by byte, as before. */
uint16_t ps1_peek16(ps1_machine *m, uint32_t addr)
{
    if (!(addr & 1u)) {
        const uint8_t *p = peek_ptr(m, addr);
        return p ? (uint16_t)(p[0] | p[1] << 8) : 0;
    }
    return (uint16_t)(ps1_peek8(m, addr) | ps1_peek8(m, addr + 1) << 8);
}

uint32_t ps1_peek32(ps1_machine *m, uint32_t addr)
{
    if (!(addr & 3u)) {
        const uint8_t *p = peek_ptr(m, addr);
        return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0;
    }
    return (uint32_t)ps1_peek16(m, addr) | (uint32_t)ps1_peek16(m, addr + 2) << 16;
}

void ps1_poke32(ps1_machine *m, uint32_t addr, uint32_t v)
{
    uint8_t *p = (uint8_t *)peek_ptr(m, addr & ~3u);
    if (p && p != m->bios + ((addr & ~3u) & 0x7FFFFu)) {
        wr32le(p, v);
        if (p >= m->ram && p < m->ram + PS1_RAM_SIZE)
            PS1_RAM_TOUCH(m, (uint32_t)(p - m->ram));
    }
}
