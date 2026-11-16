/* ctr_loader.c -- the 3DSX loader of the 3DS spike (XBA-228): what the Homebrew Launcher does with a .3dsx
 * (3dbrew « 3DSX Format »). Header, three relocation headers, the code / rodata / data segments (data without its
 * .bss), then per segment its absolute and relative relocation runs ({u16 skip, u16 patch} words). Segments go at
 * 0x00100000, each page-aligned after the previous one; a relocated word holds an address in the file's own space
 * (code at 0, then rodata, then data, each page-aligned), so it becomes 0x00100000 + that address (absolute) or that
 * minus the word's own address (relative, 32-bit or 31-bit). ctr_tools/threedsx.py does the same for the lifter. */
#include <stdio.h>
#include <string.h>

#include "ctr_internal.h"

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t rd16le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t page(uint32_t v)
{
    return (v + 0xFFF) & ~0xFFFu;
}

int ctr_load_3dsx(ctr_machine *m, const uint8_t *d, size_t n, char *why, size_t why_n)
{
    if (n < 0x20 || memcmp(d, "3DSX", 4)) {
        snprintf(why, why_n, "not a 3DSX (magic)");
        return -1;
    }
    uint32_t hdr_size = rd16le(d + 4), rel_size = rd16le(d + 6);
    uint32_t code = rd32le(d + 0x10), ro = rd32le(d + 0x14), data = rd32le(d + 0x18), bss = rd32le(d + 0x1C);
    if (hdr_size < 0x20 || rel_size < 8 || bss > data || code == 0 || code > 0x04000000u || ro > 0x04000000u
        || data > 0x04000000u) {
        snprintf(why, why_n, "bad 3DSX header (header %u, reloc header %u, code %u, data %u, bss %u)", hdr_size,
                 rel_size, code, data, bss);
        return -1;
    }
    size_t at = hdr_size;
    uint32_t nrel[3][2];
    for (int s = 0; s < 3; s++) {
        if (at + rel_size > n) {
            snprintf(why, why_n, "3DSX truncated in its relocation headers");
            return -1;
        }
        nrel[s][0] = rd32le(d + at);
        nrel[s][1] = rd32le(d + at + 4);
        at += rel_size;
    }
    uint32_t file_size[3] = {code, ro, data - bss};
    uint32_t mem_size[3] = {page(code), page(ro), page(data)};
    uint32_t seg_addr[3];
    seg_addr[0] = CTR_CODE_BASE;
    seg_addr[1] = seg_addr[0] + mem_size[0];
    seg_addr[2] = seg_addr[1] + mem_size[1];
    static const uint32_t perm[3] = {5, 1, 3};
    static const uint32_t state[3] = {9, 9, 10}; /* code, code, private */
    for (int s = 0; s < 3; s++) {
        if (at + file_size[s] > n) {
            snprintf(why, why_n, "3DSX truncated in segment %d", s);
            return -1;
        }
        if (mem_size[s] && ctr_mem_map(m, seg_addr[s], mem_size[s], 3, state[s])) {
            snprintf(why, why_n, "cannot map segment %d at 0x%08X", s, seg_addr[s]);
            return -1;
        }
        memcpy(m->h.mem + seg_addr[s], d + at, file_size[s]);
        at += file_size[s];
    }
    uint32_t total = mem_size[0] + mem_size[1] + mem_size[2];
    for (int s = 0; s < 3; s++) {
        for (int t = 0; t < 2; t++) {
            uint32_t pos = seg_addr[s], end = seg_addr[s] + mem_size[s];
            for (uint32_t k = 0; k < nrel[s][t]; k++) {
                if (at + 4 > n) {
                    snprintf(why, why_n, "3DSX truncated in the relocations of segment %d", s);
                    return -1;
                }
                uint32_t skip = rd16le(d + at), patch = rd16le(d + at + 2);
                at += 4;
                pos += skip * 4;
                for (uint32_t p = 0; p < patch && pos < end; p++, pos += 4) {
                    uint32_t orig = ctr_r32(m, pos), sub = orig >> 28, a = orig & 0x0FFFFFFFu;
                    if (a >= total) {
                        snprintf(why, why_n, "relocation at 0x%08X names 0x%X, outside the image", pos, a);
                        return -1;
                    }
                    uint32_t addr = CTR_CODE_BASE + a;
                    if (t == 0) {
                        if (sub) {
                            snprintf(why, why_n, "absolute relocation of kind %u at 0x%08X", sub, pos);
                            return -1;
                        }
                        ctr_w32(m, pos, addr);
                    } else {
                        uint32_t rel = addr - pos;
                        if (sub > 1) {
                            snprintf(why, why_n, "relative relocation of kind %u at 0x%08X", sub, pos);
                            return -1;
                        }
                        ctr_w32(m, pos, sub ? rel & 0x7FFFFFFFu : rel);
                    }
                }
            }
        }
    }
    for (int s = 0; s < 3; s++)
        if (mem_size[s])
            ctr_mem_protect(m, seg_addr[s], mem_size[s], perm[s]);
    m->code_lo = seg_addr[0];
    m->code_hi = seg_addr[0] + code;
    m->ro_hi = seg_addr[1] + ro;
    m->data_hi = seg_addr[2] + data;
    m->entry = CTR_CODE_BASE;
    return 0;
}
