/*
 * usb_hotplug.h -- when the emulated pads are plugged into and pulled out of
 * the root hub (XBA-123).
 *
 * Pure decisions, no registers and no clock of its own, so a test can run them
 * with made-up times; ohci.c turns each event into the port status change a
 * console's hub would show (connect status and its change bit, the root hub
 * status change interrupt), which is where XAPI's own device change tracking
 * -- XGetDevices / XGetDeviceChanges in a title -- learns about it.
 *
 * - Insertions go one at a time, lowest pad first: a new pad arrives only once
 *   the last one inserted is configured (or 5 s went by) and 200 ms after it,
 *   so there is never more than one device at the default address.
 * - A removal waits until the pad has gone unwanted for 1 s, so a Bluetooth
 *   pad that drops for a moment, or the touch overlay taking over from a pad
 *   that left, does not unplug the guest's pad.
 * - Auto mode (the default): the pads wanted are the ports that have a
 *   controller (xbox_InputPortMask); with none at all since the start, pad 0
 *   still, so a title never starts without a pad (as before). Fixed mode
 *   (RECOMP_USB_PADS=n): the first n pads, never removed (the old behaviour).
 */
#ifndef XBOX_USB_HOTPLUG_H
#define XBOX_USB_HOTPLUG_H

#define USB_HOTPLUG_REMOVE_MS   1000u
#define USB_HOTPLUG_SETTLE_MS   200u
#define USB_HOTPLUG_ENUM_MS     5000u

typedef struct UsbHotplug {
    unsigned      allowed;          /* pads that may exist: one per root port */
    unsigned      present;          /* pads plugged, as the guest sees them */
    unsigned long gone_since[4];    /* when a present pad stopped being wanted, 0 = wanted */
    unsigned long last_insert;      /* time of the last insertion, 0 = none yet */
    int           last_pad;         /* the pad inserted last, -1 = none */
    int           seen_any;         /* auto mode: a controller was there once */
} UsbHotplug;

void usb_hotplug_init(UsbHotplug *h, unsigned allowed);

/* Auto mode: the pads wanted from the ports that have a controller now. */
unsigned usb_hotplug_auto_wanted(UsbHotplug *h, unsigned ports);

/* One decision at time now_ms (never 0): +1 = plug *pad in, -1 = pull *pad out,
 * 0 = nothing. configured: bit n = pad n is configured (usb_gamepad.h). */
int usb_hotplug_step(UsbHotplug *h, unsigned wanted, unsigned configured, unsigned long now_ms, int *pad);

#endif
