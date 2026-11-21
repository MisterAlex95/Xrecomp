/* x360_runtime -- the Xbox 360 runtime played on the host layer (XBA-280, epic XBA-229): x360_core's machine in a
 * window (xbr_host: host_window, host_pad / console_pad, host_audio, recomp_lifecycle, the perf line, the ADPF hint),
 * on the model of the GameCube's gc_runtime. The desktop's x360_play calls x360_runtime_run; the app shell's loader
 * calls x360_runtime_start.
 *
 * Video: the GLES renderer (xenos_gl) draws on the window's context at an internal resolution (RECOMP_X360_UPSCALE,
 * else RECOMP_UPSCALE, 1-8), and each swap's picture -- the front buffer at the internal resolution when a resolve
 * wrote it -- is presented as a texture (host_window_present_texture) at 16:9. Timing: one 60 Hz frame of the title's
 * virtual clock per shown frame (x360_run_until), paced at 60 Hz, nudged by the audio queue. Pads: host pad n is the
 * controller of user n (console_pad, CONSOLE_PAD_X360: RECOMP_X360_PAD_MAP), user 0 always connected (the keyboard
 * on a desktop), the triggers analog, XamInputSetState's motors to host_pad_rumble. Audio: the render driver's mix
 * (x360_audio_pull) through host_audio. Save states (<save dir>/x360-<KEY>/state<N>.st), rewind (RECOMP_REWIND=1),
 * fast-forward and the hotkeys through recomp_play, cheats (RECOMP_CHEATS_FILE, raw big-endian words), the perf line
 * every 10 s. Shaders the cache lacks are dumped to <work>/shaders for the next Prepare / bringup to translate;
 * addresses the pack missed go to <work>/discovered.txt. consoles/README.md « Xbox 360 ». */
#ifndef XBR_X360_RUNTIME_H
#define XBR_X360_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "console_pad.h"
#include "x360_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define X360_RUNTIME_VERSION "0.1.0"

const char *x360_runtime_version(void);

typedef struct x360_runtime_opts {
    const char *xex;          /* the title: a plain XEX2 (python3 -m tools.xex --plain), a path or "fd:N" */
    const char *pack_lib;     /* its pack (x360_tools.recomp's library): required */
    const char *disc;         /* the disc (image or directory of its files), NULL: none */
    const char *data_dir;     /* the writable data directory (cache:, hdd:, save data), NULL: none */
    const char *shader_cache; /* GLSL ES by microcode CRC (bringup / Prepare), NULL: the pack's directory */
    const char *work_dir;     /* missed addresses and missing shaders go here, NULL: nowhere */
    const char *save_dir;     /* states in <save_dir>/x360-<KEY>/, NULL: off */
    const char *title_key;    /* NULL: x360_runtime_key of the XEX */
    uint64_t frames;          /* stop after this many frames (0: until the player quits) */
    int headless;             /* 1: no window (the renderer off-screen, frames read back) */
    int unpaced;              /* 1: as fast as the host runs (tests) */
    const char *screenshot;   /* the last frame as a PPM at exit, NULL: none */
    int upscale;              /* internal resolution 1-8; 0: $RECOMP_X360_UPSCALE, else $RECOMP_UPSCALE, else 1 */
    int scale, filter;        /* window scaling / filter; -1: $RECOMP_X360_SCALE / _FILTER, else the shared ones */
    const char *wav_path;     /* everything played also written as a 48 kHz stereo WAV, NULL: none */
    int pad_force_on;         /* user 0 held for the whole run on top of the pads (tests) */
    x360_pad pad_force;
} x360_runtime_opts;

void x360_runtime_opts_init(x360_runtime_opts *o);

/* Plays o->xex until the player quits, o->frames ran or the title stops. 0, or -1 when it cannot start. Logs
 * "x360: ..." lines on stdout, the perf line among them. */
int x360_runtime_run(const x360_runtime_opts *o);

/* The app shell's entry point (pack_loader.c): xex the plain title, pack_lib its library, title_key its key; the pack's
 * directory holds the shader cache (shaders/) unless RECOMP_X360_SHADERS says otherwise, the disc comes from
 * RECOMP_X360_DISC when set. RECOMP_X360_FRAMES bounds the run. */
int x360_runtime_start(const char *xex, const char *pack_lib, const char *title_key);
void x360_runtime_set_work_dir(const char *dir);
void x360_runtime_set_save_dir(const char *dir);

/* ---- pure helpers (x360_pad_map.c, tested without SDL) ---- */

/* "X360-" + the title id and the image CRC: X360-4D5307E6-1A2B3C4D (what recomp_pack_rom_identify gives an XEX). 0,
 * or -1 when the XEX cannot be read or out is too small. */
int x360_runtime_key(const uint8_t *xex, size_t size, char *out, size_t out_n);
/* The per-title defaults of the title whose key this is (renderer options it is known to want: RECOMP_X360_RESOLVE,
 * RECOMP_X360_MSAA...): each set in the environment unless already set, one line per default on out (NULL: none).
 * The app (x360_runtime_start) and x360_run (WP-25) both call it before the renderer starts. */
void x360_title_env(const char *key, FILE *out);
/* XINPUT buttons of console_pad targets (bit X360_T_*); opposite directions cancelled. */
uint16_t x360_runtime_buttons_of(uint32_t targets);
/* One host pad through the player's mapping as an Xbox 360 controller: buttons, the sticks (host +y down -> XInput +y
 * up), the triggers from the host triggers lt / rt (0..32767; full when lt / rt is mapped elsewhere and pressed). */
void x360_runtime_pad_of(const ConsolePadMap *m, const ConsolePadOut *o, int lt, int rt, x360_pad *out);

#ifdef __cplusplus
}
#endif

#endif
