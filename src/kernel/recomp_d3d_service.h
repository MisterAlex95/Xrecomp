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
 * Prints nothing unless a switch is malformed. */
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

/* Deferred RunPushBuffer fixup lists the run-time guards refused (a device
 * found at run time only), counted since start; the executor prints it in the
 * [GPU] software methods report line when non-zero. */
uint32_t recomp_d3d_fixups_refused(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_D3D_SERVICE_H */
