/* ctr_core.h -- the Nintendo 3DS spike's machine (XBA-228, epic XBA-222): one user process of Horizon (the 3DS
 * operating system) on the ARM11, high-level emulated. The 3DSX loader (relocations, code / rodata / data segments
 * at 0x00100000), an ARMv6K + VFPv2 interpreter beside the lifted code, a flat 4 GB guest address space, and in C:
 * the kernel calls a libctru program makes (memory, threads, events, mutexes, address arbiters, IPC) and just enough
 * of the system services for libctru's console (srv, APT, HID, GSP, FS). The top screen is what GSP was told
 * to show. Linux / macOS only, headless: not a 3DS emulator, no PICA200, no DSP. C11 + libc + libm. */
#ifndef CTR_CORE_H
#define CTR_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ctr_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CTR_TOP_W 400
#define CTR_TOP_H 240
#define CTR_BOTTOM_W 320
#define CTR_BOTTOM_H 240

/* A machine with the 3DSX's segments loaded and relocated at 0x00100000, its main thread at the entry. NULL with
 * why on a bad file. Needs a 64-bit host (the guest address space is one 4 GB reservation). */
ctr_machine *ctr_create(const uint8_t *data, size_t size, char *why, size_t why_n);
void ctr_destroy(ctr_machine *m);
void ctr_set_verbose(ctr_machine *m, int level);

/* Runs until the next VBlank (one 59.83 Hz frame of ARM11 time; when every thread waits, time skips ahead). Returns
 * 0, or -1 when the machine stopped (see ctr_error); 1 once the process has exited. */
int ctr_run_frame(ctr_machine *m);
const char *ctr_error(const ctr_machine *m);
void ctr_dump_trace(const ctr_machine *m, FILE *f);
uint64_t ctr_frames(const ctr_machine *m);

/* The screens as GSP last showed them, RGB888 (w * h * 3 bytes, rows top to bottom), or NULL before the program
 * gave GSP a framebuffer. top: the top screen's left eye (400x240), else the bottom screen (320x240). */
const uint8_t *ctr_screen(const ctr_machine *m, int top, int *w, int *h);

/* Debug read of the guest's memory (0 where nothing is mapped). */
uint32_t ctr_peek32(ctr_machine *m, uint32_t addr);

/* The guest address of a host fault address, when it falls inside the guest space; else -1 (crash reports). */
int64_t ctr_guest_address(const void *host);

#ifdef __cplusplus
}
#endif

#endif
