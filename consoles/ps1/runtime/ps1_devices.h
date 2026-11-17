/* ps1_devices.h -- the PlayStation devices the core drives (Plane epic P, XBA-143).
 *
 * The core (ps1_core.h: R3000A CPU + COP0, GTE, bus, DMA, interrupts, timers, pads / memory cards, BIOS) talks to the
 * other chips only through this header, so each can be written and tested on its own:
 *   GPU   ps1_gpu.c    (P-7, XBA-165)  GP0 / GP1, VRAM, software rasterizer, display output
 *   CD    ps1_disc.c   (P-1, XBA-159)  disc images: .cue / .bin, .iso, .chd; ps1_cdrom.c (P-9, XBA-167) controller
 *   SPU   ps1_spu.c    (P-8, XBA-166)  24 ADPCM voices, ADSR, reverb, CD-DA / XA mixing
 *   MDEC  ps1_mdec.c   (P-9, XBA-167)  macroblock decoder
 * Each device is a plain C11 object with no globals and no host dependency. Time: the core calls each device's tick
 * with CPU cycles (33.8688 MHz); a device raises interrupts through the sink it was created with. DMA: the core's DMA
 * controller moves words through the device's dma functions (channel 0 MDEC in, 1 MDEC out, 2 GPU, 3 CD-ROM, 4 SPU).
 * Frozen between the core and device agents: change together. */
#ifndef PS1_DEVICES_H
#define PS1_DEVICES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS1_CPU_HZ 33868800u

/* I_STAT bits. */
enum {
    PS1_IRQ_VBLANK = 0,
    PS1_IRQ_GPU = 1,
    PS1_IRQ_CDROM = 2,
    PS1_IRQ_DMA = 3,
    PS1_IRQ_TMR0 = 4,
    PS1_IRQ_TMR1 = 5,
    PS1_IRQ_TMR2 = 6,
    PS1_IRQ_SIO0 = 7, /* pads / memory cards */
    PS1_IRQ_SIO1 = 8,
    PS1_IRQ_SPU = 9,
    PS1_IRQ_LIGHTPEN = 10,
};

typedef struct ps1_irq_sink {
    void (*raise)(void *ctx, int irq); /* sets I_STAT bit irq (edge) */
    void *ctx;
} ps1_irq_sink;

/* ---- GPU ---------------------------------------------------------------------------------------------------------- */
typedef struct ps1_gpu ps1_gpu;

ps1_gpu *ps1_gpu_create(ps1_irq_sink irq);
void ps1_gpu_destroy(ps1_gpu *g);
void ps1_gpu_reset(ps1_gpu *g);
void ps1_gpu_gp0(ps1_gpu *g, uint32_t word); /* 0x1F801810 write */
void ps1_gpu_gp1(ps1_gpu *g, uint32_t word); /* 0x1F801814 write */
uint32_t ps1_gpu_read(ps1_gpu *g);           /* 0x1F801810 read: GPUREAD */
uint32_t ps1_gpu_stat(ps1_gpu *g);           /* 0x1F801814 read: GPUSTAT */
void ps1_gpu_dma_write(ps1_gpu *g, const uint32_t *words, size_t n); /* DMA2 to the GPU (GP0) */
void ps1_gpu_dma_read(ps1_gpu *g, uint32_t *words, size_t n);        /* DMA2 from the GPU (GPUREAD) */

/* Video timing chosen by GP1(08h): the core runs scanlines from it (NTSC 263 lines x 3413 video cycles, PAL 314 x
 * 3406; video clock = CPU x 11/7) and clocks timer 0 (dot clock) / timer 1 (hblank) with it. */
typedef struct ps1_gpu_timing {
    int pal;          /* 1 = PAL */
    int interlaced;   /* vertical interlace on */
    int dot_divider;  /* video cycles per dot: 10, 8, 5, 4 or 7 (368 mode) */
    int lines;        /* scanlines per field: 263 or 314 */
    int cycles_per_line; /* video cycles per scanline: 3413 or 3406 */
} ps1_gpu_timing;
void ps1_gpu_get_timing(ps1_gpu *g, ps1_gpu_timing *t);

/* Called by the core at the start of each scanline (0 .. lines-1), so GPUSTAT's interlace field bit and the
 * "drawing odd lines" bit follow the beam. Raises nothing (VBlank is the core's). */
void ps1_gpu_scanline(ps1_gpu *g, int line);

/* The displayed picture (display area of GP1(05h..07h) over VRAM, 15- or 24-bit), as 0x00RRGGBB, at most max_w x max_h
 * (640 x 512 covers every mode). Returns 0 and the size, or -1 when the display is off (then the host shows black).
 * Upscaled (ps1_gpu_set_upscale): N times wider and higher from the shadow VRAM when that fits max_w x max_h
 * (640N x 512N covers every mode), else the native picture. */
int ps1_gpu_display(ps1_gpu *g, uint32_t *xrgb, int max_w, int max_h, int *w, int *h);

/* The 1024 x 512 x 16-bit VRAM, for tests and debugging. */
const uint16_t *ps1_gpu_vram(ps1_gpu *g);

/* Internal resolution upscale (XBA-184; ps1_gpu.c's header): factor 1, 2 or 4 (above 4: 4; 3: 2). Polygons,
 * rectangles and lines are also drawn into a shadow VRAM of 1024N x 512N that the display reads; the native VRAM stays
 * what the game reads back, the same bytes as at 1x. May be called at any time (the shadow is made from the VRAM).
 * Returns the factor in use (1 when the shadow's memory, 4 MB at 2x and 16 MB at 4x, cannot be had). Not part of a
 * save state: a state loaded rebuilds the shadow from its VRAM. */
int ps1_gpu_set_upscale(ps1_gpu *g, int factor);
int ps1_gpu_upscale(ps1_gpu *g);
/* Workers that share the shadow pass of big primitives with the caller (0..3; the picture is the same whatever the
 * count). Default -1: by the host's cores (3 from 8 cores, 2 from 4, 1 from 2). Returns the workers running (none at
 * 1x). */
int ps1_gpu_set_threads(ps1_gpu *g, int n);
/* Called by each of those workers as it starts (starting = 1) and just before it ends (0), on that thread: the
 * runtime notes the thread there (ps1_runtime: its fast cores put back after a resume, the performance hint session;
 * recomp_host_worker_hook) so the GPU itself needs nothing of the host. Process-wide, for the workers started after
 * the call; NULL (the default): none. */
typedef void (*ps1_gpu_thread_hook)(int starting, void *user);
void ps1_gpu_set_thread_hook(ps1_gpu_thread_hook fn, void *user);
/* The shadow VRAM, (1024 x 512) x factor, row stride 1024 x factor; the native VRAM at 1x. Tests. */
const uint16_t *ps1_gpu_vram_hi(ps1_gpu *g);

/* ---- Disc images ---------------------------------------------------------------------------------------------------- */
typedef struct ps1_disc ps1_disc;

/* Opens a .cue (with its .bin tracks), a single .bin (one data track), an .iso (2048-byte sectors) or a .chd (told by
 * its "MComprHD" magic whatever the name). path may
 * also be "fd:N" (an open descriptor the app passes; .cue tracks then need their own paths, so fd: is for single-file
 * images). Returns NULL and a message on failure. */
ps1_disc *ps1_disc_open(const char *path, char *err, size_t errlen);
void ps1_disc_close(ps1_disc *d);
/* Reads one raw 2352-byte sector at absolute LBA (MSF 00:02:00 = LBA 0); a 2048-byte image is returned with a
 * synthesized header / subheader. Returns 0, or -1 past the end. */
int ps1_disc_read_sector(ps1_disc *d, uint32_t lba, uint8_t out[2352]);
int ps1_disc_track_count(ps1_disc *d);
/* Track t (1-based): start LBA, 1 = audio. Returns 0 or -1. */
int ps1_disc_track(ps1_disc *d, int t, uint32_t *start_lba, int *audio);
/* Track t's first LBA (its INDEX 00 / PREGAP; the same as its start without one). Returns 0 or -1. */
int ps1_disc_track_begin(ps1_disc *d, int t, uint32_t *begin_lba);
uint32_t ps1_disc_size(ps1_disc *d); /* sectors */

/* ---- CD-ROM controller ---------------------------------------------------------------------------------------------- */
typedef struct ps1_cdrom ps1_cdrom;

ps1_cdrom *ps1_cdrom_create(ps1_irq_sink irq, ps1_disc *disc /* NULL = no disc, lid closed */);
void ps1_cdrom_destroy(ps1_cdrom *c);
/* Swap the disc (multi-disc games; NULL: none). With the lid closed it is a whole swap at once: the lid opens (INT5,
 * the drive stops) and closes again on the new disc. With the lid open (ps1_cdrom_set_lid) only the disc changes. */
void ps1_cdrom_set_disc(ps1_cdrom *c, ps1_disc *disc);
/* The lid (XBA-166): 1 opens it (the drive stops, stat bit 4, INT5 01h 08h), 0 closes it (the motor spins up,
 * bit 4 stays set until a Getstat). Commands that need the disc fail while it is open. */
void ps1_cdrom_set_lid(ps1_cdrom *c, int open);
uint8_t ps1_cdrom_read(ps1_cdrom *c, uint32_t reg);           /* 0x1F801800 + reg, reg 0..3 */
void ps1_cdrom_write(ps1_cdrom *c, uint32_t reg, uint8_t v);
void ps1_cdrom_tick(ps1_cdrom *c, uint32_t cpu_cycles);
size_t ps1_cdrom_dma_read(ps1_cdrom *c, uint32_t *words, size_t n); /* DMA3 */
/* CD-DA and decoded XA-ADPCM, 44100 Hz stereo, after the CD volume matrix: the SPU pulls it as it mixes. */
size_t ps1_cdrom_audio_pull(ps1_cdrom *c, int16_t *stereo, size_t frames);

/* ---- SPU ------------------------------------------------------------------------------------------------------------ */
typedef struct ps1_spu ps1_spu;

ps1_spu *ps1_spu_create(ps1_irq_sink irq);
void ps1_spu_destroy(ps1_spu *s);
uint16_t ps1_spu_read16(ps1_spu *s, uint32_t off);          /* off = address - 0x1F801C00, 0 .. 0x3FF */
void ps1_spu_write16(ps1_spu *s, uint32_t off, uint16_t v);
void ps1_spu_dma_write(ps1_spu *s, const uint32_t *words, size_t n); /* DMA4 */
void ps1_spu_dma_read(ps1_spu *s, uint32_t *words, size_t n);
/* Advances the SPU (one sample per 768 CPU cycles), mixing in the CD audio pulled from cd (may be NULL). */
void ps1_spu_tick(ps1_spu *s, uint32_t cpu_cycles, ps1_cdrom *cd);
/* The mixed output, 44100 Hz stereo int16, buffered (at least 8 frames' worth, oldest dropped on overflow). */
size_t ps1_spu_audio_pull(ps1_spu *s, int16_t *stereo, size_t frames);

/* ---- MDEC ----------------------------------------------------------------------------------------------------------- */
typedef struct ps1_mdec ps1_mdec;

ps1_mdec *ps1_mdec_create(void);
void ps1_mdec_destroy(ps1_mdec *d);
uint32_t ps1_mdec_read(ps1_mdec *d, uint32_t reg); /* 0x1F801820 + 4 * reg, reg 0 data, 1 status */
void ps1_mdec_write(ps1_mdec *d, uint32_t reg, uint32_t v);
void ps1_mdec_dma_write(ps1_mdec *d, const uint32_t *words, size_t n); /* DMA0 in */
size_t ps1_mdec_dma_read(ps1_mdec *d, uint32_t *words, size_t n);      /* DMA1 out */
size_t ps1_mdec_out_pending(ps1_mdec *d); /* output words not read yet (what DMA1 can move before its request drops) */

/* ---- Save states (XBA-182) -------------------------------------------------------------------------------------- */
/* Each device's state as opaque bytes of this build (its own struct without host pointers: the IRQ sink, the disc,
 * buffers; the SPU adds its RAM, the MDEC its output not read yet). size: the bytes save writes (the MDEC's depends
 * on its output queue). load: 0, or -1 when n is not what this build writes (the device is left as it was). The
 * placeholders keep nothing (size 0). The core's ps1_state_save / ps1_state_load (ps1_core.h) put them together.
 * valid: checks the bytes before anything is loaded (a state is only identified by hashes, so a crafted one can hold
 * any value, and the fields used as indices or bounds would reach outside host arrays): 0 when load accepts them and
 * the device can run them, -1 when n is not what this build writes (as load), -2 when a field is out of range. */
size_t ps1_gpu_state_size(ps1_gpu *g);
void ps1_gpu_state_save(ps1_gpu *g, uint8_t *out);
int ps1_gpu_state_valid(const uint8_t *in, size_t n);
int ps1_gpu_state_load(ps1_gpu *g, const uint8_t *in, size_t n);
size_t ps1_cdrom_state_size(ps1_cdrom *c);
void ps1_cdrom_state_save(ps1_cdrom *c, uint8_t *out);
int ps1_cdrom_state_valid(const uint8_t *in, size_t n);
int ps1_cdrom_state_load(ps1_cdrom *c, const uint8_t *in, size_t n);
size_t ps1_spu_state_size(ps1_spu *s);
void ps1_spu_state_save(ps1_spu *s, uint8_t *out);
int ps1_spu_state_valid(const uint8_t *in, size_t n);
int ps1_spu_state_load(ps1_spu *s, const uint8_t *in, size_t n);
size_t ps1_mdec_state_size(ps1_mdec *d);
void ps1_mdec_state_save(ps1_mdec *d, uint8_t *out);
int ps1_mdec_state_valid(const uint8_t *in, size_t n);
int ps1_mdec_state_load(ps1_mdec *d, const uint8_t *in, size_t n);

/* For the valid functions: var = the struct field `field` of type `type` from state bytes `in` laid out as the struct
 * (by offset: the blob may not be aligned). Needs <string.h>. */
#define PS1_STATE_GET(var, in, type, field)                                                                         \
    do {                                                                                                            \
        _Static_assert(sizeof(var) == sizeof(((type *)0)->field), "PS1_STATE_GET: " #field);                       \
        memcpy(&(var), (in) + offsetof(type, field), sizeof(var));                                                  \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif
