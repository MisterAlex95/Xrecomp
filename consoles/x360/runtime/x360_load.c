/* x360_load.c -- the XEX2 loader (XBA-270): headers, the image mapped at its base, imports bound.
 *
 * Takes an unencrypted XEX2 with compression none or basic. Retail titles are encrypted and LZX-compressed: the
 * Python side decrypts them with the user's own key and rewrites them plain (python3 -m tools.xex FILE.xex --plain
 * OUT.xex), the same step that feeds the recompiler. Layout as in tools/xex/xex.py (Free60 / Xenia descriptions).
 *
 * Imports: each import library lists record addresses in the image; a record's word is (type << 24) | (library
 * index << 16) | ordinal. Type 0 (variable): the record receives the guest address of the export (this runtime
 * gives each known variable export a block of kernel memory, x360_kernel.c). Type 1 (thunk): the 16-byte code at
 * that address is the title's call target for the export; the dispatcher calls the HLE when PC lands on it. */
#include <stdlib.h>
#include <string.h>

#include "x360_internal.h"

#define H_FILE_FORMAT_INFO 0x000003FFu
#define H_ENTRY_POINT 0x00010100u
#define H_IMAGE_BASE_ADDRESS 0x00010201u
#define H_IMPORT_LIBRARIES 0x000103FFu
#define H_TLS_INFO 0x00020104u
#define H_DEFAULT_STACK_SIZE 0x00020200u
#define H_EXECUTION_INFO 0x00040006u

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

uint32_t x360_crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static int lib_id(const char *name)
{
    if (!strncmp(name, "xboxkrnl", 8))
        return X360_LIB_XBOXKRNL;
    if (!strncmp(name, "xam", 3))
        return X360_LIB_XAM;
    return X360_LIB_OTHER;
}

static int cmp_thunk(const void *a, const void *b)
{
    uint32_t x = ((const x360_thunk *)a)->addr, y = ((const x360_thunk *)b)->addr;
    return x < y ? -1 : x > y;
}

int x360_load(x360_machine *x, const uint8_t *d, size_t size, char *why, size_t n)
{
    if (size < 0x18 || memcmp(d, "XEX2", 4)) {
        snprintf(why, n, "not an XEX2 file");
        return -1;
    }
    uint32_t pe_off = be32(d + 8), sec_off = be32(d + 16), count = be32(d + 20);
    if (0x18 + 8ull * count > size || pe_off > size || sec_off + 0x184ull > size) {
        snprintf(why, n, "XEX2 headers out of file");
        return -1;
    }
    const uint8_t *ff = NULL, *imports = NULL, *tls = NULL;
    uint32_t exec_off = 0;
    uint32_t base = 0;
    x->stack_size = 0x40000;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t key = be32(d + 0x18 + 8 * k), val = be32(d + 0x1C + 8 * k);
        const uint8_t *blk = (key & 0xFF) > 1 && val < size ? d + val : NULL;
        switch (key) {
        case H_FILE_FORMAT_INFO: ff = blk; break;
        case H_ENTRY_POINT: x->entry = val; break;
        case H_IMAGE_BASE_ADDRESS: base = val; break;
        case H_IMPORT_LIBRARIES: imports = blk; break;
        case H_TLS_INFO: tls = blk; break;
        case H_DEFAULT_STACK_SIZE: x->stack_size = val; break;
        case H_EXECUTION_INFO: exec_off = val + 24 <= sec_off ? val : 0; break;
        }
    }
    x->image_size = be32(d + sec_off + 4);
    x->image_base = base ? base : be32(d + sec_off + 0x110);
    if (!ff) {
        snprintf(why, n, "XEX2 without a file format header");
        return -1;
    }
    uint16_t enc = be16(ff + 4), comp = be16(ff + 6);
    if (enc != 0 || comp > 1) {
        snprintf(why, n, "the image is %s%s: rewrite the XEX plain first (python3 -m tools.xex FILE --key-file KEY "
                 "--plain OUT.xex)", enc ? "encrypted" : "", comp > 1 ? (enc ? " and LZX-compressed" : "LZX-compressed")
                 : "");
        return -1;
    }
    if (x->image_base < 0x80000000u || (uint64_t)x->image_base + x->image_size > 0xA0000000ull || !x->image_size) {
        snprintf(why, n, "image at %08X (%u bytes) outside the image range", x->image_base, x->image_size);
        return -1;
    }
    if (x360_commit(x, x->image_base, x->image_size)) {
        snprintf(why, n, "cannot commit the image's pages");
        return -1;
    }
    uint8_t *img = x360_p(x, x->image_base);
    const uint8_t *src = d + pe_off;
    size_t avail = size - pe_off;
    if (comp == 0) {
        memcpy(img, src, avail < x->image_size ? avail : x->image_size);
    } else {
        uint32_t ffsize = be32(ff), o = 0, out = 0;
        for (uint32_t b = 8; b + 8 <= ffsize; b += 8) {
            uint32_t ds = be32(ff + b), zs = be32(ff + b + 4);
            if (o + (uint64_t)ds > avail || out + (uint64_t)ds + zs > x->image_size) {
                snprintf(why, n, "basic compression block %u runs past the data", (b - 8) / 8);
                return -1;
            }
            memcpy(img + out, src + o, ds);
            out += ds + zs; /* the zeros are already there */
            o += ds;
        }
    }
    x->image_crc = x360_crc32(img, x->image_size);

    /* the XEX header in guest memory, for RtlImageXexHeaderField */
    x->xex_header = x360_kalloc(x, sec_off);
    memcpy(x360_p(x, x->xex_header), d, sec_off);
    if (exec_off) {
        x->execution_info = x->xex_header + exec_off;
        x->title_id = be32(d + exec_off + 12);
    }

    if (tls) {
        x->tls_raw = be32(tls + 4);
        x->tls_data_size = be32(tls + 8);
        x->tls_raw_size = be32(tls + 12);
    }

    /* imports */
    if (imports) {
        uint32_t isize = be32(imports), str_size = be32(imports + 4);
        const char *names[64] = {0};
        unsigned nnames = 0;
        for (uint32_t o = 12; o < 12 + str_size && nnames < 64;) {
            const char *s = (const char *)imports + o;
            if (*s) {
                names[nnames++] = s;
                o += (uint32_t)strlen(s) + 1;
            } else
                o++;
        }
        unsigned cap = 64;
        x->thunks = malloc(cap * sizeof *x->thunks);
        /* pass 0: the thunks; pass 1: the other records (a function's IAT slot gets its thunk's address, a
         * variable's the export's storage) */
        for (int pass = 0; pass < 2; pass++) {
            unsigned idx = 0;
            for (uint32_t o = 12 + str_size; o + 0x28 <= isize; idx++) {
                uint32_t lsize = be32(imports + o);
                uint16_t name_index = be16(imports + o + 0x24), nrec = be16(imports + o + 0x26);
                int lib = name_index < nnames ? lib_id(names[name_index]) : X360_LIB_OTHER;
                if (lsize < 0x28 + 4u * nrec) {
                    snprintf(why, n, "import library %u: bad size", idx);
                    return -1;
                }
                for (unsigned r = 0; r < nrec; r++) {
                    uint32_t ra = be32(imports + o + 0x28 + 4 * r);
                    if (ra < x->image_base || ra - x->image_base + 4 > x->image_size)
                        continue;
                    uint32_t v = x360_r32(x, ra);
                    uint16_t ord = v & 0xFFFF;
                    if (pass == 0 && (v >> 24) == 1) {
                        if (x->nthunks == cap) {
                            cap *= 2;
                            x->thunks = realloc(x->thunks, cap * sizeof *x->thunks);
                        }
                        x->thunks[x->nthunks++] = (x360_thunk){ra, ord, (uint8_t)lib};
                    } else if (pass == 1 && (v >> 24) == 0) {
                        uint32_t va = 0;
                        for (unsigned t = 0; t < x->nthunks && !va; t++)
                            if (x->thunks[t].lib == lib && x->thunks[t].ordinal == ord)
                                va = x->thunks[t].addr;
                        if (!va && lib < 2 && ord < 4096) {
                            va = x->vars[lib][ord];
                            if (!va) {
                                va = x->vars[lib][ord] = x360_kalloc(x, 64); /* unknown variable: zeros */
                                x360_logf(x, "x360: variable import %s!%s (%u) has no HLE: zeros\n",
                                          lib ? "xam" : "xboxkrnl", x360_export_name(lib, ord) ?: "?", ord);
                            }
                        }
                        x360_w32(x, ra, va);
                    }
                }
                o += lsize;
            }
        }
        qsort(x->thunks, x->nthunks, sizeof *x->thunks, cmp_thunk);
        if (x->nthunks) {
            x->thunk_lo = x->thunks[0].addr;
            x->thunk_hi = x->thunks[x->nthunks - 1].addr + 16;
        }
    }
    return 0;
}
