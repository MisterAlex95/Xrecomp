/* recomp_play.h -- the player's controls of a console runtime's frame loop, shared (XBA-274).
 *
 * Every console runtime (GBA, PS1, N64, GameCube, and the next ones) runs the same controls once per shown frame:
 * the desktop keys' hotkeys (RECOMP_HOTKEYS_KEYS) next to the pads' chords, the actions they press (quick save /
 * load, slots, the fast-forward toggle: recomp_hotkeys_run), the fast-forward hold, the rewind hold
 * (recomp_rewinder_begin), and how many console frames the shown frame runs (one; `speed` under fast-forward, or as
 * many as fit when the speed is "max"; none while rewinding), with one log line when fast-forward goes on or off.
 * recomp_play_begin does all of it, so a runtime only reads its pads and steps its machine:
 *
 *     recomp_play play;
 *     recomp_play_init(&play, "gba", "frame", &rw);
 *     for (;;) {
 *         uint32_t held = 0, pressed = 0;            // the pads' chords (recomp_hotkeys_pad_update)
 *         ...
 *         recomp_play_frame f;
 *         recomp_play_begin(&play, held, pressed, window ? SDL_GetKeyboardState(&n) : NULL, n, frames, &f);
 *         if (f.ff_changed) { reset the pacing and the audio carry }
 *         if (f.rewinding) { lock; recomp_rewinder_step(...); unlock }
 *         for (int i = 0; i < f.steps; i++) { run a frame; recomp_rewinder_frame(...) }
 *     }
 *
 * libc only (xbr_host_core): the keyboard's state comes in as SDL_GetKeyboardState's array, NULL without a window. */
#ifndef XBR_RECOMP_PLAY_H
#define XBR_RECOMP_PLAY_H

#include "recomp_hotkeys.h"
#include "recomp_rewind.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Console frames a shown frame runs under fast-forward at "max" speed (RECOMP_FF_SPEED=max): the runtime stops the
 * burst earlier when its display frame is over. */
#define RECOMP_PLAY_MAX_STEPS 64

typedef struct recomp_play {
    char console[8];              /* the log lines' prefix: "gba: fast-forward on at frame N" */
    const char *unit;             /* "frame", "field" */
    recomp_rewinder *rw;          /* NULL: no rewind */
    recomp_hotkeys_state keys;    /* the keyboard's chords between frames */
    int was_ff;
} recomp_play;

typedef struct recomp_play_frame {
    uint32_t held;      /* the actions held now (pads and keys), RECOMP_HK_BIT */
    uint32_t pressed;   /* the actions pressed this frame (already run) */
    int rewinding;      /* 1: step back (recomp_rewinder_step), run no console frame, play no sound */
    int ff;             /* fast-forward on */
    int speed;          /* its speed (recomp_ff_speed: 0 = max) */
    int ff_changed;     /* fast-forward went on or off at this frame (logged): restart the pacing, the audio carry */
    int steps;          /* console frames to run: 0 rewinding, 1, speed, or RECOMP_PLAY_MAX_STEPS at max */
} recomp_play_frame;

/* unit: the log's word for a console frame (NULL: "frame"); rw may be NULL. */
void recomp_play_init(recomp_play *p, const char *console, const char *unit, recomp_rewinder *rw);

/* Once per shown frame, before running any console frame. pad_held / pad_pressed: the pads' chords (their
 * recomp_hotkeys_pad_update results, all ports or-ed); keys, nkeys: the keyboard's state (NULL: none, no window);
 * frame: the console frames run so far (the log line). Runs the pressed actions and the holds, fills *out. */
void recomp_play_begin(recomp_play *p, uint32_t pad_held, uint32_t pad_pressed, const uint8_t *keys, int nkeys,
                       uint64_t frame, recomp_play_frame *out);

#ifdef __cplusplus
}
#endif

#endif /* XBR_RECOMP_PLAY_H */
