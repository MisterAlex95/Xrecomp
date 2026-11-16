/* gx_fifo.c -- GX's front end (XBA-262): the processor interface FIFO the CPU's write-gather pipe fills, the command
 * processor's registers and parser (display lists, BP / XF / CP register loads, indexed XF loads, draws), the pixel
 * engine's registers (tokens, draw done and their interrupts).
 *
 * The CPU's stores to the gather pipe (0x0C008000) land in the FIFO in RAM at the PI write pointer, as 32-byte bursts
 * would; when the CPU FIFO is linked to the GP FIFO (CP control bit 4, libogc's immediate mode) the CP's write pointer
 * follows and the CP parses what is there at each 32-byte boundary, synchronously: the GP is never behind, so the
 * high / low watermark interrupts never fire and the CP always reads idle. A display list being built (FIFO unlinked)
 * only goes to RAM; CALL_DL parses it from RAM later. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "gx_internal.h"

void gx_log(gx_state *gx, const char *fmt, ...)
{
    (void)gx;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "gx: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void warn_once(gx_state *gx, unsigned id, const char *fmt, ...)
{
    if (id < 64 && gx->warned[id])
        return;
    if (id < 64)
        gx->warned[id] = true;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "gx: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

gx_state *gx_create(uint8_t *ram, uint32_t ram_size, gx_irq_fn irq, void *irq_user)
{
    gx_state *gx = calloc(1, sizeof *gx);
    if (!gx)
        return NULL;
    gx->ram = ram;
    gx->ram_size = ram_size;
    gx->irq = irq;
    gx->irq_user = irq_user;
    gx->tmem = calloc(1, GX_TMEM_SIZE);
    gx->vout = calloc(GX_MAX_VERTS + 4, sizeof *gx->vout);
    gx->cmd_cap = 1u << 16;
    gx->cmd = malloc(gx->cmd_cap);
    if (!gx->tmem || !gx->vout || !gx->cmd) {
        gx_destroy(gx);
        return NULL;
    }
    gx->bp_mask = 0xFFFFFFu;
    gx->cp_sr = 0x0C;
    gx->tex_epoch = 1;
    gx->trace = getenv("GC_GX_TRACE") && atoi(getenv("GC_GX_TRACE"));
    /* XF state a program that never sets it still relies on: one colour channel, identity-ish viewport. */
    gx->xf_reg[0x09] = 1;
    return gx;
}

void gx_destroy(gx_state *gx)
{
    if (!gx)
        return;
    gx_gl_detach(gx);
    free(gx->tmem);
    free(gx->vout);
    free(gx->cmd);
    free(gx);
}

void gx_set_verbose(gx_state *gx, int level)
{
    gx->verbose = level;
}

void gx_get_stats(const gx_state *gx, gx_stats *s)
{
    *s = gx->stats;
}

void gx_field(gx_state *gx)
{
    gx->field_count++;
    gx->tex_epoch++;
    gx_gl_frame(gx);
}

/* ---- interrupts ---- */

static void pe_irq_update(gx_state *gx)
{
    uint32_t set = 0, clear = 0;
    if ((gx->pe_ctrl & 4) && (gx->pe_ctrl & 1))
        set |= GX_INT_PE_TOKEN;
    else
        clear |= GX_INT_PE_TOKEN;
    if ((gx->pe_ctrl & 8) && (gx->pe_ctrl & 2))
        set |= GX_INT_PE_FINISH;
    else
        clear |= GX_INT_PE_FINISH;
    if (gx->irq)
        gx->irq(gx->irq_user, set, clear);
}

/* ---- register loads ---- */

static void load_cp(gx_state *gx, uint8_t addr, uint32_t v)
{
    gx->cpr[addr] = v;
}

static void load_xf(gx_state *gx, uint32_t addr, uint32_t v)
{
    if (addr < 0x800)
        gx->xf.u[addr] = v;
    else if (addr >= 0x1000 && addr < 0x1100)
        gx->xf_reg[addr - 0x1000] = v;
    else if (gx->verbose)
        gx_log(gx, "XF write to %04X = %08X ignored", addr, v);
}

static void tlut_load(gx_state *gx)
{
    uint32_t src = (gx->bp[BP_LOADTLUT0] & 0x1FFFFFu) << 5;
    uint32_t v = gx->bp[BP_LOADTLUT1];
    uint32_t dst = (v & 0x3FFu) << 9;
    uint32_t bytes = BITS(v, 10, 11) * 32u;
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t s = src + i, d = (0x80000u + dst + i) & (GX_TMEM_SIZE - 1);
        gx->tmem[d] = s < gx->ram_size ? gx->ram[s] : 0;
    }
    if (gx->trace)
        gx_log(gx, "TLUT load %08X -> TMEM %05X, %u bytes: %02X%02X %02X%02X ...", src, 0x80000u + dst, bytes,
               gx->tmem[(0x80000u + dst) & (GX_TMEM_SIZE - 1)], gx->tmem[(0x80001u + dst) & (GX_TMEM_SIZE - 1)],
               gx->tmem[(0x80002u + dst) & (GX_TMEM_SIZE - 1)], gx->tmem[(0x80003u + dst) & (GX_TMEM_SIZE - 1)]);
}

static void load_bp(gx_state *gx, uint32_t word)
{
    unsigned addr = word >> 24;
    uint32_t v = word & 0xFFFFFFu;
    if (addr == BP_MASK) {
        gx->bp_mask = v;
        return;
    }
    uint32_t mask = gx->bp_mask;
    gx->bp_mask = 0xFFFFFFu;
    uint32_t nv = (gx->bp[addr] & ~mask) | (v & mask);
    switch (addr) {
    case BP_PE_DONE:
        gx->bp[addr] = nv;
        gx_gl_flush(gx);
        gx->pe_ctrl |= 8;
        pe_irq_update(gx);
        return;
    case BP_PE_TOKEN:
    case BP_PE_TOKEN_INT:
        gx->bp[addr] = nv;
        gx->pe_token = (uint16_t)nv;
        if (addr == BP_PE_TOKEN_INT) {
            gx_gl_flush(gx);
            gx->pe_ctrl |= 4;
            pe_irq_update(gx);
        }
        return;
    case BP_COPY_EXEC:
        gx->bp[addr] = nv;
        gx_gl_flush(gx);
        gx_gl_copy(gx, nv);
        return;
    case BP_LOADTLUT1:
        gx_gl_tex_invalidate(gx);
        gx->bp[addr] = nv;
        tlut_load(gx);
        return;
    case BP_TEX_INVALIDATE:
        gx_gl_tex_invalidate(gx); /* draws so far still see the old contents */
        gx->bp[addr] = nv;
        gx->tex_epoch++;
        return;
    case 0x67: /* performance metrics */
        return;
    default:
        break;
    }
    if (gx->bp[addr] != nv) {
        gx_gl_flush(gx);
        gx->bp[addr] = nv;
    }
    if (addr >= BP_TEV_REGS && addr < BP_TEV_REGS + 8) { /* RA / BG halves of a colour or konst register */
        unsigned r = (addr - BP_TEV_REGS) >> 1;
        int lo = (int)(nv & 0x7FF), hi = (int)((nv >> 12) & 0x7FF);
        lo = lo >= 0x400 ? lo - 0x800 : lo;
        hi = hi >= 0x400 ? hi - 0x800 : hi;
        unsigned c_lo = (addr & 1) ? 2 : 0, c_hi = (addr & 1) ? 1 : 3; /* RA: r, a; BG: b, g */
        if (nv & 0x800000) {
            gx->kreg[r][c_lo] = (uint8_t)lo;
            gx->kreg[r][c_hi] = (uint8_t)hi;
        } else {
            gx->creg[r][c_lo] = (int16_t)lo;
            gx->creg[r][c_hi] = (int16_t)hi;
        }
    }
}

/* ---- the command parser ---- */

static uint32_t rd32(gx_state *gx, uint32_t pa)
{
    return pa + 4 <= gx->ram_size ? gx_be32(gx->ram + pa) : 0;
}

static void indexed_xf_load(gx_state *gx, unsigned op, uint32_t v)
{
    unsigned arr = 12 + ((op - 0x20) >> 3);
    uint32_t index = v >> 16, n = BITS(v, 12, 4) + 1, addr = v & 0xFFF;
    uint32_t src = (gx->cpr[0xA0 + arr] & 0x3FFFFFFFu) + index * (gx->cpr[0xB0 + arr] & 0xFF);
    for (uint32_t i = 0; i < n; i++)
        load_xf(gx, addr + i, rd32(gx, src + 4 * i));
}

static size_t parse(gx_state *gx, const uint8_t *p, size_t n);

static void call_dl(gx_state *gx, uint32_t addr, uint32_t size)
{
    addr &= 0x3FFFFFE0u;
    if (gx->dl_depth > 0) {
        warn_once(gx, 1, "nested display list call ignored (the hardware does not nest them)");
        return;
    }
    if (addr >= gx->ram_size || size > gx->ram_size - addr)
        return;
    gx->dl_depth++;
    size_t used = parse(gx, gx->ram + addr, size);
    if (used < size && gx->verbose)
        gx_log(gx, "display list %08X+%X: %zu bytes left over", addr, size, (size_t)size - used);
    gx->dl_depth--;
}

/* Parses whole commands from p[0..n): returns the bytes used (a command cut at the end is left). */
static size_t parse(gx_state *gx, const uint8_t *p, size_t n)
{
    size_t at = 0;
    while (at < n) {
        uint8_t op = p[at];
        size_t left = n - at;
        if (op == 0x00 || op == 0x48 || op == 0x44) { /* NOP, invalidate vertex cache, metrics */
            at++;
            continue;
        }
        if (op == 0x08) {
            if (left < 6)
                break;
            load_cp(gx, p[at + 1], gx_be32(p + at + 2));
            at += 6;
        } else if (op == 0x10) {
            if (left < 5)
                break;
            uint32_t h = gx_be32(p + at + 1);
            uint32_t cnt = BITS(h, 16, 4) + 1, addr = h & 0xFFFF;
            if (left < 5 + 4 * (size_t)cnt)
                break;
            for (uint32_t i = 0; i < cnt; i++)
                load_xf(gx, addr + i, gx_be32(p + at + 5 + 4 * i));
            at += 5 + 4 * (size_t)cnt;
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            if (left < 5)
                break;
            indexed_xf_load(gx, op, gx_be32(p + at + 1));
            at += 5;
        } else if (op == 0x40) {
            if (left < 9)
                break;
            call_dl(gx, gx_be32(p + at + 1), gx_be32(p + at + 5));
            at += 9;
        } else if (op == 0x61) {
            if (left < 5)
                break;
            load_bp(gx, gx_be32(p + at + 1));
            at += 5;
        } else if (op >= 0x80 && op < 0xC0) {
            if (left < 3)
                break;
            unsigned vat = op & 7, prim = (op >> 3) & 7, cnt = gx_be16(p + at + 1);
            gx->last_vat = vat;
            size_t vs = gx_vertex_size(gx, vat);
            if (left < 3 + vs * cnt)
                break;
            if (cnt) { /* cnt < 65536 = GX_MAX_VERTS: gx->vout holds any draw */
                gx_load_vertices(gx, vat, p + at + 3, cnt);
                gx_gl_draw(gx, prim, cnt);
                gx->stats.vertices += cnt;
                gx->stats.draws++;
            }
            at += 3 + vs * cnt;
        } else {
            gx->stats.unknown_cmds++;
            if (gx->verbose || gx->stats.unknown_cmds <= 4)
                gx_log(gx, "unknown command %02X skipped", op);
            at++;
        }
    }
    return at;
}

/* Feeds bytes from the FIFO into the staging buffer and parses what is complete. */
static void cmd_feed(gx_state *gx, const uint8_t *p, size_t n)
{
    if (gx->cmd_len + n > gx->cmd_cap) {
        size_t cap = gx->cmd_cap;
        while (gx->cmd_len + n > cap)
            cap *= 2;
        uint8_t *c = realloc(gx->cmd, cap);
        if (!c)
            return;
        gx->cmd = c;
        gx->cmd_cap = cap;
    }
    memcpy(gx->cmd + gx->cmd_len, p, n);
    gx->cmd_len += n;
    gx->stats.fifo_bytes += n;
    size_t used = parse(gx, gx->cmd, gx->cmd_len);
    if (used) {
        memmove(gx->cmd, gx->cmd + used, gx->cmd_len - used);
        gx->cmd_len -= used;
    }
}

static uint32_t ring_limit(uint32_t end)
{
    return (end + 31u) & ~31u;
}

/* The CP reads what lies between its read and write pointers. */
static void cp_run(gx_state *gx)
{
    if (!(gx->cp_cr & 1))
        return;
    uint32_t base = gx->cp_base & 0x03FFFFE0u, lim = ring_limit(gx->cp_end & 0x03FFFFFFu);
    int guard = 0;
    while (gx->cp_rp != gx->cp_wp && guard++ < 4) {
        uint32_t rp = gx->cp_rp, wp = gx->cp_wp;
        uint32_t stop = wp >= rp ? wp : lim;
        if (stop > gx->ram_size || rp >= stop) {
            gx->cp_rp = gx->cp_wp;
            break;
        }
        cmd_feed(gx, gx->ram + rp, stop - rp);
        rp = stop;
        if (rp >= lim)
            rp = base;
        gx->cp_rp = rp;
    }
    gx->cp_dist = 0;
}

void gx_fifo_write(gx_state *gx, uint32_t v, unsigned size)
{
    uint32_t base = gx->pi_base & 0x03FFFFE0u, lim = ring_limit(gx->pi_end & 0x03FFFFFFu);
    uint32_t wp = gx->pi_wp & 0x03FFFFFFu;
    bool crossed = false;
    for (unsigned i = 0; i < size; i++) {
        if (wp < gx->ram_size)
            gx->ram[wp] = (uint8_t)(v >> (8 * (size - 1 - i)));
        wp++;
        if ((wp & 31) == 0)
            crossed = true;
        if (wp >= lim && lim > base) {
            wp = base;
            gx->pi_wp |= 0x20000000u; /* wrapped */
        }
    }
    gx->pi_wp = (gx->pi_wp & 0x20000000u) | wp;
    if (crossed && (gx->cp_cr & 0x10)) { /* linked: the GP FIFO's write pointer follows */
        gx->cp_wp = wp & ~31u;
        cp_run(gx);
    }
}

uint32_t gx_pi_read(gx_state *gx, unsigned idx)
{
    switch (idx) {
    case 3:
        return gx->pi_base;
    case 4:
        return gx->pi_end;
    default:
        return gx->pi_wp;
    }
}

void gx_pi_write(gx_state *gx, unsigned idx, uint32_t v)
{
    switch (idx) {
    case 3:
        gx->pi_base = v & 0x03FFFFFFu;
        break;
    case 4:
        gx->pi_end = v & 0x03FFFFFFu;
        break;
    default:
        gx->pi_wp = v & 0x03FFFFFFu; /* a write clears the wrap flag */
        break;
    }
}

/* ---- CP and PE registers ---- */

static uint32_t pair(gx_state *gx, unsigned lo)
{
    return (uint32_t)gx->cp_raw[lo + 1] << 16 | gx->cp_raw[lo];
}

uint16_t gx_cp_read16(gx_state *gx, uint32_t off)
{
    unsigned i = (off & 0x7F) >> 1;
    switch (i) {
    case 0:
        return gx->cp_sr | 0x0C; /* GP read idle, command idle */
    case 1:
        return gx->cp_cr;
    case 7:
        return gx->pe_token;
    case 0x18: return (uint16_t)gx->cp_dist;
    case 0x19: return (uint16_t)(gx->cp_dist >> 16);
    case 0x1A: return (uint16_t)gx->cp_wp;
    case 0x1B: return (uint16_t)(gx->cp_wp >> 16);
    case 0x1C: return (uint16_t)gx->cp_rp;
    case 0x1D: return (uint16_t)(gx->cp_rp >> 16);
    default:
        return i < 0x40 ? gx->cp_raw[i] : 0;
    }
}

void gx_cp_write16(gx_state *gx, uint32_t off, uint16_t v)
{
    unsigned i = (off & 0x7F) >> 1;
    if (i >= 0x40)
        return;
    gx->cp_raw[i] = v;
    switch (i) {
    case 1:
        gx->cp_cr = v;
        cp_run(gx);
        break;
    case 2:
        gx->cp_sr &= (uint16_t)~(v & 3);
        break;
    case 0x10: case 0x11: gx->cp_base = pair(gx, 0x10) & 0x03FFFFFFu; break;
    case 0x12: case 0x13: gx->cp_end = pair(gx, 0x12) & 0x03FFFFFFu; break;
    case 0x14: case 0x15: gx->cp_hiwm = pair(gx, 0x14); break;
    case 0x16: case 0x17: gx->cp_lowm = pair(gx, 0x16); break;
    case 0x18: case 0x19: gx->cp_dist = pair(gx, 0x18); break;
    case 0x1A: case 0x1B:
        gx->cp_wp = pair(gx, 0x1A) & 0x03FFFFFFu;
        if (i == 0x1B)
            cp_run(gx);
        break;
    case 0x1C: case 0x1D: gx->cp_rp = pair(gx, 0x1C) & 0x03FFFFFFu; break;
    case 0x1E: case 0x1F: gx->cp_bp = pair(gx, 0x1E); break;
    default:
        break;
    }
}

uint16_t gx_pe_read16(gx_state *gx, uint32_t off)
{
    unsigned i = (off & 0x3F) >> 1;
    switch (i) {
    case 5:
        return gx->pe_ctrl;
    case 7:
        return gx->pe_token;
    case 8: return 0;   /* bounding box: the whole EFB */
    case 9: return GX_EFB_W - 1;
    case 10: return 0;
    case 11: return GX_EFB_H - 1;
    default:
        return i < 0x20 ? gx->pe_reg[i] : 0;
    }
}

void gx_pe_write16(gx_state *gx, uint32_t off, uint16_t v)
{
    unsigned i = (off & 0x3F) >> 1;
    if (i >= 0x20)
        return;
    gx->pe_reg[i] = v;
    if (i == 5) {
        uint16_t status = (uint16_t)(gx->pe_ctrl & 0x0C & ~(v & 0x0C)); /* write 1 to clear */
        gx->pe_ctrl = (uint16_t)((v & 0x03) | status);
        pe_irq_update(gx);
    }
}
