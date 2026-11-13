/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * No DSP56300 runs here: the GP's effects image is not executed and the EP
 * outputs PCM stereo: the VP's mixbins are summed to stereo, and what the
 * title asks of the DSPs is recorded (apu_dsp_watch.c). The handshakes DirectSound waits on are answered
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
#include "fpconv.h"

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

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

/* What the title asks of the DSPs is recorded in apu_dsp_watch.c (this
 * project's own code), from the GP/EP memory in the aperture. */
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
    apu_dsp_watch_reset();

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
    /* Bypass DSP: the bins are summed to stereo and written directly to the
     * monitor frame buffer as the final EP output.
     *
     * Bins 2..31 used to be dropped on the floor: only bins 0/1 were read.
     * Measured on Jet Set Radio Future, one 200 s gameplay run:
     *
     *     [APU-BIN] 2D heard=557466 lost=0
     *               3D heard=0      lost=377768
     *               lost by bin: 6,7,8,9,10
     *
     * The title's 3D positional voices -- its sound effects -- are routed
     * to bins 6 to 10 by the guest's own V0BIN..V3BIN, music on 2D voices
     * to bins 0 and 1, which is why the music was always audible and no
     * effect ever was. Even bins left, odd bins right, which preserves the
     * stereo pairing the guest set up. This is not what a real EP does; it
     * is the cheapest mixdown that stops discarding audio. */
    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    apu_dsp_watch_frame((const float (*)[32])mixbins, dsp_aperture());

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float left, right;
            if (mcpx_apu_mixdown_all()) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
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
