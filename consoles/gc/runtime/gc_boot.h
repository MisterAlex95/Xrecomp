/* gc_boot.h -- booting a GameCube disc without the IPL, and the DVD drive (XBA-261, epic XBA-219).
 *
 * gc_create_disc does what the IPL does after its menu, in C: low memory set up (the disc header's first 0x20
 * bytes at 0x80000000, boot magic, memory size, console type, arena, TV mode from the region, clocks), the disc's
 * apploader loaded at 0x81200000 and run on the Gekko (entry, then init with a report callback, main until it asks
 * for nothing more, each request read from the disc into memory, then close, which returns the main DOL's entry),
 * then the FST and bi2 placed under the top of memory when the apploader left them to the IPL, and the CPU pointed
 * at the main DOL. stderr gets "gc: boot:" lines (apploader date and entry, its reads, the main DOL entry).
 *
 * The DVD interface (0xCC006000, gc_di.c): status / cover / command / DMA / immediate registers; commands read
 * (0xA8, and 0xA8..40 the disc ID), seek (0xAB), inquiry (0x12), request error (0xE0), audio streaming (0xE1 play
 * / stop, 0xE2 status, 0xE4 buffer config; the stream's position advances in time, no samples are decoded yet:
 * DSP / AI audio is XBA-263), stop motor (0xE3), the drive-unlock debug commands (accepted). A command completes
 * after an estimate of the drive's time (seek over the distance from the head, a constant-angular-velocity transfer
 * rate from 2 MB/s inside to 3.1 MB/s outside), then its DMA lands and the transfer-complete interrupt is raised.
 * Disc change (multi-disc sets): the cover opens, half a second later it closes on the new disc. */
#ifndef GC_BOOT_H
#define GC_BOOT_H

#include "gc_core.h"
#include "gc_disc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boots disc (HLE apploader, see above). On success the machine owns disc (closed by gc_destroy); NULL with a reason
 * in why otherwise (disc left to the caller). */
gc_machine *gc_create_disc(gc_disc *disc, char *why, size_t why_len);

/* Swaps the disc in the drive: the cover opens now and closes on disc 0.5 s of guest time later (NULL: the cover
 * stays open, no disc). The machine owns disc; the previous one is closed. Works on a machine made from a DOL too
 * (a drive appears). */
void gc_disc_insert(gc_machine *m, gc_disc *disc);

/* The disc in the drive, NULL when none (or the cover is open). */
gc_disc *gc_current_disc(gc_machine *m);

/* The main DOL entry the boot jumped to (0 for a machine made from a DOL), and the number of reads the apploader
 * asked for. */
uint32_t gc_boot_entry(const gc_machine *m);
unsigned gc_boot_reads(const gc_machine *m);

#ifdef __cplusplus
}
#endif

#endif
