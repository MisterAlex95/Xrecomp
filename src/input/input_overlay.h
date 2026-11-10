/* A touch overlay pad (POSIX hosts): buttons pressed on a screen of their
 * own, merged into the SDL backend's xbox_InputGetState.
 *
 * The Android app's companion panel on a second display (the AYN Thor's
 * bottom screen, docs/android/app-shell.md « Dual screen ») sets it from the
 * UI thread; the guest and USB threads read it through xbox_InputGetState.
 *
 * - Set with a pad: the overlay is active on that port. Its buttons are added
 *   to a real pad's (xbox_InputMap_PadMerge); with no pad attached it stands
 *   in for one, so a touch-only player still has a connected pad 0.
 * - Set with NULL: inactive, nothing held (the panel went away).
 *
 * Every Set bumps a sequence number that goes into dwPacketNumber, so the
 * game sees each change as a new packet. */
#ifndef XBOX_INPUT_OVERLAY_H
#define XBOX_INPUT_OVERLAY_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

void xbox_InputOverlaySet(int port, const XBOX_GAMEPAD *pad);

/* The overlay of a port into *out (zeroed when inactive); 1 when active.
 * seq (may be NULL) gets the number of Sets so far. */
int xbox_InputOverlayGet(int port, XBOX_GAMEPAD *out, DWORD *seq);

/* For xinput_device.c. Merge: adds an active overlay to a state already
 * read (a pad, or the keyboard stand-in). StandIn: no pad on this port;
 * fills *state from an active overlay and returns 1, else returns 0. */
void xbox_InputOverlayMerge(DWORD port, XBOX_INPUT_STATE *state);
int xbox_InputOverlayStandIn(DWORD port, XBOX_INPUT_STATE *state);

#ifdef __cplusplus
}
#endif

#endif
