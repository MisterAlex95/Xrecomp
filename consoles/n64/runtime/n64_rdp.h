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
/* The two hidden RDRAM bits of each 16-bit word (the low bits of a 16-bit pixel's coverage), one byte per word:
 * index = byte address >> 1. The VI's anti-aliasing filter reads them (n64_vi.c). NULL when the renderer has none. */
const uint8_t *n64_rdp_hidden_bits(n64_rdp *d);

/* Back ends (XBA-173, n64_rdp_backend.h): the software RDP (n64_rdp.c) and, in builds with XBR_N64_RDP_VULKAN, the
 * Vulkan one (paraLLEl-RDP on a compute device, n64_rdp_vk.cpp). n64_rdp_create takes the back end set here, else
 * $RECOMP_N64_RDP ("vulkan", "software" or "auto"), else auto: Vulkan on Android when the build has it and a device
 * with the features it needs is there, software elsewhere. A Vulkan RDP that cannot start falls back to software. On
 * the Vulkan RDP n64_rdp_set_threads is a no-op (returns 1) and n64_rdp_flush waits for the GPU (the VI reads RDRAM
 * next). */
typedef enum { N64_RDP_AUTO = 0, N64_RDP_SOFTWARE = 1, N64_RDP_VULKAN = 2 } n64_rdp_backend;
void n64_rdp_set_backend(n64_rdp_backend b); /* for the next n64_rdp_create calls; N64_RDP_AUTO: back to the default */
n64_rdp_backend n64_rdp_backend_parse(const char *s); /* "vulkan" / "software" / anything else: auto */
const char *n64_rdp_backend_name(const n64_rdp *d); /* "software" or "vulkan" */

#ifdef __cplusplus
}
#endif

#endif
