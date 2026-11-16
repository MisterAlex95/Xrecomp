/* dc_core.h -- the Dreamcast spike's machine (XBA-227, epic XBA-221): an SH-4 (interpreter + lifted code), the
 * memory map, a BIOS in C (no image), the SH-4's on-chip timers / interrupt controller / serial port / store
 * queues, Holly's interrupt registers and the PowerVR2 framebuffer read-out (no TA, no 3D). Linux headless only
 * (dc_run), nothing in the app. See consoles/README.md « Dreamcast ». */
#ifndef DC_CORE_H
#define DC_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dc_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dc_machine dc_machine;

dc_machine *dc_create(void);
void dc_destroy(dc_machine *m);

/* Loads a program: an SH ELF (PT_LOAD segments, entry from the header) or a raw binary (an unscrambled
 * 1ST_READ.BIN) at 0x8C010000. Then sets the CPU up as the BIOS leaves it for a disc's program. 0 or -1 (err). */
int dc_load(dc_machine *m, const uint8_t *data, size_t size, char *err, size_t errlen);

/* Runs n frames (1/60 s of SH-4 time each, 200 MHz, one instruction per cycle). Returns the frames run (fewer when
 * the program exited). */
int dc_run_frames(dc_machine *m, int n);
bool dc_exited(const dc_machine *m, int *code);

/* What the program wrote to the serial port (SCIF), NUL-terminated. */
const char *dc_serial(const dc_machine *m, size_t *len);

/* The framebuffer as the video output shows it, RGB888 into out (at least DC_FB_MAX_W * DC_FB_MAX_H * 3
 * bytes); sets *w, *h.
 * 0, or -1 when the display is off. */
#define DC_FB_MAX_W 1024
#define DC_FB_MAX_H 1024
int dc_framebuffer_rgb(dc_machine *m, uint8_t *out, int *w, int *h);

/* Lifted code (NULL: interpreter only). 0, or -1 when the ABI or the image's bytes differ. */
int dc_attach_recomp(dc_machine *m, const dc_recomp_table *t);

/* Addresses inside the lifted image the interpreter had to start at: copies at most max, returns the total. */
size_t dc_recomp_missed(dc_machine *m, uint32_t *out, size_t max);

typedef struct dc_stats {
    uint64_t native;  /* instructions lifted code ran (packs built with -DDC_RC_COUNT) */
    uint64_t interp;  /* instructions the interpreter ran */
    uint64_t calls;   /* dispatches into lifted code */
    uint64_t irqs;    /* interrupts taken */
    uint64_t hle;     /* BIOS calls served */
} dc_stats;
void dc_get_stats(dc_machine *m, dc_stats *out);

void dc_set_verbose(dc_machine *m, int level);
uint32_t dc_peek32(dc_machine *m, uint32_t addr);
sh4_cpu *dc_cpu(dc_machine *m);

/* The decoder's view of every opcode, one "xxxx NAME" line each ("-": illegal), to compare with dc_tools.sh4. */
void dc_dump_decode_table(void *file);

#ifdef __cplusplus
}
#endif

#endif
