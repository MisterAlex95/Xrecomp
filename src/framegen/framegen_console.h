/* framegen_console.h -- frame generation for the console window (XBA-186).
 *
 * The GBA, PS1 and N64 runtimes present through host_window (src/video/host_window.c). Under RECOMP_LSFG=1, in a
 * build with XBOXRECOMP_LSFG, host_window hands each picture here instead of drawing it, and draws what comes out of
 * the timed FIFO at its due time (docs/technical/framegen.md « Console window »). C, libc + pthreads, no SDL or GL:
 * the tests drive it with the fake generator and a fake clock.
 *
 * What it adds to the pacer (framegen_pacer.h):
 *  - Distinct frames. A console presents at its field rate (about 60 Hz) whatever the game draws: a 30 fps game
 *    presents each picture twice, a 20 fps one three times. Each picture is hashed (64-bit, every byte); only a
 *    picture that differs from the previous one is a real frame for the pacer, at the time it was presented, so the
 *    pacer's period is the game's (33.3 ms at 30 fps) and the generated frames fill the repeats: 60 distinct
 *    pictures a second on a 60 Hz display at 2x. A repeat draws nothing (FGC_REPEAT): the FIFO has the frame for
 *    that slot.
 *  - Pass-through. A game that already draws a new picture every field (the mean of its last 8 distinct intervals
 *    below 3/4 of the cap period: above 40 fps at a 30 fps cap) gains nothing: its pictures are drawn as they come
 *    (FGC_DIRECT) until it slows down again, then the pacer starts from a fresh history.
 *  - Bypass and reset: fgc_frame's flags. FGC_BYPASS (fast-forward): drawn as they come, nothing reaches the pacer;
 *    the first frame after it starts a fresh history. FGC_RESET (a state was loaded): a fresh history from this
 *    frame. fgc_reset (the app paused, the menu): the same, from any thread.
 *  - Size cap. A picture wider than max_width (RECOMP_LSFG_MAX_WIDTH, default 1280) is box-filtered by a whole
 *    factor before the pacer (2560 x 1920 at a 4x internal resolution -> 1280 x 960): every frame shown while
 *    frame generation runs, real or generated, is at that size.
 *  - The FIFO: 4 entries of RGBA pixels (row 0 at the top) with their due time; fgc_take gives the newest due one.
 *
 * Latency: a real frame is shown (M - 1) / M of the game's frame period after it was presented, plus the
 * generation cost c (G at T + c, N at T + c + P / M): 16.7 ms + c for a 30 fps game at 2x, 25 ms + c at 20 fps.
 *
 * fgc_frame, fgc_take, fgc_next_due and fgc_wait from the presenting thread; fgc_reset from any. */
#ifndef FRAMEGEN_CONSOLE_H
#define FRAMEGEN_CONSOLE_H

#include "framegen_pacer.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* fgc_frame's result: what the caller does with the picture. */
enum {
    FGC_DIRECT = 0,   /* draw it now, as without frame generation (bypass, pass-through, fallen back, not running) */
    FGC_QUEUED = 1,   /* a distinct frame, handed to the pacer: draw nothing, the FIFO brings it */
    FGC_REPEAT = 2,   /* the same picture as the last one: draw nothing, the FIFO fills the slot */
};

/* fgc_frame's flags. */
enum {
    FGC_BYPASS = 1u,  /* fast-forward: draw directly, the pacer sees nothing */
    FGC_RESET = 2u,   /* a state was loaded: forget the history, this frame primes the generator */
};

#define FGC_DEFAULT_MAX_WIDTH 1280u

typedef struct FramegenConsoleConfig {
    FramegenPacerConfig pacer;  /* gen, multiplier, cap_fps, budget, threaded, clock (framegen_pacer.h) */
    uint32_t max_width;         /* 0: FGC_DEFAULT_MAX_WIDTH */
} FramegenConsoleConfig;

/* Starts the pacer with the FIFO as its sink. 0, or -1 (pacer already running, no generator). */
int fgc_start(const FramegenConsoleConfig *cfg);

/* From the environment, as framegen_pacer_start reads it but for a console: RECOMP_LSFG must be on; the generator
 * (RECOMP_LSFG_FAKE=1: the fake one; else the registered bridge), RECOMP_LSFG_MULT, RECOMP_LSFG_BUDGET_MS,
 * RECOMP_LSFG_MAX_WIDTH; the cap is display rate / multiplier (framegen_policy_cap_rule with no cap: a console's
 * rate is its own, never lowered), display rate = RECOMP_LSFG_DISPLAY_HZ, else display_hz when it is below 60, else
 * 60. 0 = started; -1 = off ("[LSFG] off: ..." logged unless not requested). */
int fgc_start_env(int display_hz);

int fgc_running(void);
void fgc_stop(void);

/* One picture as the console presents it: RGBA8 (R, G, B, A bytes), w x h, pitch bytes per row, row 0 at the top;
 * now_ns: CLOCK_MONOTONIC (the config's clock in tests). FGC_DIRECT / FGC_QUEUED / FGC_REPEAT. */
int fgc_frame(const uint8_t *rgba, int w, int h, int pitch, uint64_t now_ns, unsigned flags);

typedef struct FgcShown {
    const uint8_t *px;   /* RGBA8, packed (pitch w * 4), row 0 at the top; valid until the next take or stop */
    int w, h;
    int generated;       /* 1: a generated frame (never kept for a screenshot) */
    uint64_t due_ns;
} FgcShown;

/* The newest entry due at now_ns (within a millisecond counts as due); older due entries are skipped (late).
 * 1 with *out filled, 0 when nothing is due. */
int fgc_take(uint64_t now_ns, FgcShown *out);

/* The next entry's due time, 0 when the FIFO is empty. */
uint64_t fgc_next_due(void);

/* Blocks until CLOCK_MONOTONIC until_ns or until a new entry reaches the FIFO, whichever comes first. */
void fgc_wait(uint64_t until_ns);

/* Forget the history (pacer reset, FIFO cleared): the app paused or resumed, the in-game menu. Any thread. */
void fgc_reset(void);

typedef struct FgcCounters {
    uint64_t frames;       /* fgc_frame calls */
    uint64_t distinct;     /* pictures that differed from the previous one */
    uint64_t repeats;      /* the same picture again (FGC_REPEAT, or drawn directly in pass-through / bypass) */
    uint64_t queued;       /* handed to the pacer */
    uint64_t direct;       /* FGC_DIRECT */
    uint64_t downscaled;   /* queued frames box-filtered to max_width */
    uint64_t fifo_in, fifo_full, taken, taken_generated, late;
    uint64_t resets;       /* fresh histories: fgc_reset, FGC_RESET, after a bypass, leaving pass-through */
    uint64_t passthrough_on;  /* times pass-through started */
    int passthrough;       /* 1 while in pass-through */
    int bypassed;          /* 1 while the last frame was FGC_BYPASS */
    uint32_t distinct_period_ns;  /* mean of the last distinct intervals (0: fewer than two) */
    uint32_t max_width;
} FgcCounters;

void fgc_counters(FgcCounters *out);

/* The 64-bit hash fgc_frame compares pictures with (w * 4 bytes of each row; tests). */
uint64_t fgc_hash(const uint8_t *px, int w, int h, int pitch);

#ifdef __cplusplus
}
#endif

#endif /* FRAMEGEN_CONSOLE_H */
