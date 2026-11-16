/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * No DSP56300 runs here: the GP's effects image is not executed and the EP
 * outputs PCM stereo. The VP's mixbins are mixed down by role
 * (apu_mix.c), optionally through a native I3DL2 reverb fed from the
 * I3DL2 send, and what the title asks of the DSPs is recorded
 * (apu_dsp_watch.c). The handshakes DirectSound waits on are answered
 * below. docs/technical/apu-dsp.md: why no DSP56300 (xemu's interpreter is
 * GPL, this file and the rest of src/apu are LGPL), the handshake list.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "apu_dsp_watch.h"
#include "apu_mix.h"
#include "fpconv.h"
#include "apu_savestate.h"   /* save states (XBA-248) */

#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * RECOMP_APU_DSP_ACK=auto, the default when the variable is unset, finds the
 * doorbell from what the title programs; "0" or "off" turns the ack off.
 * The XDK DSOUND GP program takes its commands through the first page of
 * the GP's scratch space: a 2 KB command FIFO, then a header whose dword at
 * +0x810 is the doorbell the host writes and the DSP program clears (the
 * DSOUND submit of every XDK title seen spins on <block>+0x810). The
 * scratch pages are what NV_PAPU_GPSADDR's scatter-gather table maps -- 8
 * bytes an entry, the physical page in the first dword, as xemu's apu.c
 * scratch_rw walks it -- so the doorbell is SGE[0] + 0x810 wherever the
 * contiguous allocator put the block. A fixed address moves with allocation
 * order (reserving physical page 0 moved one title's by a page); this does
 * not. Wreckless fits too: its doorbell 0x014F8810 is the first page of the
 * scratch its GPSADDR table (0x01504000) maps, not the table itself, which
 * is where the earlier note here looked.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 */
#define APU_DSP_ACK_MAX 8
#define GP_DOORBELL_OFFSET 0x810u
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;
static int s_dsp_ack_auto;                /* =auto: derive from GPSADDR */
static uint32_t s_dsp_ack_auto_page;      /* the page last reported */
static volatile int s_gp_in_reset;        /* last GPRST write held the GP */
static uint32_t s_gprst_seen;             /* aperture GPRST word last polled */
static int s_gprst_poll = -1;             /* RECOMP_APU_GPRST_POLL, -1: unread */

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        spec = "auto";                    /* default: derive it */
    if (!strcmp(spec, "0") || !strcmp(spec, "off"))
        return;
    if (!strcmp(spec, "auto")) {
        s_dsp_ack_auto = 1;
        fprintf(stderr, "[APU] DSP doorbell ack: auto (GP scratch page 0"
                        " + 0x%X, from GPSADDR)\n", GP_DOORBELL_OFFSET);
        return;
    }
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* The mixdown and the native reverb live in apu_mix.c (the bins by role,
 * RECOMP_APU_MIXDOWN; RECOMP_APU_MIXDOWN_ALL=0 still means bins 0/1 only),
 * the observation in apu_dsp_watch.c. Both are this project's own code.
 *
 * The reverb is allocated here, at init, never on the audio thread. With
 * RECOMP_APU_REVERB_IMAGES=<hash>[,<hash>...] it runs only while the GP
 * holds one of the listed effects images (the hashes the [APU-DSP] lines
 * and the status file print); any other image is bypassed and counted. */
#define REVERB_IMAGES_MAX 8
static ApuReverb *s_reverb;
static uint64_t s_reverb_images[REVERB_IMAGES_MAX];
static int s_reverb_image_count;
static void *s_aperture_override;          /* tests */
extern void *xbox_GetMcpxAperture(void);

void mcpx_apu_dsp_set_aperture(void *ap)
{
    s_aperture_override = ap;
}

static const uint8_t *dsp_aperture(void)
{
    return (const uint8_t *)(s_aperture_override ? s_aperture_override
                                                 : xbox_GetMcpxAperture());
}

static void reverb_init(void)
{
    ApuI3dl2 p;
    const char *name = NULL, *list;

    apu_mix_reset_config();
    s_reverb_image_count = 0;
    if (!apu_mix_reverb_config(&p, &name))
        return;
    if (!s_reverb)
        s_reverb = apu_reverb_create();
    if (!s_reverb)
        return;
    apu_reverb_set(s_reverb, &p);
    apu_reverb_clear(s_reverb);
    list = getenv("RECOMP_APU_REVERB_IMAGES");
    while (list && *list && s_reverb_image_count < REVERB_IMAGES_MAX) {
        char *end;
        unsigned long long h = strtoull(list, &end, 16);
        if (end == list)
            break;
        if (h)
            s_reverb_images[s_reverb_image_count++] = (uint64_t)h;
        list = (*end == ',') ? end + 1 : end;
    }
    fprintf(stderr, "[APU-MIX] native I3DL2 reverb on: preset %s, fed from the"
                    " I3DL2 send (bin 10); %s\n", name,
            s_reverb_image_count ? "only under the listed GP images"
                                 : "under any GP image");
}

/* The reverb for this frame, or NULL: off, or the GP image is not listed. */
static ApuReverb *reverb_for_frame(void)
{
    uint64_t h;
    int i;

    if (!s_reverb)
        return NULL;
    if (s_reverb_image_count) {
        h = apu_dsp_watch_stats()->gp_image_hash;
        for (i = 0; i < s_reverb_image_count; i++)
            if (s_reverb_images[i] == h)
                break;
        if (i == s_reverb_image_count) {
            apu_dsp_watch_reverb(0);
            return NULL;
        }
    }
    apu_dsp_watch_reverb(1);
    return s_reverb;
}

/* GPRST (GP region + 0xFFFC): the GP runs only with both reset bits set, as
 * in xemu's gp_write. Nothing else models the GP, so this is kept for the
 * =auto ack alone. Before the first write the GP counts as running: a title
 * that never touches GPRST keeps its ack. */
void mcpx_apu_dsp_gprst_write(uint32_t val)
{
    const uint32_t run = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    int held = (val & run) != run;

    if (held != s_gp_in_reset && s_dsp_ack_auto)
        fprintf(stderr, "[APU] GPRST 0x%X: GP %s; DSP doorbell ack %s\n",
                val, held ? "held in reset" : "running",
                held ? "paused" : "resumed");
    s_gp_in_reset = held;
}

/* The doorbell for =auto: SGE[0] of the GP scratch table + 0x810, re-read on
 * every poll, so the slot always lies in the page the table names now. NULL
 * while GPSADDR is unset, the table's first entry names no page, or the GP
 * is held in reset: a title that tears DirectSound down frees the scratch,
 * and a dword cleared there after that would land in whatever reuses the
 * page. */
static uint32_t *dsp_ack_auto_slot(MCPXAPUState *d)
{
    uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]), page;

    if (!sge)
        return NULL;
    page = *(const uint32_t *)mcpx_apu_phys(sge);
    if (!page || (page & 0xFFF))
        return NULL;                       /* table not filled in yet */
    if (s_gp_in_reset) {
        /* Not answered, but counted: a title that rings while it holds the
         * GP in reset waits on hardware too -- this says how long. The page
         * is only read, and only the page the table names now. */
        uint32_t *slot = (uint32_t *)(mcpx_apu_phys(page) + GP_DOORBELL_OFFSET);
        apu_dsp_watch_pending(*slot ? APU_DSP_PEND_RESET : -1,
                              page + GP_DOORBELL_OFFSET);
        return NULL;
    }
    if (page != s_dsp_ack_auto_page) {
        s_dsp_ack_auto_page = page;
        fprintf(stderr, "[APU] DSP doorbell: GP scratch SGE table 0x%08X maps"
                        " page 0x%08X (%s); doorbell at physical 0x%08X\n",
                sge, page, mcpx_apu_phys_class(page),
                page + GP_DOORBELL_OFFSET);
    }
    return (uint32_t *)(mcpx_apu_phys(page) + GP_DOORBELL_OFFSET);
}

/* ── The EP program's canary ─────────────────────────────────────────────
 *
 * DirectSound checks that the encode processor is alive by reading a word of
 * its P memory: the XDK EP program keeps 0x00CCCCCC at P:6, which is APU
 * aperture offset 0x5A018 (Xbox VA 0xFE85A018). Fable's DSOUND (DoWork,
 * sub_0082CAF4) ends with
 *
 *     if (*(uint32_t *)0xFE85A018 != 0x00CCCCCC) sub_0082C8B4();
 *
 * and sub_0082C8B4 resets the whole APU: two AC'97 channels, the GP and EP
 * reset registers, the front end, then KeStallExecutionProcessor(10000).
 * With no EP program running the word stays 0, so every DoWork reset the
 * APU: 781 resets in 45 s of movies, 10 ms each, movies at 21-28 fps. With
 * the word in place: no resets, 30 fps.
 *
 * The EP memory is plain RAM in the aperture (only 0x00000-0x2FFFF is
 * trapped), so the read never reaches the model: the word is kept there
 * every APU frame, which also restores it after the title rewrites EP
 * memory. mcpx_apu_dsp_mem_read answers it for a host that traps the
 * read instead. RECOMP_APU_EP_CANARY=0 leaves the word alone.
 *
 * ponytail: a canary, not an EP. Anything the title reads back from EP
 * memory beyond this word is still what it last wrote there. */
#define EP_CANARY_OFFSET 0x5A018u
#define EP_CANARY_VALUE  0x00CCCCCCu

static int ep_canary_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_EP_CANARY");
        on = (e && *e) ? (atoi(e) != 0) : 1;
        if (on)
            fprintf(stderr, "[APU] EP canary: 0x%08X kept at 0x%08X\n",
                    EP_CANARY_VALUE, 0xFE800000u + EP_CANARY_OFFSET);
    }
    return on;
}

static void ep_canary_poll(void)
{
    char *ap;
    volatile uint32_t *w;

    if (!ep_canary_on() || !(ap = (char *)dsp_aperture()))
        return;
    w = (volatile uint32_t *)(ap + EP_CANARY_OFFSET);
    if (*w != EP_CANARY_VALUE)
        *w = EP_CANARY_VALUE;
}

/* ── GPRST as the title writes it ────────────────────────────────────────
 *
 * GPRST is the last word of the GP window (aperture 0x3FFFC, VA 0xFE83FFFC).
 * The window is plain memory (only 0x00000-0x2FFFF is trapped, see the EP
 * canary above and xbox_memory_layout.c), so a title's store there never
 * reaches mcpx_apu_dsp_gprst_write: on a real run the GP-in-reset pause of
 * the ack never triggered (XBA-138). Trapping the page instead would trap
 * the GP P memory sharing it, which DirectSound bulk-copies with a host
 * memcpy that cannot be resumed after a fault.
 *
 * So the word is read back from the aperture on every poll, before the
 * doorbell, and a change is handed to mcpx_apu_dsp_gprst_write as the
 * trapped write would be. Granularity is one APU frame: a hold and release
 * between two polls is not seen, which is harmless for the ack (the title
 * cannot ring and be answered in between). The aperture starts zeroed, so a
 * first store of 0 is not a change: the GP keeps counting as running until
 * the title writes something else, as with no GPRST write at all.
 * RECOMP_APU_GPRST_POLL=0 turns the poll off (the ack before XBA-138). */
static void gprst_poll(void)
{
    const uint8_t *ap;
    uint32_t v;

    if (s_gprst_poll < 0) {
        const char *e = getenv("RECOMP_APU_GPRST_POLL");
        s_gprst_poll = (e && *e) ? (atoi(e) != 0) : 1;
    }
    if (!s_gprst_poll || !(ap = dsp_aperture()))
        return;
    v = *(const volatile uint32_t *)(ap + 0x30000u + NV_PAPU_GPRST);
    if (v != s_gprst_seen) {
        s_gprst_seen = v;
        mcpx_apu_dsp_gprst_write(v);
    }
}

int mcpx_apu_dsp_mem_read(uint64_t addr, uint64_t *val)
{
    if (addr == EP_CANARY_OFFSET && ep_canary_on()) {
        *val = EP_CANARY_VALUE;
        return 1;
    }
    return 0;
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    ep_canary_poll();
    gprst_poll();
    if (!d->ram_ptr)
        return;
    if (s_dsp_ack_auto) {
        uint32_t *slot = dsp_ack_auto_slot(d);
        if (slot) {
            apu_dsp_watch_pending(-1, 0);
            if (*slot) {
                static int shown;
                if (shown++ < 3)
                    fprintf(stderr, "[APU] DSP doorbell (auto): command 0x%08X"
                                    " acknowledged\n", *slot);
                apu_dsp_watch_cmd(s_dsp_ack_auto_page + GP_DOORBELL_OFFSET, *slot);
                *slot = 0;
            }
        }
    }
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            apu_dsp_watch_cmd(s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* The doorbell ack's state is file-static, so it outlives an instance:
     * a GP the last instance held in reset would leave this one's ack
     * paused, and the RECOMP_APU_DSP_ACK setting would never be read again.
     * Start from what a fresh process has. */
    s_dsp_ack_count = -1;                  /* re-read on the first poll */
    s_dsp_ack_auto = 0;
    s_dsp_ack_auto_page = 0;
    s_gp_in_reset = 0;
    s_gprst_seen = 0;
    s_gprst_poll = -1;
    apu_dsp_watch_reset();
    reverb_init();

    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

/* Called after the frame thread is joined. Only mcpx_apu_reset_locked
 * (currently uncalled) reads this state, and it would run on that thread.
 * Safe to call twice. */
void mcpx_apu_dsp_finalize(MCPXAPUState *d)
{
    free(d->gp.dsp);
    free(d->ep.dsp);
    d->gp.dsp = NULL;
    d->ep.dsp = NULL;
    apu_reverb_destroy(s_reverb);
    s_reverb = NULL;
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* No GP or EP runs: the bins are mixed down to stereo here (apu_mix.c)
     * and written to the monitor frame buffer as the EP's output, a PCM
     * stereo stream whatever EP image the title loaded.
     *
     * Bins 2..31 used to be dropped on the floor: only bins 0/1 were read.
     * Measured on Jet Set Radio Future, one 200 s gameplay run:
     *
     *     [APU-BIN] 2D heard=557466 lost=0
     *               3D heard=0      lost=377768
     *               lost by bin: 6,7,8,9,10
     *
     * The title's 3D positional voices -- its sound effects -- are routed
     * to bins 6 to 10 by the guest's own V0BIN..V3BIN (the XTLK pairs and
     * the I3DL2 send), music on 2D voices to bins 0 and 1, which is why the
     * music was always audible and no effect ever was. The fix then summed
     * every bin, even left / odd right (RECOMP_APU_MIXDOWN=sum), which put
     * the centre in the left channel only, the LFE and the I3DL2 send on one
     * side each. The routed mixdown (XBA-122, the default) folds each bin by
     * its DirectSound role; apu_mix.c has the coefficients. */
    float out[NUM_SAMPLES_PER_FRAME][2];
    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    apu_dsp_watch_frame((const float (*)[32])mixbins, dsp_aperture());

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        apu_mix_frame(apu_mix_mode(), (const float (*)[APU_MIX_SAMPLES])mixbins,
                      reverb_for_frame(), out);
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float left = out[i][0], right = out[i][1];
            /* Clamp to [-1, 1] range */
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}

/* ---- save states (XBA-248, apu_savestate.h) ----------------------------
 * The GP and EP windows of the MCPX aperture are the DSPs' X/Y/P memories,
 * plain memory DirectSound fills (effects images, mixbin routing); run-length
 * coded, they are mostly zero. The doorbell ack's page and the GP's reset
 * state go with them. The reverb's delay lines are host-side and start
 * silent after a load. */
#define DSP_WIN_START 0x30000u
#define DSP_WIN_END   0x80000u
#define DSP_STATE_MAGIC 0x31505344u          /* "DSP1" */

void mcpx_apu_dsp_state_save(XdsWriter *w)
{
    const uint8_t *ap = dsp_aperture();

    xds_put_u32(w, DSP_STATE_MAGIC);
    xds_put_u32(w, (uint32_t)s_gp_in_reset);
    xds_put_u32(w, s_gprst_seen);
    xds_put_u32(w, s_dsp_ack_auto_page);
    xds_put_u32(w, ap ? 1u : 0u);
    if (ap)
        xds_put_words_rle(w, (const volatile uint32_t *)(ap + DSP_WIN_START),
                          (DSP_WIN_END - DSP_WIN_START) / 4);
}

int mcpx_apu_dsp_state_load(XdsReader *r)
{
    uint8_t *ap = (uint8_t *)dsp_aperture();

    if (xds_get_u32(r) != DSP_STATE_MAGIC) {
        xds_fail(r);
        return 0;
    }
    s_gp_in_reset = (int)xds_get_u32(r);
    s_gprst_seen = xds_get_u32(r);
    s_dsp_ack_auto_page = xds_get_u32(r);
    if (xds_get_u32(r)) {
        if (!ap) {
            xds_fail(r);
            return 0;
        }
        xds_get_words_rle(r, (volatile uint32_t *)(ap + DSP_WIN_START),
                          (DSP_WIN_END - DSP_WIN_START) / 4);
    }
    if (s_reverb)
        apu_reverb_clear(s_reverb);
    return !r->err;
}
