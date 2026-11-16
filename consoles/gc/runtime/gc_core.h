/* gc_core.h -- the GameCube spike's machine (XBA-225, epic XBA-219): a Gekko (PowerPC 750 + paired singles) that runs
 * lifted code with an interpreter behind it, the console's memory map, and the little hardware a libogc program
 * touches before it draws text on the XFB: the processor interface (interrupts), the video interface (timing,
 * retrace interrupts, the framebuffer address), the DSP's mailbox and ARAM DMA, EXI / SI / AI that complete at
 * once. No GX, no DSP code, no disc. Linux / macOS headless only (gc_run), nothing in the app.
 * See consoles/README.md « GameCube ». */
#ifndef GC_CORE_H
#define GC_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "gc_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parses and loads a DOL (text and data sections into MEM1, BSS cleared), sets up low memory the way the IPL leaves
 * it for an application, and points the CPU at the entry. NULL with a reason in why on a bad file. */
gc_machine *gc_create(const uint8_t *dol, size_t size, char *why, size_t why_len);
void gc_destroy(gc_machine *m);

/* Runs one video field (half a 525-line NTSC frame, 1/59.94 s): returns 0, or -1 when the machine stopped
 * (gc_error says why). */
int gc_run_frame(gc_machine *m);
const char *gc_error(const gc_machine *m);
uint64_t gc_frames(const gc_machine *m);
void gc_set_verbose(gc_machine *m, int level);
void gc_dump_state(gc_machine *m, FILE *f);

/* The picture the video interface scans out: the XFB (YUV 4:2:2, Y0 U Y1 V) converted to RGB24 (BT.601, studio
 * range). Returns 0 and fills *w / *h, or -1 when the video interface has no framebuffer yet. rgb holds
 * GC_XFB_MAX_W * GC_XFB_MAX_H * 3 bytes. */
#define GC_XFB_MAX_W 720
#define GC_XFB_MAX_H 576
int gc_xfb_rgb(gc_machine *m, uint8_t *rgb, int *w, int *h);

uint32_t gc_peek32(gc_machine *m, uint32_t ea);

#ifdef __cplusplus
}
#endif

#endif
