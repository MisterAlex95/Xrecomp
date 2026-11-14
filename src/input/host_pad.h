/* host_pad -- the host layer's pads: SDL game controllers on up to four
 * ports, in a console-agnostic state (XBA-145, src/platform/HOST_LAYER.md).
 *
 * Which controller is which port follows input_ports.h, the rules the Xbox
 * pads use (RECOMP_PAD_PORTS pins, RECOMP_PAD_BUILTIN, a port kept for its
 * controller across an unplug). A console runtime maps HostPadState onto its
 * own pad (a GBA's ten keys, a PS1 DualShock, an N64 controller); the Xbox
 * runtime keeps its own reader (xinput_device.c), which predates this.
 *
 * host_window_open calls host_pad_init and host_window_pump feeds the
 * events; without a window, call host_pad_init and host_pad_update (which
 * pumps SDL's events) yourself. Reads are thread-safe. POSIX hosts. */
#ifndef XBR_HOST_PAD_H
#define XBR_HOST_PAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

union SDL_Event;

#define HOST_PAD_PORTS 4

/* Buttons, in SDL_GameControllerButton order (bit n = button n). */
enum {
    HOST_PAD_A          = 1u << 0,
    HOST_PAD_B          = 1u << 1,
    HOST_PAD_X          = 1u << 2,
    HOST_PAD_Y          = 1u << 3,
    HOST_PAD_BACK       = 1u << 4,
    HOST_PAD_GUIDE      = 1u << 5,
    HOST_PAD_START      = 1u << 6,
    HOST_PAD_LSTICK     = 1u << 7,
    HOST_PAD_RSTICK     = 1u << 8,
    HOST_PAD_LSHOULDER  = 1u << 9,
    HOST_PAD_RSHOULDER  = 1u << 10,
    HOST_PAD_DPAD_UP    = 1u << 11,
    HOST_PAD_DPAD_DOWN  = 1u << 12,
    HOST_PAD_DPAD_LEFT  = 1u << 13,
    HOST_PAD_DPAD_RIGHT = 1u << 14
};

enum {
    HOST_PAD_AXIS_LX, HOST_PAD_AXIS_LY,      /* -32768..32767, +y down (SDL) */
    HOST_PAD_AXIS_RX, HOST_PAD_AXIS_RY,
    HOST_PAD_AXIS_LT, HOST_PAD_AXIS_RT,      /* 0..32767 */
    HOST_PAD_AXES
};

typedef struct HostPadState {
    int connected;
    uint32_t buttons;            /* HOST_PAD_* */
    int16_t axis[HOST_PAD_AXES];
    char name[64];
} HostPadState;

/* Initialises SDL's game controller subsystem and opens the controllers
 * present. Idempotent. Returns 1, or 0 when SDL cannot. */
int host_pad_init(void);

/* Controllers coming and going (SDL_CONTROLLERDEVICEADDED / REMOVED); other
 * events are ignored. */
void host_pad_handle_event(const union SDL_Event *ev);

/* Without a window: pumps SDL's events and handles the controller ones. */
void host_pad_update(void);

/* Connected ports as a bit mask (bit n = port n). */
unsigned host_pad_mask(void);

/* Fills *out for port (0..3). Returns 1 when a controller is there, 0 when
 * not (*out zeroed). */
int host_pad_read(int port, HostPadState *out);

/* Rumble, 0..65535 per motor, for ms. Returns 1 when the pad has it. */
int host_pad_rumble(int port, uint16_t low, uint16_t high, uint32_t ms);

void host_pad_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* XBR_HOST_PAD_H */
