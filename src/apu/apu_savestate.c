/*
 * The APU model's save state (XBA-248, kernel/xbox_dev_state.h): what the model holds of the guest's
 * audio, outside guest memory.
 *
 *   - the register files: the APU's own (0x20000 words: front end, sequencer, voice lists, SGE and SSL
 *     bases), the GP's and the EP's;
 *   - the sequencer's sample counter (XGSCNT, the SSL tick);
 *   - the voice processor's host-side state per voice: the linear resampler's two source samples and
 *     position, the state-variable filters, the HRTF filter's history and coefficient ramps, each bin's
 *     gain at the end of the last frame (the ramp's start), the SSL bookkeeping, the HRTF table, the
 *     submix headrooms, and which voices the title holds locked (VOICE_LOCK).
 *
 * The voice descriptors themselves (position, envelopes, state bits) are in guest memory, saved with it.
 * Host-only state is not saved: the frame thread's pacing, the monitor buffer, the voice spin locks, the
 * statistics. The model's pointers (the resampler handle, each filter's output selector) are kept or
 * re-derived, never stored.
 *
 * mcpx_apu_state_capture / _restore (apu_core.c) wrap this with the frame thread held, the GP / EP DSP
 * memories and the host ring.
 */
#include "apu_state.h"
#include "apu_savestate.h"

#define APU_MODEL_MAGIC 0x314C444Du   /* "MDL1" */

/* Which of the filter's taps op points at, so it survives as an index. */
static uint32_t svf_op_index(const sv_filter *sv)
{
    if (sv->op == &sv->l) return 1;
    if (sv->op == &sv->h) return 2;
    if (sv->op == &sv->b) return 3;
    if (sv->op == &sv->n) return 4;
    if (sv->op == &sv->p) return 5;
    return 0;
}

static float *svf_op_ptr(sv_filter *sv, uint32_t i)
{
    switch (i) {
    case 1: return &sv->l;
    case 2: return &sv->h;
    case 3: return &sv->b;
    case 4: return &sv->n;
    case 5: return &sv->p;
    default: return NULL;
    }
}

static void put_svf(XdsWriter *w, const sv_filter *sv)
{
    xds_put_f32(w, sv->f);
    xds_put_f32(w, sv->q);
    xds_put_f32(w, sv->qnrm);
    xds_put_f32(w, sv->h);
    xds_put_f32(w, sv->b);
    xds_put_f32(w, sv->l);
    xds_put_f32(w, sv->p);
    xds_put_f32(w, sv->n);
    xds_put_u32(w, svf_op_index(sv));
}

static void get_svf(XdsReader *r, sv_filter *sv)
{
    sv->f = xds_get_f32(r);
    sv->q = xds_get_f32(r);
    sv->qnrm = xds_get_f32(r);
    sv->h = xds_get_f32(r);
    sv->b = xds_get_f32(r);
    sv->l = xds_get_f32(r);
    sv->p = xds_get_f32(r);
    sv->n = xds_get_f32(r);
    sv->op = svf_op_ptr(sv, xds_get_u32(r));
}

void mcpx_apu_model_save(const MCPXAPUState *d, XdsWriter *w)
{
    const MCPXAPUVPState *vp = &d->vp;
    int v;

    xds_put_u32(w, APU_MODEL_MAGIC);
    xds_put_u32(w, MCPX_HW_MAX_VOICES);
    xds_put_words_rle(w, d->regs, sizeof d->regs / 4);
    xds_put_words_rle(w, d->gp.regs, sizeof d->gp.regs / 4);
    xds_put_words_rle(w, d->ep.regs, sizeof d->ep.regs / 4);
    xds_put_u64(w, d->sample_count);
    xds_put_u32(w, d->set_irq ? 1u : 0u);

    xds_put_u32(w, (uint32_t)vp->ssl_base_page);
    xds_put_blob(w, vp->ssl, sizeof vp->ssl);
    xds_put_u32(w, vp->hrtf_headroom);
    xds_put_blob(w, vp->hrtf_submix, sizeof vp->hrtf_submix);
    xds_put_blob(w, vp->submix_headroom, sizeof vp->submix_headroom);
    xds_put_blob(w, &vp->hrtf, sizeof vp->hrtf);
    xds_put_u32(w, vp->inbuf_sge_handle);
    xds_put_u32(w, vp->outbuf_sge_handle);
    xds_put_blob(w, (const void *)vp->voice_locked, sizeof vp->voice_locked);

    /* The per-voice block is mostly zeros (256 voices, a few playing): built apart, then run-length coded
     * as words. */
    {
    XdsWriter fw = { 0 };
    XdsWriter *wo = w;
    w = &fw;
    for (v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        const MCPXAPUVoiceFilter *f = &vp->filters[v];
        xds_put_u32(w, f->voice);
        xds_put_blob(w, f->resample_buf, sizeof f->resample_buf);
        xds_put_f32(w, f->rs_last[0]);
        xds_put_f32(w, f->rs_last[1]);
        xds_put_f32(w, f->rs_next[0]);
        xds_put_f32(w, f->rs_next[1]);
        xds_put_f32(w, f->rs_pos);
        xds_put_u32(w, (uint32_t)f->rs_have);
        put_svf(w, &f->svf[0]);
        put_svf(w, &f->svf[1]);
        xds_put_blob(w, &f->hrtf, sizeof f->hrtf);
        xds_put_blob(w, f->gain_prev, sizeof f->gain_prev);
        xds_put_u32(w, f->gain_valid ? 1u : 0u);
    }
    w = wo;
    if (fw.err) {
        w->err = 1;
    } else {
        /* Every field above is 4 or 8 bytes: the block is whole words. */
        xds_put_words_rle(w, (const uint32_t *)fw.p, fw.len / 4);
    }
    xds_writer_free(&fw);
    }
}

int mcpx_apu_model_load(MCPXAPUState *d, XdsReader *r)
{
    MCPXAPUVPState *vp = &d->vp;
    int v;

    if (xds_get_u32(r) != APU_MODEL_MAGIC || xds_get_u32(r) != MCPX_HW_MAX_VOICES) {
        xds_fail(r);
        return 0;
    }
    xds_get_words_rle(r, d->regs, sizeof d->regs / 4);
    xds_get_words_rle(r, d->gp.regs, sizeof d->gp.regs / 4);
    xds_get_words_rle(r, d->ep.regs, sizeof d->ep.regs / 4);
    d->sample_count = xds_get_u64(r);
    d->set_irq = xds_get_u32(r) != 0;

    vp->ssl_base_page = (int)xds_get_u32(r);
    xds_get_blob(r, vp->ssl, sizeof vp->ssl);
    vp->hrtf_headroom = (uint8_t)xds_get_u32(r);
    xds_get_blob(r, vp->hrtf_submix, sizeof vp->hrtf_submix);
    xds_get_blob(r, vp->submix_headroom, sizeof vp->submix_headroom);
    xds_get_blob(r, &vp->hrtf, sizeof vp->hrtf);
    vp->inbuf_sge_handle = xds_get_u32(r);
    vp->outbuf_sge_handle = xds_get_u32(r);
    xds_get_blob(r, (void *)vp->voice_locked, sizeof vp->voice_locked);

    /* The per-voice block, decoded into words first (see the save). */
    {
    uint64_t words;
    uint32_t *buf;
    XdsReader fr, *ro = r;
    size_t at = r->pos;

    words = xds_get_u64(r);
    r->pos = at;                       /* xds_get_words_rle reads the count again */
    if (r->err || words > (uint64_t)MCPX_HW_MAX_VOICES * sizeof(MCPXAPUVoiceFilter)) {
        xds_fail(r);
        return 0;
    }
    buf = (uint32_t *)malloc((size_t)words * 4 + 4);
    if (!buf || !xds_get_words_rle(r, buf, (size_t)words)) {
        free(buf);
        xds_fail(r);
        return 0;
    }
    fr = xds_reader(buf, (size_t)words * 4);
    r = &fr;
    for (v = 0; v < MCPX_HW_MAX_VOICES && !r->err; v++) {
        MCPXAPUVoiceFilter *f = &vp->filters[v];
        f->voice = (uint16_t)xds_get_u32(r);
        xds_get_blob(r, f->resample_buf, sizeof f->resample_buf);
        f->rs_last[0] = xds_get_f32(r);
        f->rs_last[1] = xds_get_f32(r);
        f->rs_next[0] = xds_get_f32(r);
        f->rs_next[1] = xds_get_f32(r);
        f->rs_pos = xds_get_f32(r);
        f->rs_have = (int)xds_get_u32(r);
        get_svf(r, &f->svf[0]);
        get_svf(r, &f->svf[1]);
        xds_get_blob(r, &f->hrtf, sizeof f->hrtf);
        xds_get_blob(r, f->gain_prev, sizeof f->gain_prev);
        f->gain_valid = xds_get_u32(r) != 0;
        if (f->resampler)
            src_reset(f->resampler);         /* the handle stays the model's */
    }
    if (r->err || fr.pos != fr.len)
        ro->err = 1;
    r = ro;
    free(buf);
    }
    /* The frame's scratch: the next frame starts from silence, as every frame does. */
    memset(vp->sample_buf, 0, sizeof vp->sample_buf);
    return !r->err;
}

/* ---- the APU section (kernel/xbox_dev_state.h) ---------------------------------------------------------
 * Kept here rather than in apu_core.c so that only a program that takes states links it (and the device
 * queue's clear with it). d->lock is taken through mcpx_apu_lock_guest, which the frame thread yields at
 * a frame boundary: with the thread held (mcpx_apu_state_hold) or not, the model is read and written
 * between two frames. */
#include "apu_xaudio2.h"

void mcpx_apu_lock_guest(MCPXAPUState *d);
void mcpx_apu_unlock_guest(MCPXAPUState *d);

#define APU_SECTION_MAGIC 0x31555041u   /* "APU1" */

int mcpx_apu_state_capture(XdsWriter *w)
{
    MCPXAPUState *d = g_state;

    xds_put_u32(w, APU_SECTION_MAGIC);
    xds_put_u32(w, d ? 1u : 0u);
    if (!d)
        return !w->err;
    mcpx_apu_lock_guest(d);
    mcpx_apu_model_save(d, w);
    mcpx_apu_dsp_state_save(w);
    apu_mixer_state_save(w);
    mcpx_apu_unlock_guest(d);
    return !w->err;
}

int mcpx_apu_state_restore(XdsReader *r, uint32_t version)
{
    MCPXAPUState *d = g_state;
    int ok;

    if (version != XDS_APU_VERSION || xds_get_u32(r) != APU_SECTION_MAGIC) {
        xds_fail(r);
        return 0;
    }
    if (!xds_get_u32(r))
        return !r->err;                    /* captured without an APU: nothing to restore */
    if (!d) {
        xds_fail(r);
        return 0;
    }
    mcpx_apu_lock_guest(d);
    ok = mcpx_apu_model_load(d, r) && mcpx_apu_dsp_state_load(r) && apu_mixer_state_load(r);
    mcpx_apu_host_after_load(d);
    mcpx_apu_unlock_guest(d);
    /* What the device still has queued was mixed from the state the load left. */
    xa2_clear();
    return ok && !r->err;
}
