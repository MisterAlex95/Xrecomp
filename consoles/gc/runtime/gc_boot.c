/* gc_boot.c -- a GameCube disc boots without the IPL (XBA-261): the disc's apploader runs on the Gekko as the IPL
 * would run it, then the main DOL starts. See gc_boot.h.
 *
 * The apploader interface (from the IPL's side): the body (size + trailer bytes from disc 0x2460) is loaded at
 * 0x81200000 and its entry called with three pointers it fills with init / main / close; init(report) gets a
 * printf-like callback; main(&dst, &len, &offset) returns non-zero while it wants len bytes of the disc at offset
 * read to dst; close() returns the main DOL's entry. The IPL's calls are made here from C: LR is a return address
 * no code lives at, and the CPU runs until it gets there. */
#include <stdlib.h>
#include <string.h>

#include "gc_di.h"

#define BOOT_RETURN 0x81300000u   /* LR of every call: nothing runs there */
#define BOOT_REPORT 0x81300010u   /* the report callback, served in C */
#define BOOT_VARS 0x81300020u     /* init / main / close pointers, then dst / len / offset */
#define BOOT_STACK 0x816FFFF0u
#define BOOT_TOP 0x81800000u
#define BOOT_STEPS 400000000ull   /* per call: a real apploader runs a few million instructions at most */
#define BOOT_MAX_READS 100000u

static uint32_t ld32(gc_machine *m, uint32_t ea)
{
    return rc_be32(m->h.ram + (ea & 0x01FFFFFFu));
}

static void st32(gc_machine *m, uint32_t ea, uint32_t v)
{
    rc_put32(m->h.ram + (ea & 0x01FFFFFFu), v);
}

/* A NUL-terminated guest string at ea, up to n - 1 bytes. */
static void guest_str(gc_machine *m, uint32_t ea, char *out, size_t n)
{
    size_t i = 0;
    uint32_t pa = ea & 0x3FFFFFFFu;
    while (i + 1 < n && pa + i < GC_RAM_SIZE && m->h.ram[pa + i]) {
        char c = (char)m->h.ram[pa + i];
        out[i++] = c == '\n' || c == '\r' ? ' ' : (c < 0x20 || c > 0x7E) ? '?' : c;
    }
    out[i] = 0;
}

/* The report callback: the format at r3, integer / string arguments from r4. */
static void report(gc_machine *m)
{
    gc_cpu *c = &m->h.cpu;
    char fmt[256], out[512];
    size_t o = 0;
    int arg = 4;
    guest_str(m, c->gpr[3], fmt, sizeof fmt);
    for (const char *p = fmt; *p && o + 1 < sizeof out; p++) {
        if (*p != '%') {
            out[o++] = *p;
            continue;
        }
        char spec[16] = "%";
        size_t k = 1;
        p++;
        while (*p && strchr("0123456789-+ #.l", *p) && k + 2 < sizeof spec) {
            if (*p != 'l')
                spec[k++] = *p;
            p++;
        }
        if (!*p)
            break;
        uint32_t v = arg <= 10 ? c->gpr[arg] : 0;
        char tmp[256];
        if (*p == 's') {
            arg++;
            guest_str(m, v, tmp, sizeof tmp);
            spec[k++] = 's';
            spec[k] = 0;
            snprintf(out + o, sizeof out - o, spec, tmp);
        } else if (strchr("diuxXcp", *p)) {
            arg++;
            spec[k++] = *p == 'p' ? 'x' : *p;
            spec[k] = 0;
            if (*p == 'd' || *p == 'i')
                snprintf(out + o, sizeof out - o, spec, (int)v);
            else
                snprintf(out + o, sizeof out - o, spec, (unsigned)v);
        } else {
            snprintf(out + o, sizeof out - o, "%%%c", *p);
        }
        o += strlen(out + o);
    }
    out[o < sizeof out ? o : sizeof out - 1] = 0;
    fprintf(stderr, "gc: boot: apploader: %s\n", out);
}

/* Calls guest code at fn (arguments already in r3..) until it returns; the result is r3. */
static int call(gc_machine *m, uint32_t fn, const char *what)
{
    gc_cpu *c = &m->h.cpu;
    c->lr = BOOT_RETURN;
    c->pc = fn;
    c->gpr[1] = BOOT_STACK;
    for (uint64_t steps = 0; c->pc != BOOT_RETURN; steps++) {
        if (m->error[0])
            return -1;
        if (c->pc == BOOT_REPORT) {
            report(m);
            c->pc = c->lr;
            continue;
        }
        if (steps >= BOOT_STEPS) {
            gc_stop(m, "apploader %s did not return (pc %08X)", what, c->pc);
            return -1;
        }
        gc_cpu_step(m);
    }
    return m->error[0] ? -1 : 0;
}

/* Bytes of the disc into guest memory at ea (the IPL's DVD reads). */
static int boot_read(gc_machine *m, gc_disc *disc, uint32_t ea, uint32_t len, uint32_t off)
{
    uint32_t pa = ea & 0x3FFFFFFFu;
    if (pa >= GC_RAM_SIZE || len > GC_RAM_SIZE - pa) {
        gc_stop(m, "apploader read of 0x%X bytes to %08X is outside MEM1", len, ea);
        return -1;
    }
    if (gc_disc_read(disc, off, m->h.ram + pa, len)) {
        gc_stop(m, "disc read of 0x%X bytes at 0x%X: %s", len, off, gc_disc_error(disc) ? gc_disc_error(disc) : "?");
        return -1;
    }
    return 0;
}

gc_machine *gc_create_disc(gc_disc *disc, char *why, size_t why_len)
{
    gc_disc_header h;
    uint8_t hdr[0x20];
    char key[48];
    if (gc_disc_header_read(disc, &h) || gc_disc_read(disc, 0, hdr, sizeof hdr)) {
        snprintf(why, why_len, "disc header: %s", gc_disc_error(disc) ? gc_disc_error(disc) : "unreadable");
        return NULL;
    }
    uint32_t body = h.apploader_size + h.apploader_trailer;
    if (!h.apploader_size || body > 0x00100000u || h.apploader_entry < GC_APPLOADER_BASE
        || h.apploader_entry >= GC_APPLOADER_BASE + body) {
        snprintf(why, why_len, "no usable apploader (entry %08X, size 0x%X + 0x%X)", h.apploader_entry,
                 h.apploader_size, h.apploader_trailer);
        return NULL;
    }
    gc_machine *m = gc_alloc_machine(why, why_len);
    if (!m)
        return NULL;
    gc_di *d = gc_di_get(m);
    if (!d) {
        snprintf(why, why_len, "out of memory");
        gc_destroy(m);
        return NULL;
    }
    gc_disc_key_from_header(hdr, key, sizeof key);
    fprintf(stderr, "gc: boot: disc %s \"%s\" (%s image), apploader %s, entry %08X, 0x%X bytes\n", key, h.name,
            gc_disc_format(disc), h.apploader_date, h.apploader_entry, body);

    /* Low memory as the IPL leaves it before the apploader (gc_alloc_machine set boot magic, sizes, clocks). */
    uint8_t *lo = m->h.ram;
    memcpy(lo, hdr, sizeof hdr);                           /* game code, maker, disc, revision, streaming */
    rc_put32(lo + 0x30, 0);                                /* arena low: the DOL's own */
    rc_put32(lo + 0x34, BOOT_TOP);                         /* arena high */
    rc_put32(lo + 0xCC, hdr[3] == 'P' ? 1 : 0);            /* TV mode: PAL for European discs, else NTSC */
    st32(m, BOOT_RETURN, 0x48000000u);                     /* b . : nothing should run there */
    st32(m, BOOT_REPORT, 0x4E800020u);                     /* blr */

    gc_cpu *c = &m->h.cpu;
    c->msr &= ~PPC_MSR_EE;
    if (boot_read(m, disc, GC_APPLOADER_BASE, body, GC_APPLOADER_OFF + 0x20))
        goto fail;
    c->gpr[3] = BOOT_VARS;
    c->gpr[4] = BOOT_VARS + 4;
    c->gpr[5] = BOOT_VARS + 8;
    if (call(m, h.apploader_entry, "entry"))
        goto fail;
    uint32_t f_init = ld32(m, BOOT_VARS), f_main = ld32(m, BOOT_VARS + 4), f_close = ld32(m, BOOT_VARS + 8);
    c->gpr[3] = BOOT_REPORT;
    if (call(m, f_init, "init"))
        goto fail;
    for (;;) {
        c->gpr[3] = BOOT_VARS + 12;
        c->gpr[4] = BOOT_VARS + 16;
        c->gpr[5] = BOOT_VARS + 20;
        if (call(m, f_main, "main"))
            goto fail;
        if (!c->gpr[3])
            break;
        uint32_t dst = ld32(m, BOOT_VARS + 12), len = ld32(m, BOOT_VARS + 16), off = ld32(m, BOOT_VARS + 20);
        if (m->verbose)
            fprintf(stderr, "gc: boot: apploader reads 0x%X bytes at disc 0x%X to %08X\n", len, off, dst);
        if (len && boot_read(m, disc, dst, len, off))
            goto fail;
        if (++d->boot_reads > BOOT_MAX_READS) {
            gc_stop(m, "apploader keeps asking for reads");
            goto fail;
        }
    }
    if (call(m, f_close, "close"))
        goto fail;
    uint32_t entry = c->gpr[3];
    if ((entry & 0x3FFFFFFFu) >= GC_RAM_SIZE || !entry) {
        gc_stop(m, "apploader close returned entry %08X", entry);
        goto fail;
    }

    /* What the IPL does when the apploader did not: the FST under the top of memory, bi2 under it, arena high. */
    uint32_t fst = rc_be32(lo + 0x38);
    if (!fst && h.fst_size) {
        uint32_t max = h.fst_max > h.fst_size ? h.fst_max : h.fst_size;
        fst = (BOOT_TOP - max) & ~31u;
        if (boot_read(m, disc, fst, h.fst_size, h.fst_off))
            goto fail;
        rc_put32(lo + 0x38, fst);
        rc_put32(lo + 0x3C, max);
    }
    if (!rc_be32(lo + 0xF4)) {
        uint32_t top = fst ? fst : BOOT_TOP;
        uint32_t arena = rc_be32(lo + 0x34);
        if (arena && arena < top)
            top = arena;
        uint32_t bi2 = (top - 0x2000) & ~31u;
        if (boot_read(m, disc, bi2, 0x2000, 0x440))
            goto fail;
        rc_put32(lo + 0xF4, bi2);
        rc_put32(lo + 0x34, bi2);
    } else if (fst && rc_be32(lo + 0x34) > fst) {
        rc_put32(lo + 0x34, fst);
    }

    /* The main DOL's text sections (from its header on the disc), for the recompiled code's coverage. */
    uint8_t dh[0x100];
    if (gc_disc_read(disc, h.dol_off, dh, sizeof dh) == 0) {
        for (int i = 0; i < 7; i++) {
            uint32_t addr = rc_be32(dh + 0x48 + 4 * i), len = rc_be32(dh + 0x90 + 4 * i);
            if (len && m->ntext < 8) {
                m->text_lo[m->ntext] = addr;
                m->text_hi[m->ntext] = addr + len;
                m->ntext++;
            }
        }
    }

    fprintf(stderr, "gc: boot: apploader done (%u reads), FST at %08X, arena high %08X; main DOL entry %08X\n",
            d->boot_reads, rc_be32(lo + 0x38), rc_be32(lo + 0x34), entry);
    /* The apploader ran outside guest time: the application starts at cycle 0, field 0. */
    memset(c->gpr, 0, sizeof c->gpr);
    c->msr = PPC_MSR_FP | PPC_MSR_IR | PPC_MSR_DR | PPC_MSR_RI | PPC_MSR_ME;
    c->lr = 0;
    c->pc = entry;
    m->h.now = 0;
    m->trace_at = 0;
    memset(m->trace, 0, sizeof m->trace);
    d->boot_entry = entry;
    d->disc = disc;
    d->cover_open = false;
    d->reg[DI_CVR] = 0;
    if (hdr[3] == 'P')
        gc_set_tv_mode(m, GC_TV_PAL); /* the video interface as the IPL leaves it for a European disc */
    gc_schedule(m);
    return m;

fail:
    snprintf(why, why_len, "%s", m->error[0] ? m->error : "boot failed");
    d->disc = NULL; /* left to the caller */
    gc_destroy(m);
    return NULL;
}
