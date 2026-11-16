/* xbox_pad_hotkeys.h -- an Xbox pad state as the hotkeys' sources (XBA-240).
 *
 * The consoles read their hotkey chords (recomp_hotkeys.h: fast-forward held
 * or toggled, ...) from the physical pad's sources, console_pad.h's CPS_*
 * bits. An Xbox title's pad reaches it as an XBOX_GAMEPAD (xbox_InputGetState,
 * after the player's RECOMP_PAD_MAP), and usb_gamepad.c looks for the chords
 * there: the sources that state holds, and the same state with a held
 * chord's sources taken out so the title does not see them. With the default
 * mapping the two are the same buttons; with a remapping the chord follows
 * the Xbox button the physical one was mapped to.
 *
 * The Xbox pad's black and white buttons count as the shoulders (where the
 * default mapping takes them from), the triggers past half travel as pressed
 * (RECOMP_HK_TRIGGER), a stick direction past half its travel.
 *
 * Header only, pure: the unit test (tests/xbox_fast_forward) includes it. */
#ifndef XBOX_PAD_HOTKEYS_H
#define XBOX_PAD_HOTKEYS_H

#include <stdint.h>

#include "input/console_pad.h"   /* CPS_* */
#include "input/xinput_xbox.h"   /* XBOX_GAMEPAD */

#define XBOX_PAD_HK_ANALOG 128    /* an analog button or trigger, 0..255: pressed from here */
#define XBOX_PAD_HK_STICK 16384   /* a stick direction: past half its travel */

static const struct { uint16_t bit; uint8_t source; } xbox_pad_hk_digital[] = {
    {XBOX_GAMEPAD_DPAD_UP, CPS_DPAD_UP}, {XBOX_GAMEPAD_DPAD_DOWN, CPS_DPAD_DOWN},
    {XBOX_GAMEPAD_DPAD_LEFT, CPS_DPAD_LEFT}, {XBOX_GAMEPAD_DPAD_RIGHT, CPS_DPAD_RIGHT},
    {XBOX_GAMEPAD_START, CPS_START}, {XBOX_GAMEPAD_BACK, CPS_BACK},
    {XBOX_GAMEPAD_LEFT_THUMB, CPS_LEFT_STICK_CLICK}, {XBOX_GAMEPAD_RIGHT_THUMB, CPS_RIGHT_STICK_CLICK},
};

/* bAnalogButtons[i]'s source: A B X Y Black White LT RT. */
static const uint8_t xbox_pad_hk_analog[8] = {
    CPS_SOUTH, CPS_EAST, CPS_WEST, CPS_NORTH,
    CPS_LEFT_SHOULDER, CPS_RIGHT_SHOULDER, CPS_LEFT_TRIGGER, CPS_RIGHT_TRIGGER,
};

/* The CPS_BIT set the pad holds (sticks: +y up, the Xbox's way). */
static inline uint32_t xbox_pad_hotkey_sources(const XBOX_GAMEPAD *g)
{
    uint32_t s = 0;
    unsigned i;
    if (!g)
        return 0;
    for (i = 0; i < sizeof xbox_pad_hk_digital / sizeof xbox_pad_hk_digital[0]; i++)
        if (g->wButtons & xbox_pad_hk_digital[i].bit)
            s |= CPS_BIT(xbox_pad_hk_digital[i].source);
    for (i = 0; i < 8; i++)
        if (g->bAnalogButtons[i] >= XBOX_PAD_HK_ANALOG)
            s |= CPS_BIT(xbox_pad_hk_analog[i]);
    if (g->sThumbLY > XBOX_PAD_HK_STICK) s |= CPS_BIT(CPS_LSTICK_UP);
    if (g->sThumbLY < -XBOX_PAD_HK_STICK) s |= CPS_BIT(CPS_LSTICK_DOWN);
    if (g->sThumbLX < -XBOX_PAD_HK_STICK) s |= CPS_BIT(CPS_LSTICK_LEFT);
    if (g->sThumbLX > XBOX_PAD_HK_STICK) s |= CPS_BIT(CPS_LSTICK_RIGHT);
    if (g->sThumbRY > XBOX_PAD_HK_STICK) s |= CPS_BIT(CPS_RSTICK_UP);
    if (g->sThumbRY < -XBOX_PAD_HK_STICK) s |= CPS_BIT(CPS_RSTICK_DOWN);
    if (g->sThumbRX < -XBOX_PAD_HK_STICK) s |= CPS_BIT(CPS_RSTICK_LEFT);
    if (g->sThumbRX > XBOX_PAD_HK_STICK) s |= CPS_BIT(CPS_RSTICK_RIGHT);
    return s;
}

/* Takes the swallowed sources out of the pad: released buttons, centred
 * stick directions. swallow 0 leaves it as it is. */
static inline void xbox_pad_hotkey_swallow(XBOX_GAMEPAD *g, uint32_t swallow)
{
    unsigned i;
    if (!g || !swallow)
        return;
    for (i = 0; i < sizeof xbox_pad_hk_digital / sizeof xbox_pad_hk_digital[0]; i++)
        if (swallow & CPS_BIT(xbox_pad_hk_digital[i].source))
            g->wButtons = (uint16_t)(g->wButtons & ~xbox_pad_hk_digital[i].bit);
    for (i = 0; i < 8; i++)
        if (swallow & CPS_BIT(xbox_pad_hk_analog[i]))
            g->bAnalogButtons[i] = 0;
    if ((swallow & CPS_BIT(CPS_LSTICK_UP) && g->sThumbLY > 0) || (swallow & CPS_BIT(CPS_LSTICK_DOWN) && g->sThumbLY < 0))
        g->sThumbLY = 0;
    if ((swallow & CPS_BIT(CPS_LSTICK_RIGHT) && g->sThumbLX > 0) || (swallow & CPS_BIT(CPS_LSTICK_LEFT) && g->sThumbLX < 0))
        g->sThumbLX = 0;
    if ((swallow & CPS_BIT(CPS_RSTICK_UP) && g->sThumbRY > 0) || (swallow & CPS_BIT(CPS_RSTICK_DOWN) && g->sThumbRY < 0))
        g->sThumbRY = 0;
    if ((swallow & CPS_BIT(CPS_RSTICK_RIGHT) && g->sThumbRX > 0) || (swallow & CPS_BIT(CPS_RSTICK_LEFT) && g->sThumbRX < 0))
        g->sThumbRX = 0;
}

#endif /* XBOX_PAD_HOTKEYS_H */
