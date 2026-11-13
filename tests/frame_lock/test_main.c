/* XBA-96: frame generation goes off only for a title that asks for every
 * vblank and is held back by the 30 fps cap on nearly every frame.
 * WP4: the gate, from the pacer's lock verdict. */
#include "frame_lock.h"
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* n frames, one swap each with `payload`, `held_pct` % of them held at the cap. */
static int run(uint32_t payload, int held_pct, uint32_t n)
{
    FrameLock f = { 0 };
    int v = 0;
    for (uint32_t i = 0; i < FRAME_LOCK_SKIP; i++) {    /* logos, movies: not judged */
        frame_lock_swap(&f, 1u);
        if (frame_lock_flip(&f, 1))
            return 99;
    }
    for (uint32_t i = 0; i < n; i++) {
        frame_lock_swap(&f, payload);
        v = frame_lock_flip(&f, (int)(i % 100u) < held_pct);
    }
    return v;
}

int main(void)
{
    CHECK(run(1u, 100, FRAME_LOCK_WINDOW - 1u) == 0);        /* undecided before the window */
    CHECK(run(1u, 100, FRAME_LOCK_WINDOW) == 1);             /* Burnout-like: interval ONE, always held */
    CHECK(run(0u, 95, 400) == 1);                            /* DEFAULT counts as ONE */
    CHECK(run(2u, 100, 400) == -1);                          /* interval TWO: a 30 fps title */
    CHECK(run(1u | 8u, 100, 400) == -1);                     /* IMMEDIATE: not vblank-locked */
    CHECK(run(1u, 40, 400) == -1);                           /* frames slower than the cap: fine at 30 */
    {
        FrameLock f = { 0 };                                 /* no swap methods seen at all */
        int v = 0;
        for (uint32_t i = 0; i < FRAME_LOCK_SKIP + 400u; i++)
            v = frame_lock_flip(&f, 1);
        CHECK(v == -1);
    }
    {
        FrameLock f = { 0 };                                 /* the verdict is kept */
        for (uint32_t i = 0; i < FRAME_LOCK_SKIP + FRAME_LOCK_WINDOW; i++) {
            frame_lock_swap(&f, 2u);
            frame_lock_flip(&f, 1);
        }
        for (uint32_t i = 0; i < 1000u; i++) {
            frame_lock_swap(&f, 1u);
            CHECK(frame_lock_flip(&f, 1) == -1);
        }
    }
    /* The gate: no verdict, wait; locked to 60 against a 30 cap, hold; locked to
     * 30 (interval TWO) or to 25 (PAL, TWO), start; free, start; a 120 Hz display
     * (cap 60) with a title locked to 60, start. */
    CHECK(frame_lock_gate(0, 60, 30) == FRAMEGEN_GATE_WAIT);
    CHECK(frame_lock_gate(1, 60, 30) == FRAMEGEN_GATE_HOLD);
    CHECK(frame_lock_gate(1, 50, 30) == FRAMEGEN_GATE_HOLD);
    CHECK(frame_lock_gate(1, 30, 30) == FRAMEGEN_GATE_START);
    CHECK(frame_lock_gate(1, 25, 30) == FRAMEGEN_GATE_START);
    CHECK(frame_lock_gate(-1, 60, 30) == FRAMEGEN_GATE_START);
    CHECK(frame_lock_gate(1, 60, 60) == FRAMEGEN_GATE_START);
    if (failures)
        return 1;
    puts("frame_lock: ok");
    return 0;
}
