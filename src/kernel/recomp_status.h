/* A headless run's status, for the compat runner (XBA-79, XBA-62).
 *
 * With RECOMP_STATUS=<file>, a thread appends one JSON object per line every
 * RECOMP_STATUS_MS (default 1000) milliseconds:
 *
 *   {"ms":3000,"flips":118,"frames":41,"hash":"0x1f2e3d4c","lit":1,
 *    "audio_ms":2100,"pad_reads":180,"threads":7,"discovered":2,"seeds":1}
 *
 *   ms          since the status started (the first runtime init)
 *   flips       frames the title presented (the executor's flip)
 *   frames      how many times the presented picture changed (hash of a
 *               32x24 sample grid)
 *   hash, lit   the last presented frame's hash; lit = 1 when a sampled
 *               pixel is brighter than near-black
 *   audio_ms    output audio that was not silence
 *   pad_reads   pad reports the title read
 *   threads     guest threads the kernel started (the first, run inline as the
 *               title's main thread, counts)
 *   discovered  new discovery-file entries (recomp_discovery.h)
 *   seeds       of them, code the analysis missed (icall, thread, thread_ctx):
 *               what the next Prepare can seed (XBA-129)
 *
 * and, once there are any (XBA-120):
 *
 *   "unsupported":{"points":12,"texture format 0x26 LU_IMAGE_A8CR8CB8Y8":40}
 *               what the renderer met and could not draw, by name, and the
 *               batches that needed it (src/kernel/nv2a_coverage.h); at most
 *               32 names, in the order first met
 *   "unmodelled":{"SET_LOGIC_OP":3}
 *               Kelvin methods the executor does not model, by xemu's name
 *               (or "0x1234"), and how often they arrived; at most 16, the
 *               executor's report sets them
 *
 * and once the title touched the Ethernet controller (XBA-117):
 *
 *   net         only once the title touched the Ethernet controller or the
 *               kernel's PHY routes (XBA-117): "nic probed, link down", or
 *               "nic probed, link up (RECOMP_NIC=0)" with the model off
 *
 * and once the APU frame thread ran (XBA-122):
 *
 *   apu_dsp     once the APU frame thread ran: what the title asked of the
 *               GP/EP DSPs (src/apu/apu_dsp_watch.h has the fields)
 *
 * Unset: nothing is counted beyond a load and a branch per call.
 * docs/technical/discovery.md has the companion file; the runner reads both. */
#ifndef XBOXRECOMP_RECOMP_STATUS_H
#define XBOXRECOMP_RECOMP_STATUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the reporting thread when RECOMP_STATUS is set (once; later calls do nothing). */
void recomp_status_start(void);

/* A presented frame: px points at h rows of pitch bytes, bpp 2 (R5G6B5 or
 * X1R5G5B5), 4 (X8R8G8B8) or 8 (2x-wide anti-aliased 32-bit). */
void recomp_status_frame(const void *px, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp);

/* count interleaved stereo frames of 16-bit audio at 48 kHz, as sent to the device. */
void recomp_status_audio(const int16_t *stereo, int count);

void recomp_status_pad_read(void);

/* The APU's DSP report, a JSON object (src/apu/apu_dsp_watch.c), written as the line's "apu_dsp" field. The
 * caller never blocks: an update that meets the writer is dropped. */
void recomp_status_apu_dsp(const char *json);
void recomp_status_thread_started(void);

/* n more uses of something the renderer could not draw. `name` must live
 * for ever (a literal or a table entry: the pointer is kept) and hold no
 * quote or backslash. One writer (the executor thread). */
void recomp_status_unsupported(const char *name, uint32_t n);

/* An unmodelled method's count so far (replaces the previous one). `name`
 * as above. */
void recomp_status_unmodelled(const char *name, uint64_t count);

/* The title has a network stack (src/kernel/xbox_nic.c): what it found, a
 * string literal. Safe from a signal handler (one pointer store). */
void recomp_status_net(const char *what);

/* Tests: write one line now (as the thread does), and forget everything. */
void recomp_status_write_now(void);
void recomp_status_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_STATUS_H */
