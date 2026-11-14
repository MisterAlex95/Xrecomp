/* n64_rdp.h -- what our software RDP (n64_rdp.c, Plane N-5 XBA-173) offers beyond the frozen n64_devices.h. Nothing
 * here is needed to run a game: the core only uses n64_devices.h. */
#ifndef N64_RDP_H
#define N64_RDP_H

#include "n64_devices.h"

#ifdef __cplusplus
extern "C" {
#endif

#define N64_RDP_MAX_THREADS 8
/* Renderers at n64_rdp_create when XBR_N64_RDP_THREADS is not set: 2 on Android (phones: the RDP is the heaviest part
 * of a frame, and two big cores are the safe bet), 1 elsewhere. A build may define its own. */
#ifndef N64_RDP_DEFAULT_THREADS
#ifdef __ANDROID__
#define N64_RDP_DEFAULT_THREADS 2
#else
#define N64_RDP_DEFAULT_THREADS 1
#endif
#endif

/* Scanline-band threads: n renderers each draw every primitive on their own rows ((y / 4) % n), so the output is the
 * same bit for bit whatever n is. With n > 1 the commands are batched and drawn at SYNC_FULL (and at a few hazards:
 * texture loads from a range drawn in the batch, colour / Z image changes, a full batch), which is when libultra and
 * libdragon look at the result. n is clamped to 1..N64_RDP_MAX_THREADS; returns the count in use. The environment
 * variable XBR_N64_RDP_THREADS sets it at n64_rdp_create (default N64_RDP_DEFAULT_THREADS). */
int n64_rdp_set_threads(n64_rdp *d, int n);
/* Draws whatever is batched (no-op with one thread). */
void n64_rdp_flush(n64_rdp *d);

#ifdef __cplusplus
}
#endif

#endif
