/* The XDK D3D service: D3D's GPU semaphore and software methods, answered
 * the way the console's GPU and D3D's interrupt handler answer them, for the
 * D3D device found at run time (recomp_d3d_service.c has the rules).
 *
 * A toolkit default: templates/new-game main() calls recomp_d3d_configure
 * right after recomp_xdk_defaults (before the memory layout; switches only)
 * and recomp_d3d_start right before the guest's entry point, for every title.
 * A title without D3D finds no device: the watch thread polls and stays idle.
 * It was the profile layer profiles/xdk/d3d8 (its d3d8.c compiled into the
 * title), and the DMA walk the layer profiles/xdk/d3d8ltcg chose: both are
 * the default now (docs/technical/profiles.md).
 *
 * Environment (docs/runtime/environment-variables.md):
 *   RECOMP_D3D_SERVICE=0        no service (no hook, no watch, no DMA-walk default)
 *   RECOMP_PB_WALK=scan         the segment scan instead of the DMA walk (default dma)
 *   RECOMP_D3D_DEVICE_PTR=0x..  the device pointer slot, RECOMP_D3D_DEVICE=0x.. the
 *                               device: not searched for (A/B)
 *   RECOMP_D3D_SW_METHODS=0x..  the software methods serviced (bit n = method n)
 *   RECOMP_D3D_CALLBACKS=0      InsertCallback (6, 7) left unserviced
 *   RECOMP_D3D_FENCE=mirror     the fence mirrored instead (needs RECOMP_D3D_DEVICE_PTR)
 */
#ifndef XBOXRECOMP_RECOMP_D3D_SERVICE_H
#define XBOXRECOMP_RECOMP_D3D_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the switches, installs the software-method hook and sets the DMA walk
 * as the default walk. Returns 0 when RECOMP_D3D_SERVICE=0 left it out, else 1.
 * Prints nothing unless a switch is malformed, an address is given
 * (RECOMP_D3D_DEVICE[_PTR]), or RECOMP_D3D_SW_METHODS leaves a RunPushBuffer
 * method out. */
int recomp_d3d_configure(void);

/* Starts the watch thread (device search, GPU semaphore hand-over). No-op on
 * Windows, when left out, and with RECOMP_D3D_FENCE=mirror. */
void recomp_d3d_start(const char *game_dir, const char *save_dir);

/* The device search, for tests: see recomp_d3d_service.c. */
int recomp_d3d_is_device(uint32_t d);
uint32_t recomp_d3d_scan(uint32_t lo, uint32_t hi);
uint32_t recomp_d3d_scan_slots(uint32_t lo, uint32_t hi, uint32_t *slot);
uint32_t recomp_d3d_fence_event_offset(uint32_t d);
int recomp_d3d_locate(void);

/* What the run-time guards of RunPushBuffer's methods refused (a device found
 * at run time; the chain cap also with the addresses given), by kind, counted
 * since start; recomp_d3d_service.c says what each guard checks. */
typedef enum {
    RECOMP_D3D_REFUSED_FIXUPS,   /* a fixup list stopped at an entry */
    RECOMP_D3D_REFUSED_SLOT,     /* a node's return slot outside the contiguous window */
    RECOMP_D3D_REFUSED_NODE,     /* a node outside mapped guest memory */
    RECOMP_D3D_REFUSED_PENDING,  /* a pending count that did not read as one (unconfirmed layout), left alone */
    RECOMP_D3D_REFUSED_CHAIN,    /* a chain cut at 4096 nodes */
    RECOMP_D3D_REFUSAL_KINDS
} RecompD3dRefusal;
uint32_t recomp_d3d_refused(RecompD3dRefusal kind);
uint32_t recomp_d3d_fixups_refused(void);   /* recomp_d3d_refused(RECOMP_D3D_REFUSED_FIXUPS) */

/* The refusals as the [GPU] software methods report line prints them:
 * "; fixup lists refused 3; slots refused 1" (kinds with a non-zero count
 * only; "" when none). Returns the length written into buf (size bytes). */
int recomp_d3d_describe_refusals(char *buf, int size);

/* One pass of the GPU semaphore watch (the watch thread runs it every ms once
 * the device is known): hands the executor [device + 0x30] when it changed
 * and lies in the contiguous window. Returns the semaphore handed over, else
 * last. For tests. */
uint32_t recomp_d3d_watch_once(uint32_t last);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_D3D_SERVICE_H */
