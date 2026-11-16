/* psp_core.h -- the PSP spike's machine (XBA-224, epic XBA-218): an Allegrex interpreter (no VFPU yet) with lifted
 * code on top, 32 MB of main RAM, 2 MB of VRAM, the scratchpad, and a minimal HLE kernel (module start, threads,
 * semaphores, LwMutexes, memory partitions, sceDisplay, sceCtrl, stdio). No GE, no audio, no file system: enough
 * for a PSPSDK debug-screen program. Plain C11, headless (psp_run). */
#ifndef PSP_CORE_H
#define PSP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "psp_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { PSP_SCREEN_W = 480, PSP_SCREEN_H = 272 };

/* Loads an EBOOT.PBP, a PRX or an ELF (psp_load.c) and prepares its start thread. NULL with a reason in why. */
psp_machine *psp_create(const uint8_t *data, size_t size, char *why, size_t why_len);
void psp_destroy(psp_machine *m);
void psp_set_verbose(psp_machine *m, int level);

/* Runs one frame (to the next VBlank). 0: running, 1: stopped on an error (psp_error), 2: the program exited
 * (sceKernelExitGame, or no thread left). */
int psp_run_frame(psp_machine *m);
const char *psp_error(psp_machine *m);
uint64_t psp_frames(psp_machine *m);

/* The framebuffer sceDisplaySetFrameBuf set, as 480x272 RGB (8 bits each). 0, or -1 when none is set. */
int psp_screen_rgb(psp_machine *m, uint8_t *rgb);

uint32_t psp_peek32(psp_machine *m, uint32_t addr);
void psp_set_buttons(psp_machine *m, uint32_t buttons);
/* The last instructions the interpreter ran and the HLE calls not implemented (debugging). */
void psp_dump_trace(psp_machine *m, FILE *f);
/* Text the program wrote to stdout / stderr (sceIoWrite on 1 / 2), NUL-terminated. */
const char *psp_stdout(psp_machine *m);

#ifdef __cplusplus
}
#endif

#endif
