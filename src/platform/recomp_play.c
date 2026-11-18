/* recomp_play.c -- the player's controls of a console runtime's frame loop, shared (XBA-274), see recomp_play.h. */
#include "recomp_play.h"
#include "recomp_states.h"

#include <stdio.h>
#include <string.h>

void recomp_play_init(recomp_play *p, const char *console, const char *unit, recomp_rewinder *rw)
{
    memset(p, 0, sizeof *p);
    snprintf(p->console, sizeof p->console, "%s", console ? console : "");
    p->unit = unit ? unit : "frame";
    p->rw = rw;
}

void recomp_play_begin(recomp_play *p, uint32_t pad_held, uint32_t pad_pressed, const uint8_t *keys, int nkeys,
                       uint64_t frame, recomp_play_frame *out)
{
    uint32_t held = pad_held, pressed = pad_pressed;
    if (keys && nkeys > 0) { /* the desktop keys (RECOMP_HOTKEYS_KEYS: Tab, F1-F4, Shift+F1-F4 by default) */
        uint32_t key_pressed = 0;
        held |= recomp_hotkeys_keys_update(recomp_hotkeys_get(), &p->keys, keys, nkeys, &key_pressed);
        pressed |= key_pressed;
    }
    recomp_hotkeys_run(pressed); /* quick save / load, slot steps, save / load slot N, the fast-forward toggle */
    recomp_ff_hold(!!(held & RECOMP_HK_BIT(RECOMP_HK_FF_HOLD)));
    int rewinding = p->rw ? recomp_rewinder_begin(p->rw, !!(held & RECOMP_HK_BIT(RECOMP_HK_REWIND))) : 0;
    int ff = recomp_ff_active(), speed = recomp_ff_speed();
    int changed = ff != p->was_ff;
    if (changed) {
        printf("%s: fast-forward %s at %s %llu\n", p->console, ff ? (speed ? "on" : "on (max)") : "off", p->unit,
               (unsigned long long)frame);
        fflush(stdout);
        p->was_ff = ff;
    }
    out->held = held;
    out->pressed = pressed;
    out->rewinding = rewinding;
    out->ff = ff;
    out->speed = speed;
    out->ff_changed = changed;
    out->steps = rewinding ? 0 : ff ? (speed ? speed : RECOMP_PLAY_MAX_STEPS) : 1;
}
