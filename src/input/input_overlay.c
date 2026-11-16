/* The touch overlay pad: see input_overlay.h. */
#include "input_overlay.h"
#include "input_map.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static XBOX_GAMEPAD s_pad[XBOX_MAX_CONTROLLERS];
static int s_active[XBOX_MAX_CONTROLLERS];
static DWORD s_seq[XBOX_MAX_CONTROLLERS];

void xbox_InputOverlaySet(int port, const XBOX_GAMEPAD *pad)
{
    if (port < 0 || port >= XBOX_MAX_CONTROLLERS)
        return;
    pthread_mutex_lock(&s_lock);
    if (pad)
        s_pad[port] = *pad;
    else
        memset(&s_pad[port], 0, sizeof s_pad[port]);
    s_active[port] = pad != NULL;
    s_seq[port]++;
    pthread_mutex_unlock(&s_lock);
    /* Once per run: the first press that reached the runtime (with the USB
     * report's own first-held line, usb_gamepad.c, it says where a press that
     * does nothing in the title stops). */
    if (pad && (pad->wButtons || pad->bAnalogButtons[0] || pad->bAnalogButtons[1] || pad->bAnalogButtons[2]
                || pad->bAnalogButtons[3] || pad->bAnalogButtons[6] || pad->bAnalogButtons[7])) {
        static int s_logged;
        if (!__atomic_exchange_n(&s_logged, 1, __ATOMIC_RELAXED)) {
            fprintf(stderr, "  [INPUT] touch overlay: first press on port %d (buttons %04X, A %u B %u X %u Y %u)\n",
                    port + 1, pad->wButtons, pad->bAnalogButtons[0], pad->bAnalogButtons[1],
                    pad->bAnalogButtons[2], pad->bAnalogButtons[3]);
            fflush(stderr);
        }
    }
}

int xbox_InputOverlayGet(int port, XBOX_GAMEPAD *out, DWORD *seq)
{
    int active;

    if (port < 0 || port >= XBOX_MAX_CONTROLLERS) {
        if (out)
            memset(out, 0, sizeof *out);
        if (seq)
            *seq = 0;
        return 0;
    }
    pthread_mutex_lock(&s_lock);
    active = s_active[port];
    if (out)
        *out = s_pad[port];
    if (seq)
        *seq = s_seq[port];
    pthread_mutex_unlock(&s_lock);
    return active;
}

void xbox_InputOverlayMerge(DWORD port, XBOX_INPUT_STATE *state)
{
    XBOX_GAMEPAD pad;
    DWORD seq;

    if (!xbox_InputOverlayGet((int)port, &pad, &seq))
        return;
    xbox_InputMap_PadMerge(&state->Gamepad, &pad);
    /* Both counts only grow, so the sum changes whenever either source does. */
    state->dwPacketNumber += seq;
}

int xbox_InputOverlayStandIn(DWORD port, XBOX_INPUT_STATE *state)
{
    XBOX_GAMEPAD pad;
    DWORD seq;

    if (!xbox_InputOverlayGet((int)port, &pad, &seq))
        return 0;
    memset(state, 0, sizeof *state);
    state->Gamepad = pad;
    state->dwPacketNumber = seq;
    return 1;
}
