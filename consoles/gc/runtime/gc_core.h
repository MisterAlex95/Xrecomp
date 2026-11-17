/* gc_core.h -- the GameCube spike's machine (XBA-225, epic XBA-219): a Gekko (PowerPC 750 + paired singles) that runs
 * lifted code with an interpreter behind it, the console's memory map, and the little hardware a libogc program
 * touches before it draws text on the XFB: the processor interface (interrupts), the video interface (timing,
 * retrace interrupts, the framebuffer address), EXI that completes at once, and standard controllers on the serial
 * interface (XBA-259). Sound: the DSP interface, ARAM, the audio DMA and interface, the DSP's microcode at a high
 * level, the disc's streamed audio (gc_audio.h, XBA-263). No GX yet. Discs and the DVD drive: gc_disc.h, gc_boot.h
 * (XBA-261). Played on the host layer by gc_runtime.h (the app, gc_play); headless by gc_run.
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

/* Runs one video field (half a 525-line NTSC frame, 1/60 s; half a 625-line PAL frame, 1/50 s, once the video
 * interface is set to PAL): returns 0, or -1 when the machine stopped (gc_error says why). */
int gc_run_frame(gc_machine *m);

/* The TV standard the IPL would have left (XBA-259): GC_TV_NTSC (the default: the video interface starts off and
 * libogc sets it up for NTSC) or GC_TV_PAL (the video interface left on in PAL, low memory's TV mode 1, so libogc's
 * VIDEO_GetPreferredMode picks a PAL mode). Call after gc_create, before the first field. */
enum { GC_TV_NTSC = 0, GC_TV_PAL = 1 };
void gc_set_tv_mode(gc_machine *m, int mode);
/* The length of the next field in nanoseconds: 16666667 at 60 Hz, 20000000 at 50 Hz (what a host paces to). */
uint64_t gc_field_ns(const gc_machine *m);

/* Standard controllers on the serial interface (XBA-259). Buttons in the SI's own bit order (the order of libogc's
 * PAD_BUTTON_*); sticks 0..255 with 128 at rest, +x right, +y up; analog triggers 0..255 (0 released). A port with
 * nothing plugged in (the default for all four) answers no command: PAD_Read says PAD_ERR_NO_CONTROLLER. */
enum {
    GC_PAD_LEFT = 0x0001, GC_PAD_RIGHT = 0x0002, GC_PAD_DOWN = 0x0004, GC_PAD_UP = 0x0008, GC_PAD_Z = 0x0010,
    GC_PAD_R = 0x0020, GC_PAD_L = 0x0040, GC_PAD_A = 0x0100, GC_PAD_B = 0x0200, GC_PAD_X = 0x0400,
    GC_PAD_Y = 0x0800, GC_PAD_START = 0x1000,
};
typedef struct gc_pad_state {
    bool connected;
    uint16_t buttons;
    uint8_t stick_x, stick_y, cstick_x, cstick_y;
    uint8_t trigger_l, trigger_r;
} gc_pad_state;
void gc_set_pad(gc_machine *m, int port, const gc_pad_state *st);
/* The rumble motor command the game last sent port's controller: 0 stop, 1 rumble, 2 hard stop. */
int gc_pad_motor(const gc_machine *m, int port);
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
/* The same picture left in YUV 4:2:2 (Y0 U Y1 V, w * 2 bytes per line, lines top to bottom, the two fields woven),
 * for a host that converts on its GPU (gc_runtime). yuv holds GC_XFB_MAX_W * GC_XFB_MAX_H * 2 bytes. */
int gc_xfb_yuv(gc_machine *m, uint8_t *yuv, int *w, int *h);
/* The physical address of the XFB the video interface scans (its top field), 0 without one: where GX's copy to the
 * XFB landed, for a host that shows GX's own picture of it (gx_xfb_texture, XBA-262). */
uint32_t gc_xfb_addr(gc_machine *m);

uint32_t gc_peek32(gc_machine *m, uint32_t ea);

/* The machine's GPU (consoles/gc/runtime/gx/gx.h): attach a renderer with gx_gl_attach(gc_gx(m), ...) on the thread
 * that runs gc_run_frame, with a GLES 3 context current (XBA-262). */
typedef struct gx_state gx_state;
gx_state *gc_gx(gc_machine *m);

/* ---- Save states (XBA-264, gc_state.c) ----
 * A state is the whole machine between two fields (call these between gc_run_frame calls, on the thread that runs
 * them: the renderer's queued draws are flushed): the Gekko, MEM1, the locked cache, ARAM, the hardware registers
 * and their timers, GX's register files, FIFO state and TMEM, the DVD drive's state (not the disc), the pads; tagged,
 * versioned sections (gc_state.c says which, and what is left out: the EFB, host resources). key: the title key
 * (gc_runtime_key / the disc's), written in the state and checked at load (NULL or "": not checked). Results share
 * recomp_states.h's RECOMP_STATE_* values; why (may be NULL) says what refused a state. */
#define GC_STATE_VERSION 1
enum {
    GC_STATE_OK = 0,
    GC_STATE_EGAME = -3,    /* another title's state, or another console's */
    GC_STATE_EVERSION = -4, /* another format version, or a section newer than this build reads */
    GC_STATE_ECORRUPT = -7, /* not a state, truncated, damaged, a required section missing */
};
/* The bytes gc_state_save needs now (MEM1 / ARAM / TMEM pages that are all zero are left out). */
size_t gc_state_size(gc_machine *m);
/* Writes the state into out: its size, or 0 when cap is too small or the machine stopped. */
size_t gc_state_save(gc_machine *m, const char *key, uint8_t *out, size_t cap);
/* Whether a state would load (nothing changes). */
int gc_state_check(gc_machine *m, const char *key, const uint8_t *data, size_t size, char *why, size_t why_n);
/* Loads a state (checked whole first: a refused state changes nothing). Lifted code is checked against memory again
 * before it runs; the renderer forgets its EFB copies and XFB pictures. */
int gc_state_load(gc_machine *m, const char *key, const uint8_t *data, size_t size, char *why, size_t why_n);

/* ---- Cheats' memory (XBA-264): MEM1 words at an effective address (0x80000000 / 0xC0000000 / physical alike),
 * big-endian values, no side effect; a write that changes a word invalidates lifted code there (gc_recomp_icbi), so
 * patched code runs in the interpreter. Outside MEM1: reads 0, writes are dropped. */
uint32_t gc_cheat_read32(gc_machine *m, uint32_t ea);
void gc_cheat_write32(gc_machine *m, uint32_t ea, uint32_t v);

#ifdef __cplusplus
}
#endif

#endif
