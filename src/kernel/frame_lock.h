/* Frame generation and titles locked to the flip rate (XBA-96, T4; WP4).
 *
 * Frame generation (RECOMP_LSFG) shows two frames per real one and paces for
 * 60 Hz, so it caps the title at 30 fps: right for the titles that run at 30,
 * wrong for one whose game logic advances per flip at 60 (Burnout 3 ran in
 * slow motion, and its profile had to force RECOMP_LSFG=0).
 *
 * Two ways to find such a title, both pure and header-only (a test drives
 * them with numbers):
 *
 *  1. The gate (frame_lock_gate, the default under RECOMP_PACING=auto):
 *     frame generation waits for the pacer's lock verdict (frame_pacing.h,
 *     judged at the title's own rate, 20 s after its first flip) and the
 *     cap is not lowered before it. A title judged locked to an ask above
 *     frame generation's cap (Burnout 3: interval ONE, holds 60) is held
 *     without frame generation while it asks for that; a free title, or one
 *     locked to an ask the cap does not divide (interval TWO: 30, which
 *     Fable asks in its world after a front end at ONE), gets it from then
 *     on.
 *
 *  2. The watch (FrameLock, under a fixed cap, RECOMP_PACING=off): the cap
 *     is lowered from the first frame, and the title is watched under it.
 *     Such a title
 *       - asks for every vblank: its swaps (D3D software method 1) carry the
 *         presentation interval ONE (or DEFAULT), not TWO or more, and not
 *         IMMEDIATE;
 *       - and the cap holds it back: nearly every frame arrives before its
 *         30 fps slot, so the executor sleeps at the cap.
 *     A title that runs at 30 by itself (interval TWO, or frames slower than
 *     the cap) is left alone, and so is a cap that is not frame generation's
 *     (RECOMP_GPU_FPS_CAP=30): the caller only asks while the cap in force
 *     is the one frame generation lowered. */
#ifndef XBOXRECOMP_FRAME_LOCK_H
#define XBOXRECOMP_FRAME_LOCK_H

#include <stdint.h>

#define FRAME_LOCK_SKIP   600u          /* flips left out first: logos and movies (20 s at 30) */
#define FRAME_LOCK_WINDOW 180u          /* flips looked at then: 6 s at the 30 cap */

typedef struct {
    uint32_t skipped;                   /* flips before the window */
    uint32_t flips, held, swaps, every_vblank;
    int verdict;                        /* 0 undecided, 1 wants 60, -1 fine at the cap */
} FrameLock;

/* A swap's software-method payload (param >> 5): interval in bits 0-2, IMMEDIATE in bit 3. */
static inline void frame_lock_swap(FrameLock *f, uint32_t payload)
{
    uint32_t interval = payload & 7u;
    if (f->skipped < FRAME_LOCK_SKIP)
        return;
    f->swaps++;
    if (!(payload & 8u) && interval <= 1u)
        f->every_vblank++;
}

/* A flip; held = the executor slept at the cap before it. Returns the
 * verdict, decided once FRAME_LOCK_WINDOW flips are in (and then kept). */
static inline int frame_lock_flip(FrameLock *f, int held)
{
    if (f->verdict)
        return f->verdict;
    /* Logos and movies are presented every vblank and held by the cap
     * whatever the game itself does: judge the game, after them. */
    if (f->skipped < FRAME_LOCK_SKIP) {
        f->skipped++;
        return 0;
    }
    f->flips++;
    f->held += held ? 1u : 0u;
    if (f->flips < FRAME_LOCK_WINDOW)
        return 0;
    f->verdict = (f->swaps >= FRAME_LOCK_WINDOW / 2u
                  && f->every_vblank * 10u >= f->swaps * 9u     /* 90 % of swaps every vblank */
                  && f->held * 10u >= f->flips * 9u)            /* 90 % of frames held by the cap */
               ? 1 : -1;
    return f->verdict;
}

/* The gate (1 above). verdict: the pacer's, 0 undecided, 1 locked to its
 * ask, -1 free; ask_fps: the rate the title asks for; fg_cap: frame
 * generation's cap (display rate / multiplier). */
#define FRAMEGEN_GATE_WAIT   0          /* no verdict yet: frames at the title's rate, no generation */
#define FRAMEGEN_GATE_START  1          /* frame generation on, the cap lowered to fg_cap */
#define FRAMEGEN_GATE_HOLD   (-1)       /* locked above fg_cap: no frame generation while the ask stays there */

static inline int frame_lock_gate(int verdict, uint32_t ask_fps, uint32_t fg_cap)
{
    if (!verdict)
        return FRAMEGEN_GATE_WAIT;
    if (verdict > 0 && fg_cap && ask_fps > fg_cap)
        return FRAMEGEN_GATE_HOLD;
    return FRAMEGEN_GATE_START;
}

#endif /* XBOXRECOMP_FRAME_LOCK_H */
