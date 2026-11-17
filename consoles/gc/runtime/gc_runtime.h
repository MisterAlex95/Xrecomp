/* gc_runtime -- the GameCube runtime (XBA-259, epic XBA-219): the machine of gc_core.h played on the host layer
 * (xbr_host: host_window, host_pad / console_pad, recomp_lifecycle, the perf line, the ADPF hint). The app shell's
 * loader calls gc_runtime_start with a homebrew DOL; the desktop's gc_play calls gc_runtime_run.
 *
 * Video: the XFB the video interface scans out (YUV 4:2:2), uploaded as is and converted to RGB on the GPU (a
 * fragment shader into a texture, presented by host_window_present_texture), shown at 4:3; RECOMP_GC_YUV=cpu (or a
 * shader that does not build) converts on the CPU and presents RGBA instead. Timing: one VI field per shown frame,
 * paced at the field's own length (60 Hz NTSC, 50 Hz PAL: RECOMP_GC_TV=pal starts the machine as a PAL IPL would).
 * Pads: host pad n is the controller in port n (console_pad, CONSOLE_PAD_GC: RECOMP_GC_PAD_MAP), port 1 always
 * plugged in; Z held by the game's motor command rumbles the host pad. Audio (XBA-263): the machine's 48 kHz output
 * (gc_audio.h: the DSP's audio DMA and the disc stream) through host_audio, the pacing nudged by its queue.
 * consoles/README.md « GameCube », Plane « Spec · Other consoles ». */
#ifndef XBR_GC_RUNTIME_H
#define XBR_GC_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "console_pad.h"
#include "gc_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_RUNTIME_VERSION "0.1.0"

/* "gc_runtime <version>". */
const char *gc_runtime_version(void);

/* What gc_runtime_run plays and how. Zeroed = defaults (gc_runtime_opts_init). */
typedef struct gc_runtime_opts {
    /* The program: a DOL, a path or "fd:N" (a descriptor the app shell opened from a document; read from offset 0,
     * left open). At most 16 MB. */
    const char *dol;
    /* The game's key (DOL-CRC32, recomp_pack_rom_identify); NULL or "": computed (gc_runtime_key). */
    const char *title_key;
    /* A pack built by gc_tools.recomp (libtitle.so exporting gc_recomp_title_table); NULL or "": the interpreter
     * alone. Its code runs only where memory holds what it was lifted from (the core checks every span). */
    const char *pack_lib;
    /* At exit, the text addresses the interpreter had to start at are written here (gc_tools.recomp --discovered);
     * NULL: not written. */
    const char *discovered_path;
    /* Stop after this many fields (0: until the player quits). */
    uint64_t frames;
    /* 1: no window (fields still run, pads still read); also when host_window_headless(). */
    int headless;
    /* 1: no pacing, as fast as the host runs (tests). */
    int unpaced;
    /* Write the last field's XFB as a PNG here at exit (CPU conversion, gc_xfb_rgb); NULL: none. */
    const char *screenshot;
    /* Capture the window's drawable (the real present path, host_window_capture) at this field, to capture_path. */
    uint64_t capture_frame;
    const char *capture_path;
    /* GC_TV_NTSC / GC_TV_PAL; -1: $RECOMP_GC_TV ("pal", else NTSC). */
    int tv_mode;
    /* 1: convert the XFB on the CPU; 0: on the GPU; -1: $RECOMP_GC_YUV ("cpu": CPU, else GPU). */
    int yuv_cpu;
    /* Port 1 held for the whole run on top of the pads (tests): its buttons are or'ed, its sticks / triggers used
     * when pad_force_on. */
    int pad_force_on;
    gc_pad_state pad_force;
    /* Window scaling / filter: -1 = $RECOMP_GC_SCALE / _FILTER, else the shared $RECOMP_SCALE / $RECOMP_FILTER, else
     * aspect and linear (host_window_env_scale / _filter); else HOST_SCALE_* / HOST_FILTER_*. */
    int scale, filter;
    /* Everything played also written here as a 48 kHz stereo WAV (tests); NULL: none. */
    const char *wav_path;
} gc_runtime_opts;

void gc_runtime_opts_init(gc_runtime_opts *o);

/* Plays o->dol until the player quits (window closed, SDL_QUIT) or o->frames fields ran. Returns 0, or -1 when the
 * file cannot be read or is no DOL (why on stderr). Logs "gc: ..." lines on stdout, the perf line among them. */
int gc_runtime_run(const gc_runtime_opts *o);

/* Where gc_runtime_start writes the discovered addresses: <dir>/gc-<KEY>/discovered.txt (the app shell's
 * <internal files>/work); NULL: not written. */
void gc_runtime_set_work_dir(const char *dir);

/* The app shell's entry point (android/app/src/main/cpp/pack_loader.c): dol is the program (a path or "fd:N"),
 * pack_lib a title pack's library or NULL, title_key the game's key. Plays it with gc_runtime_run and returns its
 * result once the player quit. RECOMP_GC_FRAMES bounds the run (checks). */
int gc_runtime_start(const char *dol, const char *pack_lib, const char *title_key);

/* ---- Pure helpers (gc_pad_map.c, tested in consoles/gc/tests) ---- */

/* The key recomp_pack_rom_identify gives a DOL: "DOL-" + CRC32 of the whole file in 8 upper-case hex digits (a DOL
 * has no title or serial). Returns 0, or -1 when out is too small (24 bytes is enough). */
int gc_runtime_key(const uint8_t *dol, size_t size, char *out, size_t out_n);

/* The GameCube buttons (GC_PAD_*) of console_pad targets (bit GC_T_*), opposite directions cancelled. */
uint16_t gc_runtime_buttons_of(uint32_t targets);

/* A host axis (-32768..32767) as a GameCube stick axis (1..255, 128 at rest); invert: the y axes (SDL's +y is down,
 * the GameCube's up). */
uint8_t gc_runtime_axis(int v, int invert);

/* One pad through the player's mapping (o: console_pad_read / _apply with a CONSOLE_PAD_GC map m) as a GameCube
 * controller: buttons, the main stick (stick 0), the C stick (stick 1), and the analog triggers from the host
 * triggers lt / rt (0..32767) that L / R are mapped to (full when their click is down). */
void gc_runtime_pad_of(const ConsolePadMap *m, const ConsolePadOut *o, int lt, int rt, gc_pad_state *out);

#ifdef __cplusplus
}
#endif

#endif /* XBR_GC_RUNTIME_H */
