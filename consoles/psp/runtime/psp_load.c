/* psp_load.c -- loads a PSP module into the spike's RAM (XBA-224): EBOOT.PBP (its DATA.PSP), PRX (relocatable
 * ELF, e_type 0xFFA0, loaded at PSP_LOAD_BASE) or ELF (ET_EXEC, at its addresses). The same steps as
 * consoles/psp/tools/psp_tools/prx.py, byte for byte (lifted code is checked against these bytes): segments copied,
 * SHT_PRXRELOC relocations applied (R_MIPS_32 / 26 / HI16 paired with the next LO16 / LO16), module info read,
 * each import stub patched to `jr ra ; syscall <import index>`, module_start and its thread parameters taken from
 * the exports. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp_internal.h"

#define SHT_PRXRELOC 0x700000A0u

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

#define FAIL(...)                                                                                                    \
    do {                                                                                                             \
        snprintf(why, why_len, __VA_ARGS__);                                                                         \
        return -1;                                                                                                   \
    } while (0)

static void cstr(psp_machine *m, uint32_t addr, char *out, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n; i++) {
        uint8_t *p = psp_ptr(m, addr + (uint32_t)i, 1);
        if (!p || !*p)
            break;
        out[i] = (char)*p;
    }
    out[i] = 0;
}

int psp_load_module(psp_machine *m, const uint8_t *data, size_t size, char *why, size_t why_len)
{
    if (size >= 40 && !memcmp(data, "\0PBP", 4)) {
        uint32_t o6 = rd32(data + 8 + 24), o7 = rd32(data + 8 + 28);
        if (o6 >= size)
            FAIL("PBP: DATA.PSP out of range");
        size_t end = o7 >= o6 && o7 <= size ? o7 : size;
        data += o6;
        size = end - o6;
    }
    if (size >= 4 && !memcmp(data, "~PSP", 4))
        FAIL("encrypted module (~PSP): decrypt it first");
    if (size < 52 || memcmp(data, "\x7f" "ELF", 4) || data[4] != 1 || data[5] != 1)
        FAIL("not a little-endian ELF32");
    uint16_t type = rd16(data + 16), machine = rd16(data + 18);
    uint32_t e_entry = rd32(data + 24), phoff = rd32(data + 28), shoff = rd32(data + 32);
    uint16_t phentsize = rd16(data + 42), phnum = rd16(data + 44), shentsize = rd16(data + 46),
             shnum = rd16(data + 48);
    if (machine != 8)
        FAIL("not a MIPS ELF (machine %u)", machine);
    if (type != 2 && type != 0xFFA0)
        FAIL("ELF type 0x%X: neither an executable nor a PRX", type);
    bool prx = type == 0xFFA0;
    uint32_t reloc = prx ? PSP_LOAD_BASE : 0;
    if ((uint64_t)phoff + (uint64_t)phnum * phentsize > size
        || (shoff && (uint64_t)shoff + (uint64_t)shnum * shentsize > size))
        FAIL("truncated ELF headers");
    uint32_t segs[8];
    int nseg = 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    const uint8_t *first = NULL;
    for (int k = 0; k < phnum; k++) {
        const uint8_t *ph = data + phoff + (size_t)k * phentsize;
        if (rd32(ph) != 1)
            continue;
        uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), filesz = rd32(ph + 16), memsz = rd32(ph + 20);
        if ((uint64_t)off + filesz > size || filesz > memsz)
            FAIL("segment %d out of the file", k);
        uint8_t *dst = psp_ptr(m, vaddr + reloc, memsz ? memsz : 1);
        if (!dst || ((vaddr + reloc) & 0x1FFFFFFFu) < PSP_USER_LO)
            FAIL("segment %d at 0x%08X outside user memory", k, vaddr + reloc);
        memcpy(dst, data + off, filesz);
        memset(dst + filesz, 0, memsz - filesz);
        if (nseg < 8)
            segs[nseg++] = vaddr;
        if (!first)
            first = ph;
        if (vaddr + reloc < lo)
            lo = vaddr + reloc;
        if (vaddr + reloc + memsz > hi)
            hi = vaddr + reloc + memsz;
    }
    if (!first)
        FAIL("no PT_LOAD segment");
    m->mod_lo = lo;
    m->mod_hi = hi;
    /* the code: executable PROGBITS sections, else the first segment */
    uint32_t clo = 0xFFFFFFFFu, chi = 0;
    for (int k = 0; shoff && k < shnum; k++) {
        const uint8_t *sh = data + shoff + (size_t)k * shentsize;
        if (rd32(sh + 4) == 1 && (rd32(sh + 8) & 4) && rd32(sh + 20)) {
            uint32_t a = rd32(sh + 12) + reloc, e = a + rd32(sh + 20);
            if (a < clo)
                clo = a;
            if (e > chi)
                chi = e;
        }
    }
    if (chi == 0) {
        clo = rd32(first + 8) + reloc;
        chi = clo + rd32(first + 16);
    }
    m->code_lo = clo;
    m->code_hi = chi;
    /* relocations */
    for (int k = 0; prx && shoff && k < shnum; k++) {
        const uint8_t *sh = data + shoff + (size_t)k * shentsize;
        if (rd32(sh + 4) != SHT_PRXRELOC)
            continue;
        uint32_t off = rd32(sh + 16), len = rd32(sh + 20);
        if ((uint64_t)off + len > size)
            FAIL("relocation section %d out of the file", k);
        uint32_t n = len / 8;
        const uint8_t *rel = data + off;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t roff = rd32(rel + 8 * i), info = rd32(rel + 8 * i + 4);
            uint32_t typ = info & 0xFF, os = (info >> 8) & 0xFF, as = (info >> 16) & 0xFF;
            if ((int)os >= nseg || (int)as >= nseg)
                FAIL("relocation %u: segment out of range", i);
            uint32_t at = segs[os] + roff + reloc, delta = segs[as] + reloc;
            uint8_t *p = psp_ptr(m, at, 4);
            if (!p || at < lo || at + 4 > hi)
                FAIL("relocation %u at 0x%08X outside the module", i, at);
            uint32_t w = rd32(p), nw = w;
            switch (typ) {
            case 0: case 7: break;
            case 2: nw = w + delta; break;
            case 4: nw = (w & 0xFC000000u) | (((w & 0x03FFFFFFu) + (delta >> 2)) & 0x03FFFFFFu); break;
            case 5: {
                int32_t l = 0;
                for (uint32_t j = i + 1; j < n; j++) {
                    uint32_t info2 = rd32(rel + 8 * j + 4);
                    if ((info2 & 0xFF) == 6) {
                        uint32_t os2 = (info2 >> 8) & 0xFF;
                        uint8_t *q = (int)os2 < nseg ? psp_ptr(m, segs[os2] + rd32(rel + 8 * j) + reloc, 4) : NULL;
                        if (q)
                            l = (int16_t)rd16(q);
                        break;
                    }
                }
                uint32_t full = ((w & 0xFFFFu) << 16) + (uint32_t)l + delta;
                nw = (w & 0xFFFF0000u) | (((full + 0x8000u) >> 16) & 0xFFFFu);
                break;
            }
            case 6: nw = (w & 0xFFFF0000u) | ((w + delta) & 0xFFFFu); break;
            default: FAIL("relocation %u: type %u not handled", i, typ);
            }
            memcpy(p, &nw, 4);
        }
    }
    /* module info */
    uint32_t mi = 0;
    if (prx) {
        uint32_t off = rd32(first + 4), paddr = rd32(first + 12), filesz = rd32(first + 16);
        if (paddr >= off && paddr < off + filesz)
            mi = paddr - off + rd32(first + 8) + reloc;
    }
    if (!mi && shoff && rd16(data + 50) < shnum) {
        const uint8_t *strsh = data + shoff + (size_t)rd16(data + 50) * shentsize;
        uint32_t stro = rd32(strsh + 16);
        for (int k = 0; k < shnum; k++) {
            const uint8_t *sh = data + shoff + (size_t)k * shentsize;
            uint32_t no = stro + rd32(sh);
            if (no + 22 <= size && !memcmp(data + no, ".rodata.sceModuleInfo", 22))
                mi = rd32(sh + 12) + reloc;
        }
    }
    if (!mi || !psp_ptr(m, mi, 52))
        FAIL("no module info");
    cstr(m, mi + 4, m->mod_name, sizeof m->mod_name);
    m->gp = psp_r32(m, mi + 32);
    uint32_t ent_top = psp_r32(m, mi + 36), ent_end = psp_r32(m, mi + 40), stub_top = psp_r32(m, mi + 44),
             stub_end = psp_r32(m, mi + 48);
    m->entry = e_entry + reloc;
    m->start_prio = 0x20;
    m->start_stack = 0x40000;
    m->start_attr = 0;
    /* exports: module_start, module_start_thread_parameter */
    for (uint32_t a = ent_top; ent_top && a + 16 <= ent_end;) {
        uint32_t len = psp_r32(m, a + 8) & 0xFF, vcount = (psp_r32(m, a + 8) >> 8) & 0xFF,
                 fcount = psp_r32(m, a + 8) >> 16, table = psp_r32(m, a + 12);
        if (len < 4)
            break;
        for (uint32_t k = 0; k < fcount + vcount; k++) {
            uint32_t nid = psp_r32(m, table + 4 * k), val = psp_r32(m, table + 4 * (fcount + vcount + k));
            if (k < fcount && nid == 0xD632ACDBu)
                m->entry = val;
            else if (k >= fcount && nid == 0x0F7C276Cu && psp_r32(m, val) >= 3) {
                m->start_prio = psp_r32(m, val + 4);
                m->start_stack = psp_r32(m, val + 8);
                m->start_attr = psp_r32(m, val + 12);
            }
        }
        a += len * 4;
    }
    /* imports */
    int cap = 0;
    for (uint32_t a = stub_top; stub_top && a + 20 <= stub_end;) {
        uint32_t w2 = psp_r32(m, a + 8);
        uint32_t len = w2 & 0xFF, fcount = w2 >> 16, nids = psp_r32(m, a + 12), stubs = psp_r32(m, a + 16);
        if (len < 5)
            break;
        char lib[32];
        cstr(m, psp_r32(m, a), lib, sizeof lib);
        for (uint32_t k = 0; k < fcount; k++) {
            if (m->nimports == cap) {
                cap = cap ? cap * 2 : 64;
                psp_import *p = realloc(m->imports, (size_t)cap * sizeof *p);
                if (!p)
                    FAIL("out of memory");
                m->imports = p;
            }
            psp_import *imp = &m->imports[m->nimports];
            snprintf(imp->lib, sizeof imp->lib, "%s", lib);
            imp->nid = psp_r32(m, nids + 4 * k);
            imp->stub = stubs + 8 * k;
            imp->handler = psp_hle_handler_of(lib, imp->nid);
            if (!psp_ptr(m, imp->stub, 8))
                FAIL("import stub 0x%08X outside memory", imp->stub);
            psp_w32(m, imp->stub, 0x03E00008u);                                        /* jr ra */
            psp_w32(m, imp->stub + 4, (((uint32_t)m->nimports & 0xFFFFFu) << 6) | 0x0Cu); /* syscall n */
            m->nimports++;
        }
        a += len * 4;
    }
    return 0;
}
