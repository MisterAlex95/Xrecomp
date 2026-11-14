/* gba_runtime -- the Game Boy Advance runtime (XBA-146, XBA-157): the machine of gba_core.h played on the host layer
 * (xbr_host: host_window, host_pad, host_audio, recomp_lifecycle). The app shell's loader calls gba_runtime_start;
 * the desktop's gba_play calls gba_runtime_run. consoles/README.md, Plane « Spec · Other consoles ». */
#ifndef XBR_GBA_RUNTIME_H
#define XBR_GBA_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBA_RUNTIME_VERSION "0.1.0"

/* "gba_runtime <version>". */
const char *gba_runtime_version(void);

/* What gba_runtime_run plays and how. Zeroed = defaults. */
typedef struct gba_runtime_opts {
    /* The cartridge: a path, or "fd:N" (a descriptor the app shell opened from a document; read from offset 0, left
     * open). At most 32 MB. */
    const char *rom;
    /* The game's key (GAME-CRC32, recomp_pack_rom_identify); NULL or "": computed from the ROM (gba_runtime_key). */
    const char *title_key;
    /* Saves: <save_dir>/gba-<KEY>/game.sav, read before the first frame, written a second after the game last wrote
     * its save, when the app pauses (background, in-game menu) and at exit. NULL: no saves. */
    const char *save_dir;
    /* A 16 KB BIOS image (never shipped); NULL: $RECOMP_GBA_BIOS, else the core's built-in BIOS. */
    const char *bios;
    /* Stop after this many frames (0: until the player quits). */
    uint64_t frames;
    /* 1: no window (frames still run, pads and audio still open); also when host_window_headless(). */
    int headless;
    /* 1: no pacing, as fast as the host runs (tests). */
    int unpaced;
    /* Write the last frame (240 x 160) as a binary PPM here at exit; NULL: none. */
    const char *screenshot;
    /* Capture the window's drawable (the real present path, host_window_capture) at this frame, to capture_path. */
    uint64_t capture_frame;
    const char *capture_path;
    /* Keys held for the whole run, on top of the pads (KEYINPUT order, 1 = pressed; tests). */
    uint16_t keys;
    /* Window scaling: -1 = $RECOMP_GBA_SCALE ("integer" or "aspect", default aspect); else HOST_SCALE_*. Filter: -1
     * = $RECOMP_GBA_FILTER ("linear" or "nearest", default nearest); else HOST_FILTER_*. */
    int scale, filter;
    /* A GBA title pack's library (XBA-157 / XBA-158, gba_pack.h): taken when its pack.json (beside it) says it is
     * this ROM's, this shell's and this host's and its table loads, else the game runs interpreted (a log line says
     * why). NULL or "": the interpreter alone. */
    const char *pack_lib;
    /* At exit, the ROM addresses the interpreter had to run (gba_recomp_missed) are merged into this file, which the
     * next Prepare hands to the lifter (the discovery loop). NULL: not written. */
    const char *discovered_path;
} gba_runtime_opts;

void gba_runtime_opts_init(gba_runtime_opts *o);

/* Plays o->rom until the player quits (window closed, SDL_QUIT) or o->frames ran. Returns 0, or -1 when the ROM
 * cannot be read or the machine cannot start (why on stderr). Logs one "gba: ..." line per step on stdout. */
int gba_runtime_run(const gba_runtime_opts *o);

/* Where gba_runtime_start keeps saves (the app shell's <internal files>/save); NULL: none. Copied. */
void gba_runtime_set_save_dir(const char *dir);

/* Where gba_runtime_start writes the discovered addresses: <dir>/gba-<KEY>/discovered.txt (the app shell's
 * <internal files>/work: the Prepare's work folder of the game, which it reads them from); NULL: not written. */
void gba_runtime_set_work_dir(const char *dir);

/* The app shell's entry point (XBA-147, android/app/src/main/cpp/pack_loader.c): rom is the game file (a path or
 * "fd:N"), pack_lib a title pack's library (NULL or "" without a pack: the interpreter alone; a pack that is not
 * this ROM's or does not load is left out, and the game still runs), title_key the game's key. Plays it with
 * gba_runtime_run (saves in gba_runtime_set_save_dir's dir, discovered addresses in gba_runtime_set_work_dir's) and
 * returns its result once the player quit. */
int gba_runtime_start(const char *rom, const char *pack_lib, const char *title_key);

/* ---- Pure helpers (tested in consoles/gba/tests) ---- */

/* The key recomp_pack_rom_identify gives a GBA ROM: game code (4 chars, [A-Z0-9] else '_') + '-' + CRC32 of the
 * whole image in 8 upper-case hex digits ("AXVE-1A2B3C4D"). A ROM whose header check fails gets "____-<CRC32>".
 * Returns 0, or -1 when out is too small (24 bytes is enough). */
int gba_runtime_key(const uint8_t *rom, size_t size, char *out, size_t out_n);

/* The GBA keys (KEYINPUT order, 1 = pressed) for an SDL game controller's state (host_pad.h: HOST_PAD_* buttons,
 * left stick -32768..32767, +y down). Xbox layout: A -> A, B -> B, X -> B, Y -> A (a second pair for thumbs that
 * rest there), shoulders -> L / R, triggers past half -> L / R, Start -> Start, Back -> Select, D-pad and left stick
 * (past a third of its travel) -> D-pad. */
uint16_t gba_runtime_pad_keys(uint32_t buttons, int lx, int ly, int lt, int rt);
/* The GBA keys for console_pad targets (bit GBA_T_* of console_pad.h), opposite directions cancelled. The game reads
 * its pads through the player's mapping (RECOMP_GBA_PAD_MAP, console_pad.h); gba_runtime_pad_keys is that mapping's
 * defaults, the fixed layout above. */
uint16_t gba_runtime_keys_of(uint32_t targets);

/* RGB555 (bit 0-4 red, 5-9 green, 10-14 blue) to RGBA8 bytes (alpha 255), n pixels; 5 bits widened as v << 3 | v >> 2. */
void gba_runtime_rgb555_to_rgba(const uint16_t *src, uint8_t *dst, size_t n);

/* The save file of a game: "<save_dir>/gba-<key>/game.sav". Returns 0, or -1 when it does not fit. */
int gba_runtime_save_path(const char *save_dir, const char *key, char *out, size_t out_n);

/* The GBA's frame period: 280896 cycles at 16.777216 MHz = 16742706 ns (59.7275 Hz). */
#define GBA_FRAME_NS 16742706ull

#ifdef __cplusplus
}
#endif

#endif /* XBR_GBA_RUNTIME_H */
