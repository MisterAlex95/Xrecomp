/* When the emulated pads come and go: see usb_hotplug.h. */
#include "usb_hotplug.h"

#include <string.h>

void usb_hotplug_init(UsbHotplug *h, unsigned allowed)
{
    memset(h, 0, sizeof *h);
    h->allowed = allowed & 0xFu;
    h->last_pad = -1;
}

unsigned usb_hotplug_auto_wanted(UsbHotplug *h, unsigned ports)
{
    if (ports)
        h->seen_any = 1;
    return ports ? ports : (h->seen_any ? 0u : 1u);
}

int usb_hotplug_step(UsbHotplug *h, unsigned wanted, unsigned configured, unsigned long now_ms, int *pad)
{
    unsigned want = wanted & h->allowed, add;
    int p;

    /* Removals first: one that is due goes out now. */
    for (p = 0; p < 4; p++) {
        unsigned bit = 1u << p;
        if (!(h->present & bit) || (want & bit)) {
            h->gone_since[p] = 0;
            continue;
        }
        if (!h->gone_since[p]) {
            h->gone_since[p] = now_ms;
        } else if (now_ms - h->gone_since[p] >= USB_HOTPLUG_REMOVE_MS) {
            h->present &= ~bit;
            h->gone_since[p] = 0;
            *pad = p;
            return -1;
        }
    }

    add = want & ~h->present;
    if (!add)
        return 0;
    /* The last pad in must have finished enumerating, then a short settle. */
    if (h->last_pad >= 0 && (h->present & (1u << h->last_pad)) && !(configured & (1u << h->last_pad))
        && now_ms - h->last_insert < USB_HOTPLUG_ENUM_MS)
        return 0;
    if (h->last_insert && now_ms - h->last_insert < USB_HOTPLUG_SETTLE_MS)
        return 0;
    for (p = 0; !(add & (1u << p)); p++)
        ;
    h->present |= 1u << p;
    h->last_insert = now_ms;
    h->last_pad = p;
    *pad = p;
    return 1;
}
