/* ps2_load.c -- loading an EE executable (XBA-226): a little-endian ELF32 for MIPS, as PS2SDK links one (statically,
 * at 0x00100000, no relocations). PT_LOAD segments are copied into main RAM (file bytes, then zeros up to the memory
 * size); the entry is e_entry. The executable's code (what lifted code covers, and where the interpreter records the
 * addresses it had to start at) is the span of its SHF_EXECINSTR sections, or of its executable segments without a
 * section table. ps2_tools.elf loads the same way, byte for byte. */
#include <stdio.h>
#include <string.h>

#include "ps2_internal.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

int ps2_load_elf(ps2_machine *m, const uint8_t *d, size_t size, char *why, size_t why_len)
{
    if (size < 52 || memcmp(d, "\x7F" "ELF", 4)) {
        snprintf(why, why_len, "not an ELF file");
        return -1;
    }
    if (d[4] != 1 || d[5] != 1 || rd16(d + 18) != 8) {
        snprintf(why, why_len, "not a little-endian 32-bit MIPS ELF");
        return -1;
    }
    if (rd16(d + 16) != 2) {
        snprintf(why, why_len, "not an executable ELF (IRX / relocatable modules are not loaded)");
        return -1;
    }
    uint32_t phoff = rd32(d + 28), shoff = rd32(d + 32);
    uint16_t phentsize = rd16(d + 42), phnum = rd16(d + 44), shentsize = rd16(d + 46), shnum = rd16(d + 48);
    m->entry = rd32(d + 24);
    uint32_t xlo = 0xFFFFFFFFu, xhi = 0, hi = 0;
    int loaded = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        size_t o = (size_t)phoff + (size_t)i * phentsize;
        if (o + 32 > size)
            break;
        const uint8_t *ph = d + o;
        if (rd32(ph) != 1) /* PT_LOAD */
            continue;
        uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), filesz = rd32(ph + 16), memsz = rd32(ph + 20);
        uint32_t flags = rd32(ph + 24);
        if (filesz > memsz || (size_t)off + filesz > size) {
            snprintf(why, why_len, "segment %u: bad sizes", i);
            return -1;
        }
        uint8_t *dst = ps2_ptr(m, vaddr, memsz);
        if (!dst || (vaddr & 0x1FFFFFFFu) < PS2_KERNEL_TOP) {
            snprintf(why, why_len, "segment %u at 0x%08X (+0x%X) outside user RAM", i, vaddr, memsz);
            return -1;
        }
        memcpy(dst, d + off, filesz);
        memset(dst + filesz, 0, memsz - filesz);
        if ((flags & 1) && memsz) { /* PF_X */
            xlo = vaddr < xlo ? vaddr : xlo;
            xhi = vaddr + memsz > xhi ? vaddr + memsz : xhi;
        }
        hi = vaddr + memsz > hi ? vaddr + memsz : hi;
        loaded++;
    }
    if (!loaded) {
        snprintf(why, why_len, "no PT_LOAD segment");
        return -1;
    }
    uint32_t slo = 0xFFFFFFFFu, shi = 0;
    for (uint32_t i = 0; shoff && i < shnum; i++) {
        size_t o = (size_t)shoff + (size_t)i * shentsize;
        if (o + 40 > size)
            break;
        const uint8_t *sh = d + o;
        uint32_t flags = rd32(sh + 8), addr = rd32(sh + 12), ssize = rd32(sh + 20);
        if ((flags & 0x4) && (flags & 0x2) && ssize) { /* SHF_EXECINSTR | SHF_ALLOC */
            slo = addr < slo ? addr : slo;
            shi = addr + ssize > shi ? addr + ssize : shi;
        }
    }
    if (shi > slo) {
        xlo = slo;
        xhi = shi;
    }
    if (xhi <= xlo) {
        snprintf(why, why_len, "no code (no executable section or segment)");
        return -1;
    }
    m->code_lo = xlo;
    m->code_hi = xhi;
    m->image_hi = hi;
    m->h.pc = m->entry;
    m->h.next_pc = m->entry + 4;
    return 0;
}
