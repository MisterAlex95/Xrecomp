/* recomp_rewind -- rewind for the console runtimes (XBA-194): hold a hotkey and the game goes back in time.
 *
 * The ring: a runtime captures its machine (gba_state_save, ps1_state_save, n64_state_save: the save-state blob of
 * XBA-182 / XBA-183) every few frames and pushes it here. The newest capture is held whole; every older one is kept
 * as the XOR of itself with the capture after it, run-length coded (runs of zero bytes skipped, the rest stored as
 * is): two captures a few frames apart differ in a few pages of RAM, so a 400 KB GBA state takes a few KB and a 3 MB
 * PS1 state some tens of KB. Stepping back decodes one delta onto the newest (pop), so every step costs the same
 * whatever the ring's depth, and dropping the oldest capture (to stay under the memory cap) frees one delta and
 * touches nothing else: these backward deltas need no keyframes. Captures of different sizes (an N64 state leaves
 * zero RDRAM pages out) code the same way: the XOR runs over the older capture's length, the newer one's bytes past
 * its end counted as zero.
 *
 * Delta format (recomp_rewind_delta_encode): varint older length, then tokens until that length is covered: varint
 * count of zero bytes (skipped), varint count of literal bytes, the literal bytes (the XOR). A literal run ends at
 * the first eight zero bytes.
 *
 * The memory cap (budget) counts everything the ring holds: the deltas, the newest capture's buffer and the coding
 * buffer. After a push the oldest deltas go until the ring fits; the newest capture always stays. The coding buffer
 * is only as large as the deltas need (XBA-278: it was a worst case, 1.125 times a capture, so a 40 MB GameCube
 * state took the whole 64 MB with the newest and no history stayed), and a capture enters the ring by a swap of
 * buffers (recomp_rewind_push_swap), not a copy.
 *
 * What the cap does not count (HOST-002): the spare capture buffer, the one recomp_rewind_push_swap hands back for the
 * next capture. So a runtime really holds the history, up to RECOMP_REWIND_MB, plus one spare capture outside it: a
 * capture's size and 12.5 % (recomp_rewinder_frame's room for a capture that grows; without it, a capture growing a
 * little each time takes the slow path every time, and that path's cost escapes the spacing below). A 41 MB GameCube
 * capture at the default 64 MB: about 110 MB in all.
 *
 * The runtimes' side (recomp_rewinder): the settings (RECOMP_REWIND, RECOMP_REWIND_MB, RECOMP_REWIND_FRAMES), the
 * capture schedule (every N console frames, at most one per shown frame, so fast-forward never captures more than
 * one per display frame), the steps while the hotkey is held (one capture back per shown frame), the cost measured
 * (ms per capture, logged at the end of the run).
 *
 * libc only; not thread-safe (the runtime's frame thread owns its ring). */
#ifndef XBR_RECOMP_REWIND_H
#define XBR_RECOMP_REWIND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The ring ------------------------------------------------------------------------------------------------- */

typedef struct recomp_rewind recomp_rewind;

/* A ring holding at most budget bytes (see above). NULL when out of memory. */
recomp_rewind *recomp_rewind_create(size_t budget);
void recomp_rewind_destroy(recomp_rewind *r);
/* Forgets every capture (keeps the buffers). */
void recomp_rewind_clear(recomp_rewind *r);

/* Pushes a capture (n > 0 bytes): it becomes the newest. Returns 0, or -1 (out of memory: the ring is as before). */
int recomp_rewind_push(recomp_rewind *r, const uint8_t *state, size_t n);
/* recomp_rewind_push without the copy (XBA-278): the capture in *buf (n bytes of *cap) becomes the ring's newest and
 * *buf / *cap come back as the previous newest's buffer (NULL / 0 after the first push), its contents undefined: the
 * next capture's buffer. Returns 0, or -1 (out of memory: the ring and *buf are as before). */
int recomp_rewind_push_swap(recomp_rewind *r, uint8_t **buf, size_t *cap, size_t n);
/* The newest capture (valid until the next push / pop / clear), NULL when the ring is empty. */
const uint8_t *recomp_rewind_top(const recomp_rewind *r, size_t *n);
/* Drops the newest capture: the one before it becomes the newest. Returns 0, or -1 when fewer than two are held (the
 * last one stays) or a delta does not decode (the ring is cleared down to the newest then). */
int recomp_rewind_pop(recomp_rewind *r);

size_t recomp_rewind_count(const recomp_rewind *r);   /* captures held (0 when empty) */
size_t recomp_rewind_bytes(const recomp_rewind *r);   /* memory held (what the budget counts: not the spare buffer) */
size_t recomp_rewind_budget(const recomp_rewind *r);
uint64_t recomp_rewind_dropped(const recomp_rewind *r); /* oldest captures dropped for the budget so far */

/* ---- The codec (the ring's; tests) ---------------------------------------------------------------------------- */

/* The most bytes recomp_rewind_delta_encode writes for an older capture of n bytes. */
size_t recomp_rewind_delta_bound(size_t n);
/* The delta that turns newer into older (older_n bytes, newer_n bytes) into out (recomp_rewind_delta_bound(older_n)
 * bytes). Returns its size. */
size_t recomp_rewind_delta_encode(const uint8_t *older, size_t older_n, const uint8_t *newer, size_t newer_n,
                                  uint8_t *out);
/* The older length a delta gives, or 0 when it is not a delta. */
size_t recomp_rewind_delta_size(const uint8_t *delta, size_t delta_n);
/* Turns buf (the newer capture, *n bytes, cap bytes of room) into the older one; *n becomes its length. Returns 0, or
 * -1 when the delta is corrupt or the older capture does not fit in cap (buf is then undefined). */
int recomp_rewind_delta_apply(uint8_t *buf, size_t *n, size_t cap, const uint8_t *delta, size_t delta_n);

/* ---- The runtimes' side --------------------------------------------------------------------------------------- */

#define RECOMP_REWIND_MB_DEFAULT 64
#define RECOMP_REWIND_MB_MIN 8
#define RECOMP_REWIND_MB_MAX 1024

typedef struct recomp_rewind_cfg {
    int on;
    int interval;    /* console frames between captures */
    size_t budget;   /* bytes */
} recomp_rewind_cfg;

/* A console's defaults (every N frames: gba / 2, ps1 / 3, n64 and gc / 4; all on since XBA-278: the coding buffer no
 * longer takes a GameCube capture's room and a costly capture spaces itself out), then RECOMP_REWIND (0 / 1),
 * RECOMP_REWIND_MB (8..1024, default 64) and RECOMP_REWIND_FRAMES (1..60) over them. */
void recomp_rewind_config(const char *console, recomp_rewind_cfg *cfg);

/* XBA-278: a capture may cost the frame thread this much per console frame on average (1 ms: 6 % of a 60 Hz frame);
 * a console whose captures cost more (a GameCube game filling its 40 MB) gets them further apart. */
#define RECOMP_REWIND_COST_NS 1000000ull

typedef struct recomp_rewinder {
    recomp_rewind *ring;   /* NULL: rewind off */
    char console[8];
    int interval;          /* console frames between captures: the setting's, or more when captures cost more */
    int interval_set;      /* the setting's */
    uint64_t cost_avg_ns;  /* a capture's cost, averaged (1/8 per capture) */
    int since;             /* console frames since the last capture */
    int captured;          /* a capture was taken during this shown frame */
    int active;            /* the hotkey was held at the last shown frame */
    uint8_t *buf;          /* the capture buffer */
    size_t buf_cap;
    uint64_t captures, steps, failed;
    uint64_t cap_ns, cap_ns_max, cap_raw;   /* time in captures (save + push), the largest, raw bytes captured */
} recomp_rewinder;

/* Sets up from recomp_rewind_config(console) (cfg NULL) or cfg, and logs one line "<console>: rewind ...". Returns
 * 1 when rewind is on. */
int recomp_rewinder_init(recomp_rewinder *w, const char *console, const recomp_rewind_cfg *cfg);
/* Logs the cost ("<console>: rewind: N captures, x.xx ms each (max y.yy), ...") and frees everything. */
void recomp_rewinder_free(recomp_rewinder *w);
int recomp_rewinder_on(const recomp_rewinder *w);

/* Once per shown frame, before running any console frame: held = the rewind hotkey. Returns 1 when this shown frame
 * steps back (recomp_rewinder_step, no console frame runs), 0 to play. Logs when the rewind starts and ends. */
int recomp_rewinder_begin(recomp_rewinder *w, int held);

/* The machine's capture: writes it into out (cap bytes), returns its size (0: failed). The runtime's frame lock is
 * held. */
typedef size_t (*recomp_rewind_save_fn)(uint8_t *out, size_t cap, void *user);
/* The most bytes the capture may take now (frame lock held; called only when a capture is due). */
typedef size_t (*recomp_rewind_size_fn)(void *user);
/* Restores a capture into the machine; 0 when it did. The runtime's frame lock is held. */
typedef int (*recomp_rewind_load_fn)(const uint8_t *in, size_t n, void *user);

/* After each console frame (frame lock held): captures when one is due (the save into the last capture's buffer; the
 * size first only when it did not fit; the time of both is the capture's cost). Returns 1 when it captured. The
 * captures stay RECOMP_REWIND_COST_NS per console frame on average: when they cost more, the interval grows (up to
 * 60 frames) and comes back down when they get cheaper; logged when it changes. */
int recomp_rewinder_frame(recomp_rewinder *w, recomp_rewind_size_fn size, recomp_rewind_save_fn save, void *user);
/* One step back (frame lock held): restores the newest capture, then drops it unless it is the last. Returns 0, or
 * -1 when nothing is held or the load failed. */
int recomp_rewinder_step(recomp_rewinder *w, recomp_rewind_load_fn load, void *user);

/* ---- The round trip check (gba_run / ps1_run / n64_run --rewind, the unit test) -------------------------------- */

typedef struct recomp_rewind_machine {
    void *m;
    void (*input)(void *m, long frame);   /* the pads for frame (1..N), the same for the same frame */
    void (*run)(void *m);                 /* one frame */
    uint64_t (*picture)(void *m);         /* a hash of the picture shown now */
    uint64_t (*audio)(void *m);           /* a hash of the audio made since the last call (taken out) */
    recomp_rewind_size_fn size;           /* user = m */
    recomp_rewind_save_fn save;           /* user = m */
    recomp_rewind_load_fn load;
} recomp_rewind_machine;

/* Plays frames 1..N with input(f), hashing each frame's picture and audio and capturing through a recomp_rewinder
 * (every interval frames, budget bytes); then steps back `back` times as the runtime does while the hotkey is held,
 * checks the picture equals the one recorded after the frame of the capture reached, and plays on to frame N with the
 * same inputs: every frame's picture and audio must equal the first run's. Prints one line ("REWIND OK ..." with the
 * capture cost, or "REWIND FAIL ..."). Returns 0, 1 on a difference, 2 on no memory / nothing captured. */
int recomp_rewind_check(const char *console, const recomp_rewind_machine *mc, long frames, int interval, int back,
                        size_t budget);

#ifdef __cplusplus
}
#endif

#endif /* XBR_RECOMP_REWIND_H */
