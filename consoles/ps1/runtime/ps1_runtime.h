/* ps1_runtime -- the PlayStation runtime (XBA-146, XBA-168): the machine of ps1_core.h played on the host layer
 * (xbr_host: host_window, host_pad, host_audio, recomp_lifecycle). The app shell's loader calls ps1_runtime_start;
 * the desktop's ps1_play calls ps1_runtime_run. consoles/README.md « Playing a PS1 game », docs/android/app-shell.md
 * « PlayStation games », Plane « Spec · Other consoles ». */
#ifndef XBR_PS1_RUNTIME_H
#define XBR_PS1_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS1_RUNTIME_VERSION "0.1.0"

/* "ps1_runtime <version>". */
const char *ps1_runtime_version(void);

/* What ps1_runtime_run plays and how. Zeroed = defaults (ps1_runtime_opts_init). */
typedef struct ps1_runtime_opts {
    /* The game: a disc image (.cue with its tracks, .bin, .iso, .chd: ps1_disc_open), a PS-X EXE (recognised by its
     * "PS-X EXE" magic), or "fd:N" holding either (a descriptor the app shell opened from a document: a single-file
     * disc image or an EXE; read from offset 0, left open). A disc boots through the BIOS (SYSTEM.CNF), an EXE is
     * fast-booted where the BIOS would enter its shell. */
    const char *game;
    /* A PS-X EXE to fast-boot with the disc of `game` in the drive (desktop tests); NULL: none. */
    const char *exe;
    /* The game's key (SLUS-00594, recomp_pack_rom_identify); NULL or "": a fallback from the content
     * (ps1_runtime_fallback_key). */
    const char *title_key;
    /* Memory cards: <save_dir>/ps1-<KEY>/card1.mcr and card2.mcr (128 KB raw images, the .mcr other emulators use),
     * read before the first frame (a missing one is a freshly formatted card), written a second after the game's
     * last write to the card (at most five seconds after its first), when the app pauses and at exit; only a card
     * that changed is written. NULL: no saves. */
    const char *save_dir;
    /* The 512 KB BIOS image: NULL = $RECOMP_PS1_BIOS, then ps1_runtime_set_bios's path, then the app's asset
     * ps1-bios/openbios.bin (Android), then build/ps1-bios/openbios.bin next to the working directory or an
     * ancestor of the executable (desktop); none of them: the HLE BIOS (ps1_hle.c, XBA-163). "hle" (here or in any
     * of those settings) asks for the HLE BIOS. OpenBIOS (MIT) only; never a Sony BIOS in the repository. */
    const char *bios;
    /* Stop after this many frames (0: until the player quits). */
    uint64_t frames;
    /* 1: no window (frames still run, pads and audio still open); also when host_window_headless(). */
    int headless;
    /* 1: no pacing, as fast as the host runs (tests). */
    int unpaced;
    /* Write the last displayed picture (its own size, 256..640 x 240..512) as a binary PPM here at exit. */
    const char *screenshot;
    /* Capture the window's drawable (the real present path, host_window_capture) at this frame, to capture_path. */
    uint64_t capture_frame;
    const char *capture_path;
    /* Buttons held on pad 1 for the whole run, on top of the pads (PS1_PAD_*, 1 = pressed; tests). */
    uint16_t buttons;
    /* Window scaling: -1 = $RECOMP_PS1_SCALE, else $RECOMP_SCALE ("aspect", "integer" or "stretch", default aspect;
     * host_window_env_scale); else HOST_SCALE_*. Filter: -1 = $RECOMP_PS1_FILTER, else $RECOMP_FILTER ("linear" or
     * "nearest", default linear: the PS1's widths 256..640 are stretched to 4:3); else HOST_FILTER_*. */
    int scale, filter;
    /* The pad plugged in port 1: -1 = $RECOMP_PS1_PAD ("analog" or "digital", default digital), 0 digital
     * (SCPH-1080; the left stick drives the D-pad too), 1 DualShock in analog mode (sticks -> analog). Back + L3 +
     * R3 together switch between the two while playing. */
    int analog;
    /* 1: every host pad drives PS1 port 1 (a handheld's own controls and an external pad alike); 2: host pad 0 ->
     * port 1, host pad 1 -> port 2 (two players). -1 = $RECOMP_PS1_PLAYERS (default 1). */
    int players;
    /* A PS1 title pack's library (XBA-168, ps1_pack.h): taken when its pack.json (beside it) says it is this game's
     * (title key, boot EXE), this shell's and this host's and its table loads, else the game runs interpreted (a log
     * line says why). NULL or "": the interpreter alone. */
    const char *pack_lib;
    /* At exit (and from the in-game menu's Quit), the code the interpreter had to run while a pack was attached
     * (ps1_recomp_write_missed: address, hash, bytes) is appended to this file without repeats, which the next
     * Prepare hands to the lifter (the discovery loop). NULL: not written. */
    const char *discovered_path;
    /* Fast boot of a disc (XBA-168): -1 = $RECOMP_PS1_FASTBOOT ("0" turns it off; on by default), 0 off, 1 on. On,
     * the BIOS's shell (OpenBIOS's own logo animation, about five seconds; there is no Sony logo) is skipped and the
     * BIOS boots the disc at once, SYSTEM.CNF and all (ps1_set_fast_boot). An EXE is always fast-booted. */
    int fast_boot;
    /* Tests and the desktop (ps1_play --swap): at frame swap_frame (> 0) the disc is changed to swap_disc as
     * ps1_runtime_change_disc does. */
    uint64_t swap_frame;
    const char *swap_disc;
} ps1_runtime_opts;

void ps1_runtime_opts_init(ps1_runtime_opts *o);

/* How the run shows its picture: o->scale / o->filter when set (>= 0), else $RECOMP_PS1_SCALE / _FILTER, else the
 * shared $RECOMP_SCALE / $RECOMP_FILTER (host_window_env_scale / _filter), else aspect and linear. o may be NULL. */
void ps1_runtime_display(const ps1_runtime_opts *o, int *scale, int *filter);

/* Plays o->game until the player quits (window closed, SDL_QUIT) or o->frames ran. Returns 0, or -1 when the game
 * or the BIOS cannot be read or the machine cannot start (why on stderr). Logs one "ps1: ..." line per step on
 * stdout. */
int ps1_runtime_run(const ps1_runtime_opts *o);

/* Where ps1_runtime_start keeps memory cards (the app shell's <internal files>/save); NULL: none. Copied. */
void ps1_runtime_set_save_dir(const char *dir);

/* Where ps1_runtime_start writes the discovered code: <dir>/ps1-<KEY>/discovered.txt (the app shell's
 * <internal files>/work: the Prepare's work folder of the game, which it reads them from); NULL: not written. */
void ps1_runtime_set_work_dir(const char *dir);

/* The BIOS image ps1_runtime_start uses when $RECOMP_PS1_BIOS is not set; NULL: the default search. Copied. */
void ps1_runtime_set_bios(const char *path);

/* The app shell's entry point (XBA-147, android/app/src/main/cpp/pack_loader.c): rom is the game (a path or
 * "fd:N"), pack_lib a title pack's library (NULL or "" without a pack: the interpreter alone; a pack that is not
 * this game's: logged, interpreted), title_key the game's key. Plays it with ps1_runtime_run (cards in
 * ps1_runtime_set_save_dir's dir, discovered code in ps1_runtime_set_work_dir's; $RECOMP_PS1_FRAMES bounds the
 * run) and returns its result once the player quit. */
int ps1_runtime_start(const char *rom, const char *pack_lib, const char *title_key);

/* ---- Multi-disc games (XBA-166) ---- */

#define PS1_RUNTIME_DISCS_MAX 8
#define PS1_RUNTIME_PATH_MAX 1024

/* Changes the disc of the game being played, from any thread (the app's in-game menu « Change disc »; on the
 * desktop F6 steps through a .m3u playlist): the lid opens (the game sees it: INT5, stat bit 4), a second later the
 * new disc is in and the lid closes. path is what ps1_disc_open takes (a .cue, .bin, .iso, .chd or "fd:N", which the
 * runtime leaves open); an image that does not open is logged and the disc stays. Returns 0 when queued, -1 when no
 * game is running or path is empty. */
int ps1_runtime_change_disc(const char *path);

/* The discs of a .m3u playlist (one image per line, relative to the playlist's folder unless absolute; blank lines
 * and '#' comments skipped; a UTF-8 BOM and CR line ends taken): at most max paths into out. Returns how many (0:
 * unreadable or empty). A game given as a .m3u plays its first disc. */
int ps1_runtime_m3u(const char *path, char out[][PS1_RUNTIME_PATH_MAX], int max);

/* ---- Pure helpers (tested in consoles/ps1/tests) ---- */

/* The PS1 buttons (PS1_PAD_*, 1 = pressed) for an SDL game controller's state (host_pad.h: HOST_PAD_* buttons,
 * left stick -32768..32767 with +y down, triggers 0..32767). Xbox layout: A -> Cross, B -> Circle, X -> Square,
 * Y -> Triangle, LB / RB -> L1 / R1, LT / RT past half -> L2 / R2, Start -> Start, Back -> Select, L3 / R3 (stick
 * clicks) -> L3 / R3, D-pad -> D-pad; digital (analog = 0): the left stick past a third of its travel -> D-pad too.
 * Opposite directions at once cancel. */
uint16_t ps1_runtime_pad_buttons(uint32_t buttons, int lx, int ly, int lt, int rt, int analog);
/* The PS1 buttons for a pad read through console_pad (console_pad.h: bit PS1_T_* of o->buttons; digital: the mapped
 * left stick past stick_threshold is the D-pad too), opposite directions cancelled. The game reads its pads through
 * the player's mapping (RECOMP_PS1_PAD_MAP); ps1_runtime_pad_buttons is that mapping's defaults, the layout above. */
struct ConsolePadOut;
uint16_t ps1_runtime_pad_of(const struct ConsolePadOut *o, int stick_threshold, int analog);

/* An SDL stick axis (-32768..32767) as the DualShock's byte (0..255, 0x80 = centre; +y down on both). */
uint8_t ps1_runtime_axis(int v);

/* 0x00RRGGBB pixels (ps1_display) to RGBA8 bytes (alpha 255), n pixels. */
void ps1_runtime_xrgb_to_rgba(const uint32_t *src, uint8_t *dst, size_t n);

/* The memory card file of a game: "<save_dir>/ps1-<key>/card<slot + 1>.mcr". Returns 0, or -1 when it does not
 * fit or an argument is missing. */
int ps1_runtime_card_path(const char *save_dir, const char *key, int slot, char *out, size_t out_n);

/* 1 when the first bytes of a file are a PS-X EXE's magic ("PS-X EXE"). */
int ps1_runtime_is_exe(const uint8_t *head, size_t n);

/* A key for a game recomp_pack_rom_identify could not name: "EXE-<CRC32 of the EXE>" for an EXE, else
 * "PSX-<CRC32 of the disc's sector 16 (its ISO 9660 volume descriptor), the 2048 user bytes>"; "PSX-UNKNOWN" when
 * neither can be read. Returns 0, or -1 when out is too small (24 bytes is enough). */
int ps1_runtime_fallback_key(const uint8_t *exe, size_t exe_n, const char *disc, char *out, size_t out_n);

/* The length of a frame of the core in ns, from the CPU cycles it ran (33.8688 MHz), held between 1/75 and 1/45 s
 * (a first frame from reset, a mode change). NTSC about 16.7 ms (59.94 / 59.29 Hz), PAL 20 ms (50 Hz). */
uint64_t ps1_runtime_frame_ns(uint64_t cycles);

#ifdef __cplusplus
}
#endif

#endif /* XBR_PS1_RUNTIME_H */
