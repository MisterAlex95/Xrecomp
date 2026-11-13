/* Four pads from the connected controllers (XBA-123):
 *  1. input_ports.c's rules alone (start packing, pins, the built-in pad,
 *     hotplug keeping a port for its owner, a fifth controller);
 *  2. usb_hotplug.c's decisions (one insertion at a time, removal after 1 s,
 *     the auto and fixed wanted sets);
 *  3. the SDL backend (xinput_device.c) over a fake SDL with several
 *     controllers: which one each port reads, rumble on the right one, a
 *     disconnect keeping the port, xbox_InputPortMask with the overlay.
 * The same start cases are in the app's PadPortsTest (PadPorts.java). */
#include "xinput_xbox.h"
#include "input_overlay.h"
#include "input_ports.h"
#include "usb_hotplug.h"
#include <SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        s_fail = 1; \
    } \
} while (0)

/* ---- 1. input_ports ------------------------------------------------------ */

static void start(InputPorts *p, int n, const char *const keys[], int out[])
{
    int ids[8], i;
    for (i = 0; i < n; i++)
        ids[i] = 100 + i;
    xbox_InputPortsStart(p, n, keys, ids, out);
}

static void test_ports(void)
{
    InputPorts p;
    int out[8];
    char key[INPUT_PORT_KEY];
    static const char *const two[] = { "045e:0b13", "2dc8:3106" };
    static const char *const five[] = { "0001:0001", "0002:0002", "0003:0003", "0004:0004", "0005:0005" };

    xbox_InputPortsKey(key, 0x45E, 0xB13);
    CHECK(strcmp(key, "045e:0b13") == 0);

    /* No pins: host order, packed. */
    CHECK(xbox_InputPortsInit(&p, NULL, NULL) == 0);
    start(&p, 2, two, out);
    CHECK(out[0] == 0 && out[1] == 1);

    /* The built-in pad (second in host order) goes first. */
    xbox_InputPortsInit(&p, "", "2DC8:3106");
    start(&p, 2, two, out);
    CHECK(out[1] == 0 && out[0] == 1);

    /* A pin wins over the built-in pad; the pin's name part is ignored. */
    CHECK(xbox_InputPortsInit(&p, "1=045e:0b13:Xbox Wireless Controller", "2dc8:3106") == 0);
    start(&p, 2, two, out);
    CHECK(out[0] == 0 && out[1] == 1);

    /* A pin to port 2 whose controller is absent leaves no hole. */
    xbox_InputPortsInit(&p, "1=9999:9999,2=2dc8:3106", NULL);
    start(&p, 2, two, out);
    CHECK(out[1] == 0 && out[0] == 1);

    /* Bad entries are counted and ignored. */
    CHECK(xbox_InputPortsInit(&p, "5=045e:0b13, 1=zz, 2=045e:0b13 ,x", NULL) == 3);
    CHECK(strcmp(p.pin[1], "045e:0b13") == 0);

    /* Five controllers: the fifth has no port. */
    xbox_InputPortsInit(&p, NULL, NULL);
    start(&p, 5, five, out);
    CHECK(out[0] == 0 && out[3] == 3 && out[4] == -1);

    /* Hotplug: a port waits for its owner; a newcomer takes a fresh port, and
     * only an absent owner's port when nothing else is free. */
    xbox_InputPortsInit(&p, NULL, NULL);
    start(&p, 2, two, out);                                 /* ids 100 (port 1), 101 (port 2) */
    CHECK(xbox_InputPortsDetach(&p, 100) == 0);
    CHECK(xbox_InputPortsAttach(&p, "0003:0003", 200) == 2);   /* not port 1: it waits */
    CHECK(xbox_InputPortsAttach(&p, "045e:0b13", 201) == 0);   /* the owner is back */
    CHECK(xbox_InputPortsPortOf(&p, 201) == 0);
    CHECK(xbox_InputPortsDetach(&p, 101) == 1);
    CHECK(xbox_InputPortsAttach(&p, "0004:0004", 202) == 3);
    CHECK(xbox_InputPortsAttach(&p, "0005:0005", 203) == 1);   /* last free: the absent owner's */
    CHECK(xbox_InputPortsAttach(&p, "0006:0006", 204) == -1);
    CHECK(xbox_InputPortsDetach(&p, 999) == -1);

    /* A pin is honoured for a late arrival when its port is still unused. */
    xbox_InputPortsInit(&p, "3=045e:0b13", NULL);
    start(&p, 1, &two[1], out);
    CHECK(out[0] == 0);
    CHECK(xbox_InputPortsAttach(&p, "0003:0003", 300) == 1);
    CHECK(xbox_InputPortsAttach(&p, "045e:0b13", 301) == 2);

    /* Two pads of one model: interchangeable, each keeps a port. */
    xbox_InputPortsInit(&p, NULL, NULL);
    {
        static const char *const twins[] = { "045e:0b13", "045e:0b13" };
        start(&p, 2, twins, out);
        CHECK(out[0] == 0 && out[1] == 1);
        xbox_InputPortsDetach(&p, 101);
        CHECK(xbox_InputPortsAttach(&p, "045e:0b13", 400) == 1);
    }
}

/* ---- 2. usb_hotplug ------------------------------------------------------- */

static void test_hotplug(void)
{
    UsbHotplug h;
    int pad = -1;

    usb_hotplug_init(&h, 0xF);
    /* Auto: nothing ever connected still wants pad 1; once something was, no. */
    CHECK(usb_hotplug_auto_wanted(&h, 0) == 1);
    CHECK(usb_hotplug_auto_wanted(&h, 4) == 4);
    CHECK(usb_hotplug_auto_wanted(&h, 0) == 0);

    usb_hotplug_init(&h, 0xF);
    CHECK(usb_hotplug_step(&h, 0x5, 0, 1000, &pad) == 1 && pad == 0);
    CHECK(usb_hotplug_step(&h, 0x5, 0, 1500, &pad) == 0);          /* pad 1 not configured yet */
    CHECK(usb_hotplug_step(&h, 0x5, 0x1, 1100, &pad) == 0);        /* configured, but settling */
    CHECK(usb_hotplug_step(&h, 0x5, 0x1, 1300, &pad) == 1 && pad == 2);
    CHECK(usb_hotplug_step(&h, 0x5, 0x5, 9000, &pad) == 0);        /* all there */
    CHECK(h.present == 0x5);
    /* A drop shorter than a second unplugs nothing. */
    CHECK(usb_hotplug_step(&h, 0x1, 0x5, 10000, &pad) == 0);
    CHECK(usb_hotplug_step(&h, 0x5, 0x5, 10900, &pad) == 0);
    CHECK(usb_hotplug_step(&h, 0x1, 0x5, 11000, &pad) == 0);
    CHECK(usb_hotplug_step(&h, 0x1, 0x5, 11999, &pad) == 0);
    CHECK(usb_hotplug_step(&h, 0x1, 0x5, 12000, &pad) == -1 && pad == 2);
    CHECK(h.present == 0x1);
    /* A pad that never configures holds the next one back 5 s at most. */
    CHECK(usb_hotplug_step(&h, 0x3, 0x1, 20000, &pad) == 1 && pad == 1);
    CHECK(usb_hotplug_step(&h, 0x7, 0x1, 24000, &pad) == 0);
    CHECK(usb_hotplug_step(&h, 0x7, 0x1, 25000, &pad) == 1 && pad == 2);
    /* Only the allowed pads (one per root port). */
    usb_hotplug_init(&h, 0x3);
    CHECK(usb_hotplug_step(&h, 0x4, 0, 1000, &pad) == 0);
}

/* ---- 3. the SDL backend with several controllers ------------------------- */

struct SDL_Joystick { SDL_JoystickID instance_id; };
struct SDL_GameController {
    struct SDL_Joystick joystick;
    int open;
    int device;          /* index into s_dev */
    uint16_t rumble_lo, rumble_hi;
};

typedef struct {
    int present;
    SDL_JoystickID id;
    uint16_t vendor, product;
    const char *name;
    int button_a;
} FakeDevice;

#define MAXDEV 6
static FakeDevice s_dev[MAXDEV];
static struct SDL_GameController s_ctl[MAXDEV];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* SDL lists present devices by index, in s_dev order. */
static int dev_of_index(int index)
{
    int i, n = 0;
    for (i = 0; i < MAXDEV; i++)
        if (s_dev[i].present && n++ == index)
            return i;
    return -1;
}

int xbox_InputKeyboardPoll(XBOX_INPUT_STATE *state) { (void)state; return 0; }
int SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return 1; }
Uint32 SDL_WasInit(Uint32 flags) { (void)flags; return SDL_INIT_GAMECONTROLLER; }
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }

int SDL_NumJoysticks(void)
{
    int i, n = 0;
    for (i = 0; i < MAXDEV; i++)
        n += s_dev[i].present;
    return n;
}

int SDL_IsGameController(int index) { return dev_of_index(index) >= 0; }

SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int index)
{
    int d = dev_of_index(index);
    return d < 0 ? -1 : s_dev[d].id;
}

Uint16 SDL_JoystickGetDeviceVendor(int index) { int d = dev_of_index(index); return d < 0 ? 0 : s_dev[d].vendor; }
Uint16 SDL_JoystickGetDeviceProduct(int index) { int d = dev_of_index(index); return d < 0 ? 0 : s_dev[d].product; }
const char *SDL_JoystickNameForIndex(int index) { int d = dev_of_index(index); return d < 0 ? NULL : s_dev[d].name; }

SDL_GameController *SDL_GameControllerOpen(int index)
{
    int d = dev_of_index(index);
    if (d < 0)
        return NULL;
    memset(&s_ctl[d], 0, sizeof s_ctl[d]);
    s_ctl[d].joystick.instance_id = s_dev[d].id;
    s_ctl[d].open = 1;
    s_ctl[d].device = d;
    return &s_ctl[d];
}

void SDL_GameControllerClose(SDL_GameController *c) { if (c) c->open = 0; }

int SDL_GameControllerGetAttached(SDL_GameController *c)
{
    return c && c->open && s_dev[c->device].present && s_dev[c->device].id == c->joystick.instance_id;
}

SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c) { return c ? &c->joystick : NULL; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) { return j ? j->instance_id : -1; }
void SDL_GameControllerUpdate(void) {}

Uint8 SDL_GameControllerGetButton(SDL_GameController *c, int button)
{
    return button == SDL_CONTROLLER_BUTTON_A && s_dev[c->device].button_a;
}

Sint16 SDL_GameControllerGetAxis(SDL_GameController *c, int axis) { (void)c; (void)axis; return 0; }

int SDL_GameControllerRumble(SDL_GameController *c, uint16_t lo, uint16_t hi, Uint32 ms)
{
    (void)ms;
    c->rumble_lo = lo;
    c->rumble_hi = hi;
    return 0;
}

void SDL_LockJoysticks(void) { pthread_mutex_lock(&s_lock); }
void SDL_UnlockJoysticks(void) { pthread_mutex_unlock(&s_lock); }
int SDL_AtomicGet(SDL_atomic_t *v) { return atomic_load(&v->value); }
int SDL_AtomicSet(SDL_atomic_t *v, int x) { return atomic_exchange(&v->value, x); }

static void plug(int d, SDL_JoystickID id, uint16_t vendor, uint16_t product, const char *name)
{
    s_dev[d].present = 1;
    s_dev[d].id = id;
    s_dev[d].vendor = vendor;
    s_dev[d].product = product;
    s_dev[d].name = name;
    s_dev[d].button_a = 0;
}

/* Which device port `port` reads: press A on each in turn. */
static int device_on_port(DWORD port)
{
    XBOX_INPUT_STATE st;
    int d, found = -1;
    for (d = 0; d < MAXDEV; d++) {
        if (!s_dev[d].present)
            continue;
        s_dev[d].button_a = 1;
        if (xbox_InputGetState(port, &st) == ERROR_SUCCESS && st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255)
            found = d;
        s_dev[d].button_a = 0;
    }
    return found;
}

static void test_backend(void)
{
    XBOX_VIBRATION vib = { 1234, 5678 };
    XBOX_GAMEPAD pad;
    char name[64];

    /* A handheld: an external pad listed first, the built-in pad second. */
    setenv("RECOMP_PAD_BUILTIN", "2dc8:3106", 1);
    unsetenv("RECOMP_PAD_PORTS");
    plug(0, 10, 0x045e, 0x0b13, "Xbox Wireless Controller");
    plug(1, 11, 0x2dc8, 0x3106, "Thor Controller");
    xbox_InputInit();
    CHECK(device_on_port(0) == 1);       /* the built-in pad is player 1 */
    CHECK(device_on_port(1) == 0);
    CHECK(xbox_InputPortMask() == 0x3);
    CHECK(xbox_InputPortName(0, name, (int)sizeof name) && strcmp(name, "Thor Controller") == 0);

    /* Rumble goes to the controller on that port. */
    CHECK(xbox_InputSetState(1, &vib) == ERROR_SUCCESS);
    CHECK(s_ctl[0].rumble_lo == 1234 && s_ctl[0].rumble_hi == 5678 && s_ctl[1].rumble_lo == 0);

    /* The external pad drops: port 2 empties and waits for it; a third pad
     * takes port 3; the first one comes back to port 2 with a new instance. */
    s_dev[0].present = 0;
    CHECK(xbox_InputPortMask() == 0x1);
    CHECK(xbox_InputGetState(1, &(XBOX_INPUT_STATE){0}) == ERROR_DEVICE_NOT_CONNECTED);
    plug(2, 12, 0x054c, 0x09cc, "PS4 Controller");
    CHECK(xbox_InputPortMask() == 0x5);
    CHECK(device_on_port(2) == 2);
    plug(0, 13, 0x045e, 0x0b13, "Xbox Wireless Controller");
    CHECK(xbox_InputPortMask() == 0x7);
    CHECK(device_on_port(1) == 0);
    CHECK(xbox_InputSetState(2, &vib) == ERROR_SUCCESS && s_ctl[2].rumble_lo == 1234);

    /* Pinned in the Controls screen: the Xbox pad on port 1, the Thor on 2. */
    setenv("RECOMP_PAD_PORTS", "1=045e:0b13:Xbox Wireless Controller,2=2dc8:3106", 1);
    s_dev[2].present = 0;
    xbox_InputInit();
    CHECK(device_on_port(0) == 0 && device_on_port(1) == 1);
    CHECK(xbox_InputPortMask() == 0x3);

    /* No controller: nothing, unless the touch overlay stands in on port 1. */
    s_dev[0].present = 0;
    s_dev[1].present = 0;
    CHECK(xbox_InputPortMask() == 0);
    memset(&pad, 0, sizeof pad);
    xbox_InputOverlaySet(0, &pad);
    CHECK(xbox_InputPortMask() == 0x1);
    xbox_InputOverlaySet(0, NULL);
    CHECK(xbox_InputPortMask() == 0);
    unsetenv("RECOMP_PAD_PORTS");
    unsetenv("RECOMP_PAD_BUILTIN");
}

int main(void)
{
    test_ports();
    test_hotplug();
    test_backend();
    if (!s_fail)
        puts("pad ports: ok");
    return s_fail;
}
