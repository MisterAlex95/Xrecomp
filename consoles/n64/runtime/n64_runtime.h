/* n64_runtime -- the Nintendo 64 runtime (XBA-146, XBA-174): the machine of n64_core.h played on the host layer
 * (xbr_host: host_window, host_pad, host_audio, recomp_lifecycle). The app shell's loader calls n64_runtime_start;
 * the desktop's n64_play calls n64_runtime_run. consoles/README.md « Playing an N64 game », docs/android/app-shell.md
 * « Nintendo 64 games », Plane « Spec · Other consoles ». */
#ifndef XBR_N64_RUNTIME_H
#define XBR_N64_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N64_RUNTIME_VERSION "0.1.0"

/* "n64_runtime <version>". */
const char *n64_runtime_version(void);

/* The accessory in each controller's slot (n64_runtime_opts.pak). */
enum { N64_RT_PAK_DEFAULT = -1, N64_RT_PAK_NONE = 0, N64_RT_PAK_CONTROLLER = 1, N64_RT_PAK_RUMBLE = 2 };

/* What n64_runtime_run plays and how. Zeroed = defaults (n64_runtime_opts_init). */
typedef struct n64_runtime_opts {
    /* The cartridge: a ROM file in any byte order (.z64, .v64, .n64: n64_rom_detect), or "fd:N" holding one (a
     * descriptor the app shell opened from a document; read from offset 0, left open). No BIOS: the PIF is HLE. */
    const char *rom;
    /* The game's key (NSME-635A2BFF, recomp_pack_rom_identify); NULL or "": n64_runtime_fallback_key. */
    const char *title_key;
    /* Saves: <save_dir>/n64-<KEY>/game.eep (EEPROM 4K: 512 B, 16K: 2 KB), game.sra (SRAM, 32 KB), game.fla
     * (FlashRAM, 128 KB) and pak1.mpk .. pak4.mpk (controller paks, 32 KB): raw images in the cartridge's byte order,
     * the files other emulators (mupen64plus, ares) read and write. Read before the first frame (a cartridge save
     * also fixes the save type); written a second after the game's last write (at most five seconds after its
     * first), when the app pauses and at exit; only an image that changed is written. NULL: no saves. */
    const char *save_dir;
    /* Stop after this many frames (VI interrupts; 0: until the player quits). */
    uint64_t frames;
    /* 1: no window (frames still run, pads and audio still open); also when host_window_headless(). */
    int headless;
    /* 1: no pacing, as fast as the host runs (tests). */
    int unpaced;
    /* Write the last displayed picture (its own size) as a binary PPM here at exit. */
    const char *screenshot;
    /* Capture the window's drawable (the real present path, host_window_capture) at this frame, to capture_path. */
    uint64_t capture_frame;
    const char *capture_path;
    /* Buttons held on controller 1 for the whole run, on top of the pads (N64_BTN_*, 1 = pressed; tests). */
    uint16_t buttons;
    /* Window scaling: -1 = $RECOMP_N64_SCALE ("integer" or "aspect", default aspect); else HOST_SCALE_*. Filter: -1 =
     * $RECOMP_N64_FILTER ("linear" or "nearest", default linear: the VI's 256..640-wide pictures are stretched to
     * 4:3); else HOST_FILTER_*. */
    int scale, filter;
    /* The accessory in the controllers: N64_RT_PAK_DEFAULT = $RECOMP_N64_PAK ("controller", "rumble" or "none";
     * default controller: the controller pak, saved as pakN.mpk); a rumble pak drives the host pad's rumble. */
    int pak;
    /* 1: every host pad drives controller 1 (a handheld's own controls and an external pad alike); 2..4: host pad N
     * -> controller N. -1 = $RECOMP_N64_PLAYERS (default 1). */
    int players;
    /* RDP scanline-band threads (n64_rdp_set_threads): -1 = $RECOMP_N64_RDP_THREADS, else 2 on Android and the core's
     * default elsewhere ($XBR_N64_RDP_THREADS, else 1); 1..N64_RDP_MAX_THREADS. */
    int rdp_threads;
    /* TV type: -1 from the ROM's region (default), 0 NTSC (60 Hz), 1 PAL (50 Hz). */
    int force_pal;
    /* 1: copy the game's IS-Viewer debug text (libdragon debugf, osSyncPrintf) to stdout, "n64: tty: ..." lines; -1 =
     * $RECOMP_N64_TTY (default off). */
    int tty;
    /* An N64 title pack's library (XBA-174, n64_pack.h): taken when its pack.json (beside it) says it is this game's
     * (title key, the ROM's big-endian SHA-256), this shell's and this host's and its table loads, else the game runs
     * interpreted (a log line says why). NULL or "": the interpreter alone. */
    const char *pack_lib;
    /* At exit (and from the in-game menu's Quit), the code the interpreter had to run while a pack was attached
     * (n64_recomp_write_missed: address, hash, bytes) is appended to this file without repeats, which the next
     * Prepare hands to the lifter (the discovery loop). NULL: not written. */
    const char *discovered_path;
} n64_runtime_opts;

void n64_runtime_opts_init(n64_runtime_opts *o);

/* Plays o->rom until the player quits (window closed, SDL_QUIT) or o->frames ran. Returns 0, or -1 when the ROM cannot
 * be read or the machine cannot start (why on stderr). Logs one "n64: ..." line per step on stdout. */
int n64_runtime_run(const n64_runtime_opts *o);

/* Where n64_runtime_start keeps saves (the app shell's <internal files>/save); NULL: none. Copied. */
void n64_runtime_set_save_dir(const char *dir);

/* Where n64_runtime_start writes the discovered code: <dir>/n64-<KEY>/discovered.txt (the app shell's
 * <internal files>/work: the Prepare's work folder of the game, which it reads them from); NULL: not written. */
void n64_runtime_set_work_dir(const char *dir);

/* The app shell's entry point (XBA-147, android/app/src/main/cpp/pack_loader.c): rom is the cartridge (a path or
 * "fd:N"), pack_lib a title pack's library (NULL or "" without a pack: the interpreter alone; a pack that is not this
 * game's: logged, interpreted), title_key the game's key. Plays it with n64_runtime_run (saves in
 * n64_runtime_set_save_dir's dir, discovered code in n64_runtime_set_work_dir's; $RECOMP_N64_FRAMES bounds the run)
 * and returns its result once the player quit. */
int n64_runtime_start(const char *rom, const char *pack_lib, const char *title_key);

/* ---- Pure helpers (tested in consoles/n64/tests) ---- */

/* The N64 buttons (N64_BTN_*, 1 = pressed) for an SDL game controller's state (host_pad.h: HOST_PAD_* buttons, right
 * stick -32768..32767 with +y down, triggers 0..32767). Xbox layout: A -> A, B and X -> B, Y -> C-up, LB -> L,
 * RB -> R, LT or RT past a quarter -> Z, Start -> Start, D-pad -> D-pad, the right stick past half its travel ->
 * the C buttons (up / down / left / right). Opposite directions at once cancel. */
uint16_t n64_runtime_pad_buttons(uint32_t buttons, int rx, int ry, int lt, int rt);
/* The N64 buttons for console_pad targets (bit N64_T_* of console_pad.h), opposite directions cancelled. The game
 * reads its pads through the player's mapping (RECOMP_N64_PAD_MAP, console_pad.h); n64_runtime_pad_buttons is that
 * mapping's defaults, the layout above. */
uint16_t n64_runtime_buttons_of(uint32_t targets);

/* An SDL left stick (-32768..32767 each, +y down) as the N64 stick (+y up), range = the value at full travel on an
 * axis (the real stick's gate is about +-80; 0: 80), dead = the radial dead zone in SDL units (-1: 8 % of the travel).
 * The diagonals reach about 0.85 x range on each axis, as the real stick's octagonal gate does. */
void n64_runtime_stick(int lx, int ly, int range, int dead, int8_t *x, int8_t *y);

/* 0x00RRGGBB pixels (n64_display) to RGBA8 bytes (alpha 255), n pixels. */
void n64_runtime_xrgb_to_rgba(const uint32_t *src, uint8_t *dst, size_t n);

/* A save file of a game: "<save_dir>/n64-<key>/<name>" where name is game.eep (both EEPROM kinds), game.sra,
 * game.fla, or pak1.mpk .. pak4.mpk for N64_SAVE_PAK0..3 (n64_core.h n64_save_kind). Returns 0, or -1 when it does not
 * fit, an argument is missing or the kind is N64_SAVE_NONE. */
int n64_runtime_save_path(const char *save_dir, const char *key, int kind, char *out, size_t out_n);

/* A key for a ROM recomp_pack_rom_identify could not name (the same formula: game code + header CRC1,
 * "NSME-635A2BFF"; any byte order). Returns 0, or -1 when the ROM is shorter than its header or out is too small
 * (16 bytes is enough). */
int n64_runtime_fallback_key(const uint8_t *rom, size_t n, char *out, size_t out_n);

/* The length of a frame of the core in ns, from the CPU cycles it ran (93.75 MHz), held between 1/75 and 1/45 s (a
 * first frame from reset, a mode change). NTSC about 16.7 ms (60 Hz), PAL 20 ms (50 Hz). */
uint64_t n64_runtime_frame_ns(uint64_t cycles);

/* The accessory a value names ("controller" / "pak" / "mempak", "rumble", "none"), or N64_RT_PAK_DEFAULT when it is
 * none of them (NULL too). */
int n64_runtime_pak_parse(const char *v);

#ifdef __cplusplus
}
#endif

#endif /* XBR_N64_RUNTIME_H */
