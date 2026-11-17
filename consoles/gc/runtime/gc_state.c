/* gc_state.c -- save states of the GameCube machine (gc_core.h « Save states », XBA-264).
 *
 * The blob: a header (magic "XRSTATE\0", console "gc", GC_STATE_VERSION, the title key, the section count), then
 * sections, each a tag (four characters), a section version, a length and its bytes. Every field is written one by
 * one in little-endian order (no struct is copied), so a state does not depend on the build's struct layout. One
 * function per section walks its fields for the size, the save and the load alike (io_* below).
 *
 * Sections (required ones refuse the state when missing; optional ones are reset or kept when missing; a section
 * this build does not know is skipped, so a newer build may add one without a format break; a section whose version
 * is newer than this build's refuses the state):
 *   CPU   required  the Gekko: GPRs, FPRs (ps0 / ps1), PC, LR, CTR, CR, XER, MSR, FPSCR, the reservation, SRs, the whole
 *                   SPR file (SRR0/1, GQRs, HID0/1/2/4, WPAR, DMAU/L, BATs, SPRGs, DAR, DSISR, L2CR, ...), the clock
 *   TIME  required  video timing (line, field, PAL / NTSC), time base and decrementer, the interpreter's flags
 *   HW    required  PI (INTSR / INTMR / its registers), VI, SI (and the pads plugged in), EXI registers, MI
 *   MEM1  required  the 24 MB of MEM1: a bitmap of the 4 KB pages stored (not all zero), then those pages
 *   LC    optional  the locked L1 cache (16 KB at 0xE0000000)
 *   ARAM  optional  the 16 MB of ARAM, pages as MEM1
 *   GX    optional  the GPU: the PI FIFO, CP / PE registers, the bytes read from the FIFO not yet parsed, BP / CP / XF
 *                   register files, XF memory (matrices, lights), TEV colour registers, TMEM (pages as MEM1)
 *   DI    optional  the DVD interface: registers, the command in flight, the head position, cover state, streaming
 *                   (not the disc: the run's disc stays in the drive)
 *   EXI   optional  the EXI devices' transfer state, as gc_exi.c saves it (memory cards' commands and status, the
 *                   IPL chip, SRAM, the RTC offset); a memory card's file content is never in a state
 *   DSP   optional  the sound hardware (XBA-263, XBA-266; version 2: field by field): DSP interface and mailboxes,
 *                   the microcodes' HLE state (AX, ASND, Zelda, AESND), ARAM / audio DMA in flight, the AI and its
 *                   streaming counter, the resamplers (see sec_dsp)
 *
 * Not state: host resources (GL objects, the renderer's caches, the disc handle, the attached pack, the dlopen'd
 * table), statistics, the trace of interpreted PCs, the discovery record. After a load: every recompiled span is
 * checked again before it runs (memory may hold other code), the BAT fast paths and the next event are recomputed,
 * GX's texture cache is checked against RAM again and the renderer drops its EFB copies and XFB pictures
 * (gx_gl_state_loaded).
 *
 * The EFB is not in a state (the robust minimum): it lives in the renderer's framebuffer at the internal resolution,
 * and reading it back (and drawing it in again) would tie a state to a GL context. After a load the EFB holds what the
 * renderer last drew; games clear and redraw it every frame, so the first field after the load is right again. The
 * picture shown meanwhile is the XFB in MEM1 (restored), the frame the game had finished when the state was taken.
 * A state taken without a renderer loads with one, and the other way round. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "gc_di.h"
#include "gc_dsp_internal.h"
#include "gc_exi.h"
#include "gc_internal.h"
#include "gx/gx_internal.h"

#define STATE_MAGIC "XRSTATE\0"
#define HEADER_BYTES 72u /* magic 8, console 4, version 4, key 48, sections 4, reserved 4 */
#define KEY_BYTES 48u
#define PAGE 4096u

enum { REQUIRED = 1, FIXED = 2 }; /* FIXED: its length depends on nothing but the build */

/* ---- Field walker: one function per section counts, writes or reads its fields ---------------------------------- */

typedef struct io {
    uint8_t *out;       /* save: the buffer (NULL: count only) */
    const uint8_t *in;  /* load */
    size_t at, cap;
    int load;
    int dry;            /* load: check only (what the section reads fits its length), nothing changes */
    int bad;            /* load: ran past the section, or a value out of range */
    uint32_t ver;       /* load: the section's version in the state (a walk may read older layouts) */
} io;

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void io_bytes(io *s, void *p, size_t n)
{
    if (s->load) {
        if (s->bad || n > s->cap - s->at) {
            s->bad = 1;
            return;
        }
        if (!s->dry)
            memcpy(p, s->in + s->at, n);
    } else if (s->out) {
        memcpy(s->out + s->at, p, n);
    }
    s->at += n;
}

static void io_u32(io *s, uint32_t *v)
{
    uint8_t b[4];
    if (!s->load)
        put32(b, *v);
    if (s->load) { /* into b even when dry: io_get32 reads values the walk needs */
        if (s->bad || 4 > s->cap - s->at) {
            s->bad = 1;
            return;
        }
        memcpy(b, s->in + s->at, 4);
        s->at += 4;
        if (!s->dry)
            *v = get32(b);
        return;
    }
    io_bytes(s, b, 4);
}

/* A value the walk itself needs (a length), read even in a dry run: into a local, never into the machine. */
static uint32_t io_get32(io *s, uint32_t *local)
{
    int dry = s->dry;
    s->dry = 0;
    io_u32(s, local);
    s->dry = dry;
    return *local;
}

static void io_u64(io *s, uint64_t *v)
{
    uint32_t lo = (uint32_t)*v, hi = (uint32_t)(*v >> 32);
    io_u32(s, &lo);
    io_u32(s, &hi);
    if (s->load && !s->bad && !s->dry)
        *v = (uint64_t)hi << 32 | lo;
}

static void io_u16(io *s, uint16_t *v)
{
    uint32_t x = *v;
    io_u32(s, &x);
    if (s->load && !s->bad && !s->dry)
        *v = (uint16_t)x;
}

static void io_u8(io *s, uint8_t *v)
{
    io_bytes(s, v, 1); /* not written when dry */
}

static void io_bool(io *s, bool *v)
{
    uint8_t x = *v ? 1 : 0;
    int dry = s->dry;
    s->dry = 0;
    io_u8(s, &x);
    s->dry = dry;
    if (s->load && !s->bad && !s->dry)
        *v = x != 0;
}

static void io_f64(io *s, double *v)
{
    uint64_t x;
    memcpy(&x, v, 8);
    io_u64(s, &x);
    if (s->load && !s->bad && !s->dry)
        memcpy(v, &x, 8);
}

static void io_u32s(io *s, uint32_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        io_u32(s, &v[i]);
}

static void io_u16s(io *s, uint16_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        io_u16(s, &v[i]);
}

/* A memory block: a bitmap of its 4 KB pages (bit set: stored, not all zero), then the stored pages. Loading checks
 * the bitmap against the section's length before it writes anything (pages_check). */
static int page_zero(const uint8_t *p)
{
    static const uint8_t zero[PAGE];
    return memcmp(p, zero, PAGE) == 0;
}

static void io_pages(io *s, uint8_t *mem, uint32_t size)
{
    uint32_t pages = size / PAGE, head = (pages + 7) / 8;
    if (s->load) {
        if (s->bad || head > s->cap - s->at) {
            s->bad = 1;
            return;
        }
        const uint8_t *bits = s->in + s->at;
        size_t stored = 0;
        for (uint32_t i = 0; i < pages; i++)
            stored += (bits[i >> 3] >> (i & 7)) & 1;
        if (stored * PAGE > s->cap - s->at - head) {
            s->bad = 1;
            return;
        }
        const uint8_t *p = bits + head;
        for (uint32_t i = 0; i < pages && !s->dry; i++) {
            uint8_t *dst = mem + (size_t)i * PAGE;
            if ((bits[i >> 3] >> (i & 7)) & 1) {
                memcpy(dst, p, PAGE);
                p += PAGE;
            } else {
                memset(dst, 0, PAGE);
            }
        }
        s->at += head + stored * PAGE;
        return;
    }
    uint8_t *bits = s->out ? s->out + s->at : NULL;
    if (bits)
        memset(bits, 0, head);
    size_t n = head;
    for (uint32_t i = 0; i < pages; i++) {
        const uint8_t *src = mem + (size_t)i * PAGE;
        if (page_zero(src))
            continue;
        if (bits) {
            bits[i >> 3] |= (uint8_t)(1u << (i & 7));
            memcpy(bits + n, src, PAGE);
        }
        n += PAGE;
    }
    s->at += n;
}

/* ---- Sections --------------------------------------------------------------------------------------------------- */

static void sec_cpu(gc_machine *m, io *s)
{
    gc_cpu *c = &m->h.cpu;
    io_u32s(s, c->gpr, 32);
    for (int i = 0; i < 32; i++)
        io_f64(s, &c->ps0[i]);
    for (int i = 0; i < 32; i++)
        io_f64(s, &c->ps1[i]);
    io_u32(s, &c->pc);
    io_u32(s, &c->lr);
    io_u32(s, &c->ctr);
    io_u32(s, &c->cr);
    io_u32(s, &c->xer_so);
    io_u32(s, &c->xer_ov);
    io_u32(s, &c->xer_ca);
    io_u32(s, &c->xer_bc);
    io_u32(s, &c->msr);
    io_u32(s, &c->fpscr);
    io_u32(s, &c->reserve);
    io_u32s(s, c->sr, 16);
    io_u32s(s, c->spr, 1024);
    io_u64(s, &m->h.now);
}

static void sec_time(gc_machine *m, io *s)
{
    io_u64(s, &m->frames);
    io_u64(s, &m->field_end);
    io_u64(s, &m->next_line);
    io_u32(s, &m->line);
    io_u32(s, &m->frame_lines);
    io_u32(s, &m->field_line);
    io_u64(s, &m->line_cycles);
    io_u64(s, &m->tb_base);
    io_u64(s, &m->dec_zero);
    io_u64(s, &m->dec_fire_tick);
    io_u64(s, &m->dec_fire);
    io_bool(s, &m->dec_pending);
    io_bool(s, &m->branched);
}

static void io_pad(io *s, gc_pad_state *p)
{
    io_bool(s, &p->connected);
    io_u16(s, &p->buttons);
    io_u8(s, &p->stick_x);
    io_u8(s, &p->stick_y);
    io_u8(s, &p->cstick_x);
    io_u8(s, &p->cstick_y);
    io_u8(s, &p->trigger_l);
    io_u8(s, &p->trigger_r);
}

static void sec_hw(gc_machine *m, io *s)
{
    io_u32(s, &m->pi_intsr);
    io_u32(s, &m->pi_intmr);
    io_u32s(s, m->pi_reg, 0x40);
    io_u16s(s, m->vi, 0x80);
    io_u32s(s, m->si, 0x40);
    for (int p = 0; p < 4; p++)
        io_pad(s, &m->pad[p]);
    io_u32s(s, m->exi, 15);
    io_u32s(s, m->mi_reg, 0x40);
}

static void sec_mem1(gc_machine *m, io *s)
{
    io_pages(s, m->h.ram, GC_RAM_SIZE);
}

static void sec_lc(gc_machine *m, io *s)
{
    io_bytes(s, m->lc, GC_LC_SIZE);
}

static void sec_aram(gc_machine *m, io *s)
{
    io_pages(s, m->aram, GC_ARAM_SIZE);
}

/* GX's command staging buffer holds bytes read from the FIFO that do not make a whole command yet. */
static void sec_gx(gc_machine *m, io *s)
{
    gx_state *g = m->gx;
    io_u32(s, &g->pi_base);
    io_u32(s, &g->pi_end);
    io_u32(s, &g->pi_wp);
    io_u16(s, &g->cp_sr);
    io_u16(s, &g->cp_cr);
    io_u16(s, &g->cp_clear);
    io_u16s(s, g->cp_raw, 0x40);
    io_u32(s, &g->cp_base);
    io_u32(s, &g->cp_end);
    io_u32(s, &g->cp_hiwm);
    io_u32(s, &g->cp_lowm);
    io_u32(s, &g->cp_dist);
    io_u32(s, &g->cp_wp);
    io_u32(s, &g->cp_rp);
    io_u32(s, &g->cp_bp);
    io_u16(s, &g->cp_token);
    io_u16s(s, g->pe_reg, 0x20);
    io_u16(s, &g->pe_ctrl);
    io_u16(s, &g->pe_token);
    uint32_t len = (uint32_t)g->cmd_len, dl = (uint32_t)g->dl_depth, vat = g->last_vat;
    io_get32(s, &len);
    if (s->load && !s->bad) {
        if (len > s->cap - s->at || len > (64u << 20)) {
            s->bad = 1;
            return;
        }
    }
    if (s->load && !s->bad && !s->dry) {
        if (len > g->cmd_cap) {
            uint8_t *p = realloc(g->cmd, len);
            if (!p) {
                s->bad = 1;
                return;
            }
            g->cmd = p;
            g->cmd_cap = len;
        }
        g->cmd_len = len;
    }
    io_bytes(s, g->cmd, len);
    io_get32(s, &dl);
    io_get32(s, &vat);
    if (s->load && !s->bad && !s->dry) {
        g->dl_depth = (int)dl;
        g->last_vat = vat & 7;
    }
    io_u16s(s, g->vsize, 8);
    io_u32s(s, g->bp, 256);
    io_u32(s, &g->bp_mask);
    io_u32s(s, g->cpr, 256);
    io_u32s(s, g->xf.u, 0x800);
    io_u32s(s, g->xf_reg, 0x100);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) {
            uint16_t v = (uint16_t)g->creg[i][k];
            io_u16(s, &v);
            if (s->load && !s->bad && !s->dry)
                g->creg[i][k] = (int16_t)v;
        }
    io_bytes(s, g->kreg, sizeof g->kreg);
    io_u64(s, &g->field_count);
    io_pages(s, g->tmem, GX_TMEM_SIZE);
    if (s->load && !s->bad && !s->dry) {
        /* Derived state is derived again, never trusted: parse() bounds a draw by vsize while the vertex loader
         * reads by the CP registers, so a vsize that disagrees with them (another build, a damaged file) let the
         * loader read past the command or display list; and a state is taken between fields, never inside a
         * display list, so a non-zero depth would only turn display lists off for good. */
        g->dl_depth = 0;
        for (unsigned v = 0; v < 8; v++)
            if (g->vsize[v])
                g->vsize[v] = (uint16_t)(gx_vertex_size(g, v) + 1);
    }
}

static void sec_di(gc_machine *m, io *s)
{
    gc_di scratch, *d = m->di;
    if (s->load && !s->cap)
        return; /* the state's machine never used its drive */
    if (!d && s->load && s->dry) { /* checking a state's drive before the machine has one */
        memset(&scratch, 0, sizeof scratch);
        d = &scratch;
    }
    if (!d) /* never used: written empty, so a load into a machine with a drive leaves its drive as it is */
        return;
    io_bool(s, &d->cover_open);
    io_u64(s, &d->cover_close_at);
    io_u32s(s, d->reg, DI_NREG);
    io_u64(s, &d->done_at);
    io_u32(s, &d->cmd0);
    io_u32(s, &d->cmd1);
    io_u32(s, &d->cmd2);
    io_u32(s, &d->mar);
    io_u32(s, &d->len);
    io_u32(s, &d->error);
    io_bool(s, &d->fail);
    io_u64(s, &d->head);
    io_bool(s, &d->motor_off);
    io_bool(s, &d->stream_on);
    io_bool(s, &d->stream_stop_at_end);
    io_u64(s, &d->stream_start);
    io_u64(s, &d->stream_len);
    io_u64(s, &d->stream_pos);
    io_u64(s, &d->stream_next_start);
    io_u64(s, &d->stream_next_len);
    io_u32(s, &d->stream_cfg);
    for (int i = 0; i < 56; i++) {
        uint16_t v = (uint16_t)d->stream_blk[i];
        io_u16(s, &v);
        if (s->load && !s->bad && !s->dry)
            d->stream_blk[i] = (int16_t)v;
    }
    uint32_t at = (uint32_t)d->stream_blk_at;
    io_u32(s, &at);
    for (int i = 0; i < 4; i++) {
        uint32_t v = (uint32_t)d->stream_hist[i];
        io_u32(s, &v);
        if (s->load && !s->bad && !s->dry)
            d->stream_hist[i] = (int32_t)v;
    }
    if (s->load && !s->bad && !s->dry) {
        d->stream_blk_at = at > 28 ? 28 : (int)at;
        d->stream_cache_n = 0; /* the disc read cache: read again */
    }
    io_u32(s, &d->boot_entry);
    uint32_t br = d->boot_reads;
    io_u32(s, &br);
    if (s->load && !s->bad && !s->dry)
        d->boot_reads = br;
}

/* A block another file serializes (EXI, the sound hardware): its bytes as they come, length checked by the owner. */
static void io_blob(io *s, size_t n, size_t (*save)(gc_machine *, uint8_t *, size_t), gc_machine *m)
{
    if (s->load) {
        if (s->cap - s->at != n)
            s->bad = 1;
        else
            s->at += n;
        return;
    }
    if (s->out && save(m, s->out + s->at, n) != n)
        memset(s->out + s->at, 0, n);
    s->at += n;
}

/* EXI (gc_exi.c, the memory-card work): the channels' registers and the devices' transfer state (card commands under
 * way, status and interrupt bits, the IPL chip's command, SRAM, the RTC offset), as gc_exi_state_save writes it. A
 * memory card's contents are never part of a state (they are the save files). */
static size_t exi_save(gc_machine *m, uint8_t *out, size_t cap)
{
    return gc_exi_state_save(m, out, cap);
}

static void sec_exi(gc_machine *m, io *s)
{
    size_t start = s->at;
    io_blob(s, m->exidev ? gc_exi_state_size() : 0, exi_save, m);
    if (s->load && !s->bad && s->at > start) {
        /* the dry run checks the blob too: the real load comes after CPU, MEM1, GX..., too late to refuse it */
        if (s->dry ? gc_exi_state_check(s->in + start, s->at - start) != 0
                   : gc_exi_state_load(m, s->in + start, s->at - start) != 0)
            s->bad = 1;
    }
}

/* The sound hardware (gc_dsp.c / gc_ai.c and the microcodes' HLE: gc_ax.c, gc_asnd.c, gc_zelda.c, gc_aesnd.c; XBA-263,
 * XBA-266), field by field: the DSP interface and its mailboxes, the running ucode and the one switched away from
 * (both with their HLE state: AX, ASND, Zelda, AESND), ARAM and audio DMA in flight, the AI and its streaming counter,
 * the DMA's frames waiting for the resampler and both resamplers. Not saved: the 48 kHz ring for the host (emptied at
 * a load: what was queued belongs to the moment before), the statistics (mails, lists, blocks, overruns...) and the
 * "warned once" flags. Version 1 was struct gc_audio copied as it was: a state with it loads without its sound (the
 * hardware is left as it is). Indices are checked against their arrays at a load, so a damaged section is refused
 * before anything changes. */
#define DSP_STATE_VERSION 2

static void io_i16s(io *s, int16_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint16_t x = (uint16_t)v[i];
        io_u16(s, &x);
        if (s->load && !s->bad && !s->dry)
            v[i] = (int16_t)x;
    }
}

static void io_i32s(io *s, int32_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t x = (uint32_t)v[i];
        io_u32(s, &x);
        if (s->load && !s->bad && !s->dry)
            v[i] = (int32_t)x;
    }
}

/* An index or a count: read even in a dry run, refused past max. */
static void io_idx(io *s, unsigned *v, unsigned max)
{
    uint32_t x = *v;
    if (s->load) {
        io_get32(s, &x);
        if (x > max)
            s->bad = 1;
        if (!s->bad && !s->dry)
            *v = x;
        return;
    }
    io_u32(s, &x);
}

static void io_int(io *s, int *v, int max)
{
    unsigned x = (unsigned)*v;
    io_idx(s, &x, (unsigned)max);
    if (s->load && !s->bad && !s->dry)
        *v = (int)x;
}

static void io_ax(io *s, gc_ax *ax)
{
    for (int b = 0; b < 9; b++)
        io_i32s(s, ax->buf[b], GC_AX_SAMPLES);
    io_u16s(s, ax->cmd, GC_AX_CMDLIST_MAX);
    io_u16(s, &ax->cmd_size);
    io_int(s, &ax->state, 2);
    io_u16(s, &ax->compressor_pos);
    io_bool(s, &ax->old);
}

static void io_asnd(io *s, gc_asnd *a)
{
    io_u32(s, &a->voice_addr);
    io_bool(s, &a->next_is_voice);
    io_bool(s, &a->new_flags);
    io_bool(s, &a->swap_lr);
    io_bool(s, &a->has_terminate);
    io_u16s(s, a->in, 32);
    io_u16s(s, a->out, 1024 * 2);
    io_u32s(s, a->v, 16);
}

static void io_zelda(io *s, gc_zelda *z)
{
    io_u32(s, &z->flags);
    io_int(s, &z->state, 3);
    io_u32(s, &z->expect);
    io_u32(s, &z->sync_max_voice);
    io_u16s(s, z->skip, 256);
    io_bool(s, &z->sync_second_half);
    io_u32s(s, z->cmd, 64);
    io_idx(s, &z->rd, 63);
    io_idx(s, &z->wr, 63);
    io_idx(s, &z->pending, 64);
    io_bool(s, &z->can_execute);
    io_u32(s, &z->req_frames);
    io_u32(s, &z->cur_frame);
    io_u32(s, &z->cur_voice);
    io_u16(s, &z->voices_per_frame);
    io_bool(s, &z->prepared);
    io_u32(s, &z->out_l);
    io_u32(s, &z->out_r);
    io_u16(s, &z->out_vol);
    for (int b = 0; b < ZB_COUNT; b++)
        io_i16s(s, z->buf[b], GC_ZELDA_SAMPLES);
    io_u32(s, &z->vpb_base);
    io_u32(s, &z->reverb_base);
    io_i16s(s, z->resample_coefs, 0x100);
    io_i16s(s, z->patterns, 0x100);
    io_i16s(s, z->sine, 0x80);
    io_i16s(s, z->afc_coefs, 0x20);
    io_u16s(s, z->reverb_pos, 4);
    for (int r = 0; r < 4; r++)
        io_i16s(s, z->reverb_last8[r], 8);
    io_u64(s, &z->voices);
    io_u64(s, &z->frames);
}

static void io_aesnd(io *s, gc_aesnd *a)
{
    io_bool(s, &a->next_is_pb);
    io_bool(s, &a->new_flags);
    io_bool(s, &a->swap_lr);
    io_bool(s, &a->fixed_terminate);
    io_u32(s, &a->pb_addr);
    io_u32(s, &a->out_buf);
    io_i16s(s, a->out, GC_AESND_FRAMES * 2);
}

static void io_resampler(io *s, gc_resampler *r)
{
    io_u32(s, &r->in_rate);
    io_u32(s, &r->out_rate);
    io_u32(s, &r->phase);
    for (int k = 0; k < 4; k++)
        io_i32s(s, r->hist[k], 2);
    if (s->load && !s->dry && !s->bad && (!r->in_rate || !r->out_rate)) /* a divisor: never 0 */
        r->in_rate = r->out_rate = 1;
}

static void sec_dsp(gc_machine *m, io *s)
{
    gc_audio *a = m->audio;
    if (s->load && s->ver < DSP_STATE_VERSION) { /* the raw struct of version 1: skipped, the sound kept as it is */
        s->at = s->cap;
        return;
    }
    if (!a) /* no sound hardware: an empty section (and one is skipped) */
        return;
    if (s->load && s->cap == 0)
        return;
    uint32_t ver = DSP_STATE_VERSION;
    io_u32(s, &ver);
    io_u16(s, &a->csr);
    io_u16s(s, a->reg, 0x100);
    io_u32(s, &a->cpu_mail);
    for (int i = 0; i < GC_MAILQ; i++) {
        io_u32(s, &a->q[i].v);
        io_bool(s, &a->q[i].irq_next);
    }
    io_idx(s, &a->qh, GC_MAILQ - 1);
    io_idx(s, &a->qn, GC_MAILQ);
    io_u32(s, &a->last_mail);
    io_u64(s, &a->dspint_at);
    io_u64(s, &a->aidint_at);
    io_u64(s, &a->initcode_clear_at);
    io_u64(s, &a->aram_done_at);
    io_int(s, &a->ucode, UC_UNKNOWN_AX);
    io_u32(s, &a->crc);
    io_idx(s, &a->booted, 0xFFFFFFFFu);
    io_idx(s, &a->unknown, 0xFFFFFFFFu);
    io_u32(s, &a->rom_param);
    io_u32(s, &a->boot_addr);
    io_u32(s, &a->boot_len);
    io_u32(s, &a->boot_imem);
    io_u32(s, &a->boot_dmem_len);
    io_bool(s, &a->uploading);
    io_idx(s, &a->up_step, 9);
    io_u32s(s, a->up, 10);
    io_bool(s, &a->resume_mail);
    io_int(s, &a->prev_ucode, UC_UNKNOWN_AX);
    io_u32(s, &a->prev_crc);
    io_ax(s, &a->ax);
    io_ax(s, &a->prev_ax);
    io_asnd(s, &a->asnd);
    io_asnd(s, &a->prev_asnd);
    io_zelda(s, &a->zelda);
    io_zelda(s, &a->prev_zelda);
    io_aesnd(s, &a->aesnd);
    io_aesnd(s, &a->prev_aesnd);
    io_u32(s, &a->aid_cur);
    io_u32(s, &a->aid_left);
    io_bool(s, &a->aid_on);
    io_u32(s, &a->aicr);
    io_u32(s, &a->aivr);
    io_u32(s, &a->aiit);
    io_u32(s, &a->scnt_base);
    io_u64(s, &a->scnt_t0);
    io_u64(s, &a->aiit_at);
    io_u64(s, &a->tick_at);
    io_u64(s, &a->out_done);
    for (int i = 0; i < GC_AID_FIFO; i++)
        io_i16s(s, a->fifo[i], 2);
    io_idx(s, &a->fh, GC_AID_FIFO - 1);
    io_idx(s, &a->fn, GC_AID_FIFO);
    io_resampler(s, &a->rs_dma);
    io_resampler(s, &a->rs_stream);
    if (s->load && !s->bad && !s->dry)
        a->rh = a->rn = 0;
}

typedef struct section {
    char tag[4];
    uint32_t version;  /* this build's; a state's newer version is refused, an older one is read by the section */
    int flags;
    void (*walk)(gc_machine *m, io *s);
} section;

static const section k_sections[] = {
    {{'C', 'P', 'U', ' '}, 1, REQUIRED | FIXED, sec_cpu},
    {{'T', 'I', 'M', 'E'}, 1, REQUIRED | FIXED, sec_time},
    {{'H', 'W', ' ', ' '}, 1, REQUIRED | FIXED, sec_hw},
    {{'M', 'E', 'M', '1'}, 1, REQUIRED, sec_mem1},
    {{'L', 'C', ' ', ' '}, 1, FIXED, sec_lc},
    {{'A', 'R', 'A', 'M'}, 1, 0, sec_aram},
    {{'G', 'X', ' ', ' '}, 1, 0, sec_gx},
    {{'D', 'I', ' ', ' '}, 1, 0, sec_di},
    {{'E', 'X', 'I', ' '}, 1, 0, sec_exi},
    {{'D', 'S', 'P', ' '}, DSP_STATE_VERSION, 0, sec_dsp},
};
#define NSEC (sizeof k_sections / sizeof k_sections[0])
#define SEC_HEAD 12u /* tag, version, length */

static size_t sec_len(gc_machine *m, const section *k)
{
    io s = {0};
    k->walk(m, &s);
    return s.at;
}

size_t gc_state_size(gc_machine *m)
{
    if (!m)
        return 0;
    size_t n = HEADER_BYTES;
    for (size_t i = 0; i < NSEC; i++)
        n += SEC_HEAD + sec_len(m, &k_sections[i]);
    return n;
}

size_t gc_state_save(gc_machine *m, const char *key, uint8_t *out, size_t cap)
{
    if (!m || !out || m->error[0]) /* a stopped machine has nothing worth resuming */
        return 0;
    gx_gl_flush(m->gx); /* draws queued so far land in the EFB / XFB now */
    size_t need = gc_state_size(m);
    if (cap < need)
        return 0;
    memset(out, 0, HEADER_BYTES);
    memcpy(out, STATE_MAGIC, 8);
    memcpy(out + 8, "gc", 2);
    put32(out + 12, GC_STATE_VERSION);
    snprintf((char *)out + 16, KEY_BYTES, "%s", key ? key : "");
    put32(out + 16 + KEY_BYTES, (uint32_t)NSEC);
    size_t at = HEADER_BYTES;
    for (size_t i = 0; i < NSEC; i++) {
        const section *k = &k_sections[i];
        io s = {0};
        s.out = out + at + SEC_HEAD;
        k->walk(m, &s);
        memcpy(out + at, k->tag, 4);
        put32(out + at + 4, k->version);
        put32(out + at + 8, (uint32_t)s.at);
        at += SEC_HEAD + s.at;
    }
    return at;
}

static int why_set(char *why, size_t why_n, int rc, const char *fmt, const char *a, const char *b)
{
    if (why && why_n)
        snprintf(why, why_n, fmt, a, b);
    return rc;
}

/* Walks the header and the sections, filling at[] / len[] / ver[] per known section (NULL: absent). */
static int parse(gc_machine *m, const char *key, const uint8_t *data, size_t size, const uint8_t *at[NSEC],
                 size_t len[NSEC], uint32_t ver[NSEC], char *why, size_t why_n)
{
    if (!m || !data || size < HEADER_BYTES || memcmp(data, STATE_MAGIC, 8) != 0)
        return why_set(why, why_n, GC_STATE_ECORRUPT, "not a save state%s%s", "", "");
    if (memcmp(data + 8, "gc\0\0", 4) != 0)
        return why_set(why, why_n, GC_STATE_EGAME, "a state of another console (%.4s)%s", (const char *)data + 8,
                       "");
    uint32_t v = get32(data + 12);
    if (v != GC_STATE_VERSION) {
        char a[16], b[16];
        snprintf(a, sizeof a, "%u", v);
        snprintf(b, sizeof b, "%u", GC_STATE_VERSION);
        return why_set(why, why_n, GC_STATE_EVERSION, "state format version %s, this build reads %s", a, b);
    }
    char skey[KEY_BYTES + 1];
    memcpy(skey, data + 16, KEY_BYTES);
    skey[KEY_BYTES] = 0;
    if (key && *key && strcmp(skey, key) != 0)
        return why_set(why, why_n, GC_STATE_EGAME, "a state of another game (%s, not %s)", skey, key);
    uint32_t count = get32(data + 16 + KEY_BYTES);
    for (size_t i = 0; i < NSEC; i++) {
        at[i] = NULL;
        len[i] = 0;
        ver[i] = 0;
    }
    size_t off = HEADER_BYTES;
    for (uint32_t n = 0; n < count; n++) {
        if (size - off < SEC_HEAD)
            return why_set(why, why_n, GC_STATE_ECORRUPT, "truncated state%s%s", "", "");
        const uint8_t *h = data + off;
        uint32_t sv = get32(h + 4), sl = get32(h + 8);
        off += SEC_HEAD;
        if (sl > size - off)
            return why_set(why, why_n, GC_STATE_ECORRUPT, "truncated state (section %.4s)%s", (const char *)h, "");
        for (size_t i = 0; i < NSEC; i++) {
            if (memcmp(h, k_sections[i].tag, 4) != 0)
                continue;
            if (at[i])
                return why_set(why, why_n, GC_STATE_ECORRUPT, "section %.4s twice%s", (const char *)h, "");
            if (sv > k_sections[i].version)
                return why_set(why, why_n, GC_STATE_EVERSION, "section %.4s is newer than this build reads%s",
                               (const char *)h, "");
            at[i] = data + off;
            len[i] = sl;
            ver[i] = sv;
        }
        off += sl; /* a section this build does not know: skipped */
    }
    for (size_t i = 0; i < NSEC; i++)
        if ((k_sections[i].flags & REQUIRED) && !at[i])
            return why_set(why, why_n, GC_STATE_ECORRUPT, "section %.4s missing%s", k_sections[i].tag, "");
    /* Fixed-size sections: their length is this build's (a different one: another layout of the same version). */
    for (size_t i = 0; i < NSEC; i++)
        if (at[i] && (k_sections[i].flags & FIXED) && len[i] != sec_len(m, &k_sections[i]))
            return why_set(why, why_n, GC_STATE_EVERSION, "section %.4s has another size%s", k_sections[i].tag, "");
    return GC_STATE_OK;
}

/* A dry run of a section's walk: what it reads fits its length exactly (nothing changes). */
static int dry_ok(gc_machine *m, const section *k, const uint8_t *p, size_t n, uint32_t ver)
{
    io s = {0};
    s.load = 1;
    s.ver = ver;
    s.dry = 1;
    s.in = p;
    s.cap = n;
    k->walk(m, &s);
    return !s.bad && s.at == n;
}

int gc_state_check(gc_machine *m, const char *key, const uint8_t *data, size_t size, char *why, size_t why_n)
{
    const uint8_t *at[NSEC];
    size_t len[NSEC];
    uint32_t ver[NSEC];
    int rc = parse(m, key, data, size, at, len, ver, why, why_n);
    if (rc != GC_STATE_OK)
        return rc;
    for (size_t i = 0; i < NSEC; i++)
        if (at[i] && !dry_ok(m, &k_sections[i], at[i], len[i], ver[i]))
            return why_set(why, why_n, GC_STATE_ECORRUPT, "section %.4s is damaged%s", k_sections[i].tag, "");
    return GC_STATE_OK;
}

int gc_state_load(gc_machine *m, const char *key, const uint8_t *data, size_t size, char *why, size_t why_n)
{
    int rc = gc_state_check(m, key, data, size, why, why_n);
    if (rc != GC_STATE_OK)
        return rc;
    const uint8_t *at[NSEC];
    size_t len[NSEC];
    uint32_t ver[NSEC];
    parse(m, key, data, size, at, len, ver, NULL, 0);
    gx_gl_flush(m->gx); /* the old state's queued draws, before its registers go */
    m->error[0] = 0;    /* a stopped machine runs again from the state */
    for (size_t i = 0; i < NSEC; i++)
        if (k_sections[i].walk == sec_di && at[i] && len[i] && !gc_di_get(m))
            return why_set(why, why_n, GC_STATE_ECORRUPT, "out of memory%s%s", "", "");
    for (size_t i = 0; i < NSEC; i++) {
        const section *k = &k_sections[i];
        if (!at[i]) {
            /* optional and absent: the locked cache cleared, the rest kept as it is */
            if (k->walk == sec_lc)
                memset(m->lc, 0, GC_LC_SIZE);
            continue;
        }
        if (k->walk == sec_di && !len[i])
            continue;
        io s = {0};
        s.load = 1;
        s.in = at[i];
        s.cap = len[i];
        s.ver = ver[i];
        k->walk(m, &s);
        /* checked before (sizes, page bitmaps): a failure here would be a bug, the machine half loaded */
        if (s.bad)
            snprintf(m->error, sizeof m->error, "save state: section %.4s did not load", k->tag);
    }
    /* What is derived from the state, and what host-side caches must forget. */
    m->fault_armed = false;
    if (!m->frame_lines || !m->line_cycles) { /* not from this core: the loop would spin */
        m->frame_lines = GC_LINES;
        m->line_cycles = GC_CYCLES_PER_LINE;
    }
    gc_mmu_bats(m); /* the BAT fast paths for the loaded BATs and MSR */
    if (m->span_state && m->rc)
        memset(m->span_state, 0, m->rc->nspans ? m->rc->nspans : 1); /* every span checked again */
    m->gx->tex_epoch++; /* textures hashed against RAM again */
    gx_gl_state_loaded(m->gx);
    gc_schedule(m);
    return GC_STATE_OK;
}

/* ---- Cheats' memory access (XBA-264): MEM1, big-endian, side-effect free ----------------------------------------- */

uint32_t gc_cheat_read32(gc_machine *m, uint32_t ea)
{
    return gc_peek32(m, ea & ~3u);
}

void gc_cheat_write32(gc_machine *m, uint32_t ea, uint32_t v)
{
    uint32_t pa = ea & 0x3FFFFFFCu;
    if (pa > GC_RAM_SIZE - 4 || rc_be32(m->h.ram + pa) == v)
        return;
    rc_put32(m->h.ram + pa, v);
    /* code patched under lifted code: its span is checked again, and the interpreter runs it (gc_recomp.c) */
    gc_recomp_icbi(m, 0x80000000u | pa);
}
