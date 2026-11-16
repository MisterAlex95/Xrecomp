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

/* The machine the core models. Naomi (XBA-235, a spike) is off unless asked for (dc_run --naomi): 32 MB main RAM,
 * 8 MB sound RAM, 32 KB SRAM at 0x00200000 instead of the flash, the cartridge board in the G1 window
 * (0x005F7000) with G1 DMA from it, and the Naomi BIOS in C (the cartridge header's load list). See
 * docs/technical/naomi.md. */
typedef enum dc_model { DC_MODEL_DREAMCAST = 0, DC_MODEL_NAOMI = 1 } dc_model;

/* The JVS I/O board as games see it (stub: XBA-235): coin counters, the test / service buttons, two players' 13
 * switches (JVS bit order: start, service, up, down, left, right, push 1..7). */
typedef struct dc_jvs_state {
    bool test;
    uint16_t coins[2];
    uint16_t players[2]; /* bit 15 start, 14 service, 13 up, 12 down, 11 left, 10 right, 9-3 push 1-7 */
} dc_jvs_state;

dc_machine *dc_create(void); /* a Dreamcast */
dc_machine *dc_create_model(dc_model model);
dc_model dc_get_model(const dc_machine *m);
void dc_destroy(dc_machine *m);

/* Loads a program: an SH ELF (PT_LOAD segments, entry from the header) or a raw binary (an unscrambled
 * 1ST_READ.BIN) at 0x8C010000. Then sets the CPU up as the BIOS leaves it for a disc's program. 0 or -1 (err). */
int dc_load(dc_machine *m, const uint8_t *data, size_t size, char *err, size_t errlen);

/* Naomi: inserts the cartridge, a plain (unencrypted, or already decrypted) ROM image in a file the user supplies
 * (XRecomp ships none), read whole. Then dc_naomi_boot does what the BIOS does: checks the header ("NAOMI" board
 * name), copies the game load list (header 0x360: ROM offset, RAM address, length; offset 0xFFFFFFFF ends it) to
 * RAM and starts at the game entry (0x420), the CPU set up as for a disc's program. 0, or -1 (err). */
int dc_naomi_insert_cart(dc_machine *m, const char *path, char *err, size_t errlen);
int dc_naomi_insert_cart_mem(dc_machine *m, const uint8_t *data, size_t size, char *err, size_t errlen);
int dc_naomi_boot(dc_machine *m, char *err, size_t errlen);

/* The JVS state the stubbed Maple-JVS bridge reports (Naomi). */
dc_jvs_state *dc_jvs(dc_machine *m);
/* One JVS request packet (E0 dst len data.. sum, D0 escapes) -> the I/O board's reply packet into out. Serves
 * reset (no reply), set address, ID, versions, features, switch and coin reads. Returns the reply's length, 0 when
 * none (reset, another node, bad checksum). The Maple side (the MIE bridge's frames) is not modelled yet. */
size_t dc_jvs_transfer(dc_machine *m, const uint8_t *req, size_t n, uint8_t *out, size_t cap);

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
