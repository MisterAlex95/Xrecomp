/* n64_rdp.h -- what our software RDP (n64_rdp.c, Plane N-5 XBA-173) offers beyond the frozen n64_devices.h. Nothing
 * here is needed to run a game: the core only uses n64_devices.h. */
#ifndef N64_RDP_H
#define N64_RDP_H

#include "n64_devices.h"

#ifdef __cplusplus
extern "C" {
#endif

#define N64_RDP_MAX_THREADS 8
/* Renderer threads at n64_rdp_create when XBR_N64_RDP_THREADS is not set: 2 on Android (phones: the RDP is the
 * heaviest part of a frame, and two big cores are the safe bet), 1 elsewhere. A build may define its own. */
#ifndef N64_RDP_DEFAULT_THREADS
#ifdef __ANDROID__
#define N64_RDP_DEFAULT_THREADS 2
#else
#define N64_RDP_DEFAULT_THREADS 1
#endif
#endif

/* Renderer threads: the emulation thread only queues the commands, n renderers on their own threads each draw every
 * primitive on their own rows ((y / 4) % n), so the output is the same bit for bit whatever n is, and drawing overlaps
 * the CPU and RSP work of the same display list. They draw into a mirror of RDRAM: the pixels land in RDRAM at fixed
 * emulated moments only (commits: SYNC_FULL, which is when libultra and libdragon look at the result; a texture load
 * from pixels drawn since the last commit; n64_rdp_flush), so what the CPU and the RSP see never depends on host
 * timing nor on n. Without threads (Windows, or none could start) the emulation thread draws at the commits. n is
 * clamped to 1..N64_RDP_MAX_THREADS; returns the count in use. The environment variable XBR_N64_RDP_THREADS sets it at
 * n64_rdp_create (default N64_RDP_DEFAULT_THREADS). */
int n64_rdp_set_threads(n64_rdp *d, int n);
/* Called by each renderer thread as it starts (starting = 1) and just before it ends (0), on that thread: the
 * runtime notes the thread there (n64_runtime: its fast cores put back after a resume, the performance hint session;
 * recomp_host_worker_hook) so the RDP itself needs nothing of the host. Process-wide, for the threads started after
 * the call; NULL (the default): none. */
typedef void (*n64_rdp_thread_hook)(int starting, void *user);
void n64_rdp_set_thread_hook(n64_rdp_thread_hook fn, void *user);
/* The hook as set (both NULL when none): the Vulkan RDP gives it to paraLLEl-RDP's own threads (command ring, fence
 * executor, pipeline compiles) at n64_rdp_create. */
void n64_rdp_get_thread_hook(n64_rdp_thread_hook *fn, void **user);
/* Writes the back end's caches to disk now when they changed (the Vulkan RDP's pipeline cache under
 * RECOMP_VK_CACHE_DIR, n64_vk_cache.h): the app is pausing and may be killed there. Any thread; the RDP, if any, that
 * is alive. Nothing for the software RDP. */
void n64_rdp_save_caches(void);
/* A commit: every command queued is drawn and in RDRAM (the VI reads it next, a save state takes it...). */
void n64_rdp_flush(n64_rdp *d);
/* The end of an emulated frame (n64_run_frame): the software RDP commits there, whether or not the host shows the
 * frame (frame skip, fast-forward), so the moments RDRAM changes stay emulated ones. Other back ends: nothing. */
void n64_rdp_frame_end(n64_rdp *d);
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

/* Internal resolution (XBA-185): the Vulkan RDP draws at 2, 4 or 8 times the N64's resolution (paraLLEl-RDP's
 * upscaling) and its own VI scans the picture out at that size (n64_rdp_scanout; n64_display takes it when the
 * caller's buffer is large enough). RDRAM still holds the native picture, so what the game reads back is unchanged.
 * The software RDP always draws at 1x (logged once at n64_rdp_create when more was asked). n64_rdp_create takes the
 * factor set here, else $RECOMP_N64_UPSCALE, else the shared $RECOMP_UPSCALE, else 1; the Vulkan RDP may lower it
 * when the device lacks memory or buffer range for it (logged). Options read at create (Vulkan RDP, factor > 1):
 *   RECOMP_N64_UPSCALE_DOWNSCALE   0..3: halvings of the scanned-out picture (super-sampling); default: as many as
 *                                  keep it at most 2560 pixels wide (8x: one), so the readback stays affordable
 *   RECOMP_N64_UPSCALE_SSREADBACK  1: the native RDRAM copy is the upscaled picture averaged down (default 0: drawn
 *                                  at 1x, bit-exact)
 *   RECOMP_N64_UPSCALE_NATIVE_TEXRECT  0: texture rectangles are upscaled too (default 1: drawn at 1x, which avoids
 *                                  seams in 2D and HUD art that assumes 1:1 texels)
 *   RECOMP_N64_UPSCALE_NATIVE_LOD  1: texture LOD computed as at 1x (default 0) */
int n64_rdp_upscale_parse(const char *s); /* "1" / "2" / "4" / "8" (other numbers: the power of two below, at most
                                             8); NULL, "" or not a number: 0 */
int n64_rdp_upscale_env(void);            /* $RECOMP_N64_UPSCALE, else $RECOMP_UPSCALE, else 1 */
void n64_rdp_set_upscale(int factor);     /* for the next n64_rdp_create calls; 0: back to the environment */
int n64_rdp_upscale(const n64_rdp *d);    /* the factor the RDP draws at: 1, 2, 4 or 8 (software RDP: 1) */
/* Output pixels per VI pixel of n64_rdp_scanout's picture (the factor less the downscale steps): its width is 640
 * times this, its height 240 / 288 (480 / 576 interlaced) times this. 0: no scanout (software RDP, or 1x). */
int n64_rdp_scanout_scale(const n64_rdp *d);
/* The VI's picture (registers vi[0..13] as n64_io.c holds them, field = the interlace field the VI scans now) as
 * 0x00RRGGBB pixels from the RDP's own VI at the upscaled size. Returns 0, or -1 when there is no scanout (see
 * n64_rdp_scanout_scale), the VI shows nothing, or the picture is larger than max_w x max_h. */
int n64_rdp_scanout(n64_rdp *d, const uint32_t *vi, uint32_t field, uint32_t *xrgb, int max_w, int max_h, int *w,
                    int *h);

/* Host performance counters (n64_core.h n64_perf), totals since n64_rdp_create (kept across a back end replacement):
 * commands run, SYNC_FULL among them, flushes that drew or waited (the software RDP: the emulation thread waiting for
 * its renderer threads at commits, barriers and a full queue, or drawing itself without threads; the Vulkan RDP
 * waiting for the GPU, for the write-back into RDRAM and for the upscaled scanout) and their time in ns.
 * Not in save states. */
typedef struct n64_rdp_stats {
    uint64_t commands, full_syncs, waits, wait_ns;
} n64_rdp_stats;
void n64_rdp_get_stats(n64_rdp *d, n64_rdp_stats *out);

#ifdef __cplusplus
}
#endif

#endif
