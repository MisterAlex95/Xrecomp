/*
 * xbox_dev_state.h - device models' save states: NV2A, APU, USB (XBA-248, docs/technical/xbox-save-states.md)
 *
 * A save state of an Xbox title is a set of tagged sections (« Data format » in the design): RAM, CPU, MEM,
 * KOBJ, FILE and the three device sections here. Each device serialises its own model into a byte buffer
 * and reads it back; the container (XBA-245) only stores the sections. The byte buffer and the section
 * framing are header-only, so every library (kernel, APU, USB, the GL back end) can use them without a link
 * dependency.
 *
 * Section framing, as the design names it: u32 tag, u32 version, u64 length, payload. Integers are little
 * endian host order (a state only loads into the binary that saved it, on the same host).
 *
 * The protocol, with every guest thread frozen at a safe point (xbox_ParkFreeze):
 *   1. xbox_DevStateFreeze()   holds the device threads between two passes: the NV2A poll thread (after it
 *                              has finished its walk, resolved pending visibility reports and had the GPU
 *                              back end read every render target back into guest memory), the APU frame
 *                              thread, the OHCI controller thread.
 *   2. capture: RAM and the rest (XBA-245), then each device section (xbox_DevStateSections).
 *      or restore: RAM first, then each device section.
 *   3. xbox_DevStateThaw(loaded) lets them run again. After a restore (loaded = 1) the NV2A poll thread
 *      drops every GPU back-end cache (textures, shaders, targets, vertex buffers rebuild from guest memory)
 *      and re-takes PUT / GET from the restored registers before its next pass; the APU's host ring is
 *      already cleared; the pads re-attach to the host's current pads at the controller's next pass.
 *
 * What a device section holds and what it rebuilds instead:
 *   NV2A  the 16 MB register aperture (PGRAPH, PFIFO, PCRTC, PVIDEO, RAMIN with the DMA objects; run-length
 *         coded), the pushbuffer walker's GET / PUT and call state, the executor's decoded state (surface,
 *         vertex attributes, texture stages, combiners, Kelvin register file, vertex program and
 *         constants, semaphore target), the CPU rasteriser's host-side depth buffers. GPU objects are not
 *         saved: targets are read back before the capture, every cache is dropped after a load.
 *   APU   the APU model's registers (main, GP, EP), the voice processor's host-side state (per-voice
 *         resampler, filters, HRTF history and gain ramps, SSLs, HRTF table), the sequencer's sample
 *         count, the GP / EP DSP memories in the MCPX aperture, the DSP handshake state, the software
 *         mixer's play positions. The host ring, the monitor buffer and the reverb's delay lines are
 *         cleared, not saved.
 *   USB   both OHCI register files, the control pipes' state, the pads' addresses and configurations, the
 *         plugged set and hotplug bookkeeping, each pad's rumble (re-applied to the host pad on load).
 */
#ifndef XBOX_DEV_STATE_H
#define XBOX_DEV_STATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XDS_TAG(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) | (uint32_t)(uint8_t)(b) << 8 | (uint32_t)(uint8_t)(c) << 16 | (uint32_t)(uint8_t)(d) << 24)
#define XDS_TAG_NV2A XDS_TAG('N', 'V', '2', 'A')
#define XDS_TAG_APU  XDS_TAG('A', 'P', 'U', ' ')
#define XDS_TAG_USB  XDS_TAG('U', 'S', 'B', ' ')

/* ---- byte buffer -------------------------------------------------------------------------------------- */

/* A growing output buffer. err is set (and stays set) when an allocation fails; every put after that is a
 * no-op, so a writer checks err once at the end. Zero-initialise, free p when done (xds_writer_free). */
typedef struct {
    uint8_t *p;
    size_t len, cap;
    int err;
} XdsWriter;

/* A bounded input. err is set by any read past the end or any value a reader refuses (xds_fail); every get
 * after that returns zeros. */
typedef struct {
    const uint8_t *p;
    size_t len, pos;
    int err;
} XdsReader;

static inline void xds_writer_free(XdsWriter *w)
{
    free(w->p);
    w->p = NULL;
    w->len = w->cap = 0;
    w->err = 0;
}

static inline XdsReader xds_reader(const void *p, size_t len)
{
    XdsReader r;
    r.p = (const uint8_t *)p;
    r.len = len;
    r.pos = 0;
    r.err = 0;
    return r;
}

static inline uint8_t *xds_reserve(XdsWriter *w, size_t n)
{
    if (w->err)
        return NULL;
    if (w->len + n > w->cap) {
        size_t cap = w->cap ? w->cap : 4096;
        uint8_t *q;
        while (cap < w->len + n)
            cap *= 2;
        q = (uint8_t *)realloc(w->p, cap);
        if (!q) {
            w->err = 1;
            return NULL;
        }
        w->p = q;
        w->cap = cap;
    }
    w->len += n;
    return w->p + w->len - n;
}

static inline void xds_put_bytes(XdsWriter *w, const void *p, size_t n)
{
    uint8_t *d = xds_reserve(w, n);
    if (d && n)
        memcpy(d, p, n);
}
static inline void xds_put_u32(XdsWriter *w, uint32_t v) { xds_put_bytes(w, &v, 4); }
static inline void xds_put_u64(XdsWriter *w, uint64_t v) { xds_put_bytes(w, &v, 8); }
static inline void xds_put_f32(XdsWriter *w, float v) { xds_put_bytes(w, &v, 4); }

static inline void xds_fail(XdsReader *r) { r->err = 1; }

static inline const uint8_t *xds_take(XdsReader *r, size_t n)
{
    if (r->err || n > r->len - r->pos) {
        r->err = 1;
        return NULL;
    }
    r->pos += n;
    return r->p + r->pos - n;
}
static inline void xds_get_bytes(XdsReader *r, void *p, size_t n)
{
    const uint8_t *s = xds_take(r, n);
    if (s)
        memcpy(p, s, n);
    else if (n)
        memset(p, 0, n);
}
static inline uint32_t xds_get_u32(XdsReader *r) { uint32_t v; xds_get_bytes(r, &v, 4); return v; }
static inline uint64_t xds_get_u64(XdsReader *r) { uint64_t v; xds_get_bytes(r, &v, 8); return v; }
static inline float xds_get_f32(XdsReader *r) { float v; xds_get_bytes(r, &v, 4); return v; }

/* A plain-data block of a known size, stored with its size so that a state from a build whose struct has
 * another size is refused rather than read askew. */
static inline void xds_put_blob(XdsWriter *w, const void *p, size_t n)
{
    xds_put_u64(w, (uint64_t)n);
    xds_put_bytes(w, p, n);
}
/* Reads a block written by xds_put_blob into p (n bytes); a size other than n fails the reader and leaves p
 * alone. Returns 1 when read. */
static inline int xds_get_blob(XdsReader *r, void *p, size_t n)
{
    const uint8_t *s;
    if (xds_get_u64(r) != (uint64_t)n) {
        xds_fail(r);
        return 0;
    }
    s = xds_take(r, n);
    if (!s)
        return 0;
    if (n)
        memcpy(p, s, n);
    return 1;
}

/* 32-bit words, run-length coded: the register apertures and the depth buffers are mostly runs of one value.
 * Tokens: u32 header; bit 31 set: a run of (header & 0x7FFFFFFF) copies of the u32 that follows; clear: that
 * many literal words follow. The source may be device memory another thread writes (volatile). */
static inline void xds_put_words_rle(XdsWriter *w, const volatile uint32_t *src, size_t n)
{
    size_t i = 0;
    xds_put_u64(w, (uint64_t)n);
    while (i < n && !w->err) {
        uint32_t v = src[i];
        size_t run = 1, lit;
        while (i + run < n && run < 0x7FFFFFFFu && src[i + run] == v)
            run++;
        if (run >= 3) {
            xds_put_u32(w, 0x80000000u | (uint32_t)run);
            xds_put_u32(w, v);
            i += run;
            continue;
        }
        /* Literals up to the next run of three. */
        lit = run;
        while (i + lit < n && lit < 0x7FFFFFFFu) {
            uint32_t a = src[i + lit];
            if (i + lit + 2 < n && src[i + lit + 1] == a && src[i + lit + 2] == a)
                break;
            lit++;
        }
        xds_put_u32(w, (uint32_t)lit);
        {
            uint32_t *d = (uint32_t *)xds_reserve(w, lit * 4);
            size_t k;
            if (d)
                for (k = 0; k < lit; k++) {
                    uint32_t x = src[i + k];
                    memcpy(d + k, &x, 4);
                }
        }
        i += lit;
    }
}

/* Reads n words written by xds_put_words_rle into dst. A different count or a malformed stream fails the
 * reader (dst may then be partly written). */
static inline int xds_get_words_rle(XdsReader *r, volatile uint32_t *dst, size_t n)
{
    size_t i = 0;
    if (xds_get_u64(r) != (uint64_t)n) {
        xds_fail(r);
        return 0;
    }
    while (i < n && !r->err) {
        uint32_t h = xds_get_u32(r), cnt = h & 0x7FFFFFFFu, k;
        if (r->err || cnt == 0 || cnt > n - i) {
            xds_fail(r);
            return 0;
        }
        if (h & 0x80000000u) {
            uint32_t v = xds_get_u32(r);
            /* Compared first: a run of zeros over memory never touched (most of an aperture) stays
             * uncommitted. */
            for (k = 0; k < cnt; k++)
                if (dst[i + k] != v)
                    dst[i + k] = v;
        } else {
            const uint8_t *s = xds_take(r, (size_t)cnt * 4);
            if (!s)
                return 0;
            for (k = 0; k < cnt; k++) {
                uint32_t v;
                memcpy(&v, s + (size_t)k * 4, 4);
                dst[i + k] = v;
            }
        }
        i += cnt;
    }
    return !r->err;
}

/* ---- sections ------------------------------------------------------------------------------------------- */

/* Opens a section; returns the offset xds_section_end patches the length at. */
static inline size_t xds_section_begin(XdsWriter *w, uint32_t tag, uint32_t version)
{
    size_t at;
    xds_put_u32(w, tag);
    xds_put_u32(w, version);
    at = w->len;
    xds_put_u64(w, 0);
    return at;
}
static inline void xds_section_end(XdsWriter *w, size_t at)
{
    if (!w->err) {
        uint64_t n = (uint64_t)(w->len - at - 8);
        memcpy(w->p + at, &n, 8);
    }
}

/* The next section of r: its tag, version and a reader over its payload. Returns 0 at the end of r or on a
 * malformed header (r->err then set). */
static inline int xds_section_next(XdsReader *r, uint32_t *tag, uint32_t *version, XdsReader *payload)
{
    uint64_t n;
    const uint8_t *p;
    if (r->err || r->pos == r->len)
        return 0;
    *tag = xds_get_u32(r);
    *version = xds_get_u32(r);
    n = xds_get_u64(r);
    if (r->err || n > (uint64_t)(r->len - r->pos)) {
        xds_fail(r);
        return 0;
    }
    p = xds_take(r, (size_t)n);
    *payload = xds_reader(p, (size_t)n);
    return 1;
}

/* One device section, for the state container (XBA-245): capture writes the payload (no framing) and
 * returns 1, or 0 when the device cannot be captured; restore reads a payload of that version and returns 1
 * when the device took it (0: refused, the device keeps its state as far as the section says). Both run
 * between xbox_DevStateFreeze and xbox_DevStateThaw. A device that is not running (no APU, USB off)
 * captures an empty payload and restores one as nothing. */
typedef struct {
    uint32_t tag;
    uint32_t version;
    const char *name;
    int (*capture)(XdsWriter *w);
    int (*restore)(XdsReader *r, uint32_t version);
} XdsSection;

#define XDS_NV2A_VERSION 1u
#define XDS_APU_VERSION  1u
#define XDS_USB_VERSION  1u

/* ---- the devices (each in its own library) ------------------------------------------------------------ */

/* NV2A (kernel: xbox_dev_state.c, nv2a_pb_exec.c, nv2a_pb_scan.c, nv2a_vsh_interp.c, xbox_memory_layout.c). */
int  xbox_Nv2aStateCapture(XdsWriter *w);
int  xbox_Nv2aStateRestore(XdsReader *r, uint32_t version);
/* The executor alone (what the fake-guest-memory tests drive). prepare: on the executor's thread before a
 * capture, finish what is queued (visibility reports) and have the back end write every render target to
 * guest memory. after_load: on the executor's thread after a restore, drop the back end's caches. */
void nv2a_pb_exec_state_prepare(void);
void nv2a_pb_exec_state_after_load(void);
void nv2a_pb_exec_state_save(XdsWriter *w);
int  nv2a_pb_exec_state_load(XdsReader *r);
void nv2a_vsh_state_save(XdsWriter *w);
int  nv2a_vsh_state_load(XdsReader *r);
void nv2a_pb_scan_state_save(XdsWriter *w);
int  nv2a_pb_scan_state_load(XdsReader *r);
/* The NV2A poll thread (xbox_memory_layout.c): hold (1) between passes, after it has run
 * nv2a_pb_exec_state_prepare on its own thread, or release (0; loaded: run nv2a_pb_exec_state_after_load
 * and re-take PUT / GET first). Returns 1 when the thread is held / released, 0 when it did not answer
 * within 2 s, -1 when there is no such thread (the caller then runs prepare / after_load itself). */
int  xbox_Nv2aPollHold(int hold, int loaded);
/* The register aperture (16 MB of host memory at 0xFD000000), saved and restored whole. */
void xbox_Nv2aApertureSave(XdsWriter *w);
int  xbox_Nv2aApertureLoad(XdsReader *r);

/* APU (apu_core.c, apu_savestate.c). */
int  mcpx_apu_state_capture(XdsWriter *w);
int  mcpx_apu_state_restore(XdsReader *r, uint32_t version);
/* Hold the frame thread idle (1) or let it run (0). Returns 1 when there is an APU. */
int  mcpx_apu_state_hold(int hold);

/* USB (ohci.c). */
int  xbox_OhciStateCapture(XdsWriter *w);
int  xbox_OhciStateRestore(XdsReader *r, uint32_t version);
/* Hold the controller thread between passes (1) or release it (0). Returns 1 when held / released. */
int  xbox_OhciStateHold(int hold);

/* ---- all of it (kernel: xbox_dev_state.c) ------------------------------------------------------------ */

/* The three device sections, in capture order; *count gets their number. */
const XdsSection *xbox_DevStateSections(int *count);
/* Hold / release every device thread (see the protocol above). Freeze returns 1 when the NV2A poll thread
 * stood still (or there is none); 0 means it did not answer, and a capture would not be consistent (call
 * Thaw all the same). */
int  xbox_DevStateFreeze(void);
void xbox_DevStateThaw(int loaded);
/* Every device section, framed, appended to w / read from r (sections in any order; unknown tags skipped).
 * Call between Freeze and Thaw. Return 1 on success; restore stops at the first section a device refuses. */
int  xbox_DevStateCaptureAll(XdsWriter *w);
int  xbox_DevStateRestoreAll(XdsReader *r);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_DEV_STATE_H */
