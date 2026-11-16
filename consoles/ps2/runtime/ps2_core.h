/* ps2_core.h -- the PS2 spike's machine (XBA-226, epic XBA-220): an EE (R5900) interpreter with lifted code on top,
 * 32 MB of main RAM, the scratchpad, the EE's SIO (its TX FIFO is the output), latched IO registers, and a minimal
 * HLE of the EE kernel (the syscalls a PS2SDK program makes on its way to main: SetupThread / SetupHeap, the
 * semaphores, the kernel patches PS2SDK installs, Exit). No BIOS image, no IOP (the SIF registers read as an IOP
 * that never answers), no VU0 / VU1, no GS, no DMA. Plain C11, headless (ps2_run). */
#ifndef PS2_CORE_H
#define PS2_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ps2_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads an EE ELF (ps2_load.c) and points the CPU at its entry, as the kernel's LoadExecPS2 leaves it. NULL with a
 * reason in why. */
ps2_machine *ps2_create(const uint8_t *data, size_t size, char *why, size_t why_len);
void ps2_destroy(ps2_machine *m);
void ps2_set_verbose(ps2_machine *m, int level);

/* Runs at most `insns` instructions. 0: still running, 1: stopped on an error (ps2_error), 2: the program exited
 * (the Exit syscall, ps2_exit_code). */
int ps2_run(ps2_machine *m, uint64_t insns);
const char *ps2_error(ps2_machine *m);
int ps2_exit_code(ps2_machine *m);

/* What the program wrote to the EE's SIO TX FIFO (0x1000F180), carriage returns dropped, NUL-terminated. */
const char *ps2_sio_output(ps2_machine *m);

uint32_t ps2_peek32(ps2_machine *m, uint32_t addr);
/* The last instructions the interpreter ran and the syscalls not implemented (debugging). */
void ps2_dump_trace(ps2_machine *m, FILE *f);

#ifdef __cplusplus
}
#endif

#endif
