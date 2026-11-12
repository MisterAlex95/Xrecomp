/* Does the title want more frames than frame generation lets it have?
 * (XBA-96, T4)
 *
 * Frame generation (RECOMP_LSFG) shows two frames per real one and paces for
 * 60 Hz, so it caps the title at 30 fps: right for the titles that run at 30,
 * wrong for one whose game logic advances per flip at 60 (Burnout 3 ran in
 * slow motion, and its profile had to force RECOMP_LSFG=0). Seen from here,
 * such a title
 *   - asks for every vblank: its swaps (D3D software method 1) carry the
 *     presentation interval ONE (or DEFAULT), not TWO or more, and not
 *     IMMEDIATE;
 *   - and the cap holds it back: nearly every frame arrives before its 30 fps
 *     slot, so the executor sleeps at the cap.
 * A title that runs at 30 by itself (interval TWO, or frames slower than the
 * cap) is left alone, and so is a cap that is not frame generation's (a
 * title profile's RECOMP_GPU_FPS_CAP=30): the caller only asks while the cap
 * in force is the one frame generation lowered.
 *
 * Pure and header-only, so a test drives it with numbers. */
#ifndef XBOXRECOMP_FRAME_LOCK_H
#define XBOXRECOMP_FRAME_LOCK_H

#include <stdint.h>

#define FRAME_LOCK_WINDOW 180u          /* flips looked at: 6 s at the 30 cap */

typedef struct {
    uint32_t flips, held, swaps, every_vblank;
    int verdict;                        /* 0 undecided, 1 wants 60, -1 fine at the cap */
} FrameLock;

/* A swap's software-method payload (param >> 5): interval in bits 0-2, IMMEDIATE in bit 3. */
static inline void frame_lock_swap(FrameLock *f, uint32_t payload)
{
    uint32_t interval = payload & 7u;
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

#endif /* XBOXRECOMP_FRAME_LOCK_H */
