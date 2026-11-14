/* recomp_states -- save states and fast-forward for the console runtimes (XBA-182).
 *
 * One interface the app shell (android/app/src/main/cpp/menu_jni.c, the in-game menu) and the desktop players
 * (gba_play, ps1_play: F1-F4 / Shift+F1-F4 / Tab) call, whatever console runs. A console runtime (gba_runtime,
 * ps1_runtime; n64_runtime can plug in the same way) attaches itself while a game plays: its console name, its
 * state folder (<save dir>/<console>-<KEY>) and two callbacks that save / load a slot between frames (the runtime
 * holds its frame mutex in them, so a state is never taken mid-frame). Xbox games never attach: their state is the
 * whole guest memory and GPU, too large to snapshot.
 *
 * Slot files: <dir>/state<N>.st, N = 1..RECOMP_STATE_SLOTS. A small header (magic "XRSLOT", version, console, game
 * key, time saved, frames run), a thumbnail of the last frame (RGB565, at most RECOMP_STATE_THUMB_W x _H), then the
 * core's own blob (gba_state_save / ps1_state_save: its own header checks the game and format version, so a slot from
 * another game or another build is refused by the core too). Written through <path>.tmp + rename.
 *
 * Fast-forward: a toggle (the menu's « Fast-forward », a desktop key) and a hold (Tab on a desktop, the pad chord
 * Back + right trigger), either one makes recomp_ff_active true. The speed: RECOMP_FF_SPEED = 2..16 (times the
 * console's rate) or "max" (as fast as the host runs, audio muted); default 4. Runtimes run `speed` console frames
 * per shown frame and pass the audio through recomp_ff_audio (averaged down: higher pitched, never gapped).
 *
 * libc + pthreads (xbr_host_core); every function may be called from any thread. */
#ifndef XBR_RECOMP_STATES_H
#define XBR_RECOMP_STATES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_STATE_SLOTS 4
#define RECOMP_STATE_THUMB_W 160
#define RECOMP_STATE_THUMB_H 120
#define RECOMP_STATE_SLOT_VERSION 1

/* Results (the in-game menu shows a message per code). */
enum {
    RECOMP_STATE_OK = 0,
    RECOMP_STATE_ENOGAME = -1,   /* no console game attached (an Xbox game, or none) */
    RECOMP_STATE_EEMPTY = -2,    /* nothing saved in that slot */
    RECOMP_STATE_EGAME = -3,     /* the slot holds another game's state */
    RECOMP_STATE_EVERSION = -4,  /* saved by another format version / build: not loadable */
    RECOMP_STATE_EIO = -5,       /* cannot write or read the file */
    RECOMP_STATE_ESLOT = -6,     /* slot out of range */
    RECOMP_STATE_ECORRUPT = -7,  /* not a state file, or truncated */
};

/* "ok", "no console game", "empty slot", ... (logs). */
const char *recomp_states_error(int code);

/* ---- The runtime's side --------------------------------------------------------------------------------------- */

typedef struct recomp_state_ops {
    const char *console;                 /* "gba", "ps1" (copied) */
    int (*save)(int slot, void *user);   /* slot 1..RECOMP_STATE_SLOTS; RECOMP_STATE_* */
    int (*load)(int slot, void *user);
    void *user;
} recomp_state_ops;

/* Attaches the running game (dir: its state folder, created on the first save; NULL or "": states off, e.g. no
 * save dir). A second attach replaces the first. detach: the game ended (waits for a save / load in progress). */
void recomp_states_attach(const recomp_state_ops *ops, const char *dir);
void recomp_states_detach(void);

/* "<dir>/state<slot>.st". Returns 0, or -1 (bad slot, no dir, too long). */
int recomp_states_slot_path(const char *dir, int slot, char *out, size_t out_n);

typedef struct recomp_state_meta {
    char console[8];
    char key[48];
    int64_t saved_ms;      /* Unix time in ms */
    uint64_t frames;       /* frames the run had played (informative) */
    int thumb_w, thumb_h;  /* 0 x 0: no thumbnail */
} recomp_state_meta;

/* Writes a slot file: meta (saved_ms 0: now), the thumbnail (thumb_w x thumb_h RGB565, may be NULL), the core blob.
 * Returns RECOMP_STATE_OK or RECOMP_STATE_EIO. */
int recomp_states_write(const char *path, const recomp_state_meta *meta, const uint16_t *thumb, const uint8_t *blob,
                        size_t blob_n);
/* Reads a slot file: meta, the thumbnail into thumb (RECOMP_STATE_THUMB_W * _H entries; may be NULL), the blob into
 * a malloc'ed *blob (may be NULL: header only). Returns RECOMP_STATE_OK, _EEMPTY (no file), _ECORRUPT, _EVERSION. */
int recomp_states_read(const char *path, recomp_state_meta *meta, uint16_t *thumb, uint8_t **blob, size_t *blob_n);

/* A thumbnail of an RGBA8 picture (w x h, stride bytes per row), box-filtered to fit RECOMP_STATE_THUMB_W x _H at
 * the picture's display aspect (aspect_w:aspect_h, e.g. 3:2 for the GBA, 4:3 for the PS1; 0:0 = w:h). */
void recomp_states_thumb(const uint8_t *rgba, int w, int h, int stride, int aspect_w, int aspect_h, uint16_t *out,
                         int *out_w, int *out_h);

/* ---- The UI's side (the in-game menu, desktop keys) ------------------------------------------------------------- */

/* 1 while a console game that supports states is attached with a state folder. */
int recomp_states_available(void);
/* The attached console ("gba", "ps1"), or "" when none. */
const char *recomp_states_console(void);

/* Saves / loads a slot (1..RECOMP_STATE_SLOTS) of the attached game, between its frames. RECOMP_STATE_*. */
int recomp_states_save(int slot);
int recomp_states_load(int slot);
/* States loaded so far (successful recomp_states_load calls), from any thread: the console window resets its frame
 * generation history when it changes (XBA-186), so no frame from before the load is blended with one after it. */
unsigned recomp_states_loads(void);

/* What a slot holds: RECOMP_STATE_OK with meta (and the thumbnail when thumb is not NULL), else _EEMPTY /
 * _ECORRUPT / _EVERSION / _ENOGAME. */
int recomp_states_info(int slot, recomp_state_meta *meta, uint16_t *thumb);

/* ---- Fast-forward --------------------------------------------------------------------------------------------- */

void recomp_ff_set(int on);       /* the toggle */
int recomp_ff_toggled(void);
void recomp_ff_hold(int held);    /* the hold (key or pad chord), from the runtime's own loop */
int recomp_ff_active(void);       /* toggle || hold */
/* Console frames per shown frame: RECOMP_FF_SPEED (2..16, default 4), or 0 for "max" (unpaced). Read once. */
int recomp_ff_speed(void);
/* Resets the speed so RECOMP_FF_SPEED is read again (tests). */
void recomp_ff_reset(void);

/* Fast-forward audio: averages each `factor` stereo frames of in (n frames) into one, in place; returns the frames
 * left (n / factor, the remainder carried to the next call through *carry, which the caller zeroes when FF starts).
 * factor <= 1: n unchanged. */
typedef struct recomp_ff_carry {
    int32_t sum[2];
    int n;
} recomp_ff_carry;
size_t recomp_ff_audio(int16_t *stereo, size_t n, int factor, recomp_ff_carry *carry);

#ifdef __cplusplus
}
#endif

#endif /* XBR_RECOMP_STATES_H */
