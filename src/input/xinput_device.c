/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one.
 *
 * The first version put the keyboard behind XInput's failure, on the
 * assumption that with nothing plugged in the call would fail. It does
 * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
 * with every button at rest, so the fallback was unreachable and
 * pressing a key did nothing at all.
 *
 * Merging is also the better rule. A real pad keeps working -- its
 * buttons are already in pState and the keyboard only adds to them --
 * and there is no special case left to get wrong. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!xbox_InputKeyboardPoll(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* The keyboard on port 0 (RECOMP_KEYBOARD=1) comes from keyboard.c, shared
 * with the SDL backend; the GDI window in fb_present.c records the keys. */

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = xbox_InputKeyboardPoll(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            if (kb || mkb_enabled())
                return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    if (dwPort == 0)
        keyboard_merge(pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0)
        mkb_merge(dwPort, pState);

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

/* XInput's slots are the ports already; the keyboard and mouse stand in on 1. */
unsigned xbox_InputPortMask(void)
{
    unsigned mask = 0;
    const char *kb = getenv("RECOMP_KEYBOARD");

    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        if (XInputGetState(i, &state) == ERROR_SUCCESS)
            mask |= 1u << i;
    }
    if ((kb && *kb && *kb != '0') || mkb_enabled())
        mask |= 1u;
    return mask;
}

int xbox_InputPortName(DWORD dwPort, char *out, int size)
{
    XINPUT_STATE state;

    if (!out || size <= 0)
        return 0;
    out[0] = 0;
    if (dwPort >= XBOX_MAX_CONTROLLERS || XInputGetState(dwPort, &state) != ERROR_SUCCESS)
        return 0;
    snprintf(out, (size_t)size, "XInput %lu", (unsigned long)dwPort + 1);
    return 1;
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include "input_overlay.h"   /* touch buttons on a second screen (Android) */
#include "input_map.h"       /* RECOMP_PAD_MAP: the player's remapping */
#include "input_ports.h"     /* which controller is which port (XBA-123) */

/* RECOMP_PAD_MAP (XBA-131, the Android launcher's Controls screen writes it
 * into recomp.cfg): which physical control of a pad each Xbox control takes,
 * for every pad. Unset or empty: the fixed mapping below, as before. Set: the
 * pad is read into an InputRaw and mapped by input_map.c, with no deadzone and
 * no trigger threshold so that an identity map reads exactly as the fixed one;
 * the sticks are copied raw (PadMap would round a diagonal onto the circle). */
static InputPadMap g_padmap;
static int g_padmap_on;

static void padmap_init(void)
{
    const char *v = getenv("RECOMP_PAD_MAP");
    char text[512];
    int bad;

    g_padmap_on = 0;
    if (!v || !*v)
        return;
    xbox_InputMap_PadMapDefaults(&g_padmap);
    g_padmap.deadzone_left = g_padmap.deadzone_right = 0;
    g_padmap.trigger_threshold = 0;
    bad = xbox_InputMap_PadMapParse(&g_padmap, v);
    xbox_InputMap_PadMapFormat(&g_padmap, text, (int)sizeof text);
    fprintf(stderr, "[INPUT] RECOMP_PAD_MAP: %s", text[0] ? text : "(the default mapping)");
    if (bad > 0)
        fprintf(stderr, " (%d entr%s not understood, ignored)", bad, bad == 1 ? "y" : "ies");
    fputc('\n', stderr);
    g_padmap_on = 1;
}

static int16_t half_axis(int v)   /* the positive half of a signed axis, 0..32767 */
{
    return (int16_t)(v > 0 ? (v > 32767 ? 32767 : v) : 0);
}

static void stick_of(int which, const InputRaw *r, SHORT *x, SHORT *y)
{
    *x = which == INPUT_STICK_LEFT ? r->lx : which == INPUT_STICK_RIGHT ? r->rx : 0;
    *y = which == INPUT_STICK_LEFT ? r->ly : which == INPUT_STICK_RIGHT ? r->ry : 0;
}

/* The controller through g_padmap. Called with the joysticks locked. */
static void padmap_read(SDL_GameController *c, XBOX_GAMEPAD *out)
{
    static const struct { int src, button; } k_buttons[] = {
        { INPUT_SRC_SOUTH, SDL_CONTROLLER_BUTTON_A }, { INPUT_SRC_EAST, SDL_CONTROLLER_BUTTON_B },
        { INPUT_SRC_WEST, SDL_CONTROLLER_BUTTON_X }, { INPUT_SRC_NORTH, SDL_CONTROLLER_BUTTON_Y },
        { INPUT_SRC_BACK, SDL_CONTROLLER_BUTTON_BACK }, { INPUT_SRC_START, SDL_CONTROLLER_BUTTON_START },
        { INPUT_SRC_LEFT_STICK_CLICK, SDL_CONTROLLER_BUTTON_LEFTSTICK },
        { INPUT_SRC_RIGHT_STICK_CLICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK },
        { INPUT_SRC_LEFT_SHOULDER, SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
        { INPUT_SRC_RIGHT_SHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
        { INPUT_SRC_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP }, { INPUT_SRC_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
        { INPUT_SRC_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT },
        { INPUT_SRC_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
    };
    InputRaw r;
    size_t i;

    memset(&r, 0, sizeof r);
    for (i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; i++)
        r.src[k_buttons[i].src] = SDL_GameControllerGetButton(c, k_buttons[i].button) ? 32767 : 0;
    r.src[INPUT_SRC_LEFT_TRIGGER] = half_axis(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    r.src[INPUT_SRC_RIGHT_TRIGGER] = half_axis(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    r.lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    r.ly = (int16_t)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    r.rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    r.ry = (int16_t)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
    r.src[INPUT_SRC_LSTICK_UP] = half_axis(r.ly);
    r.src[INPUT_SRC_LSTICK_DOWN] = half_axis(-(int)r.ly);
    r.src[INPUT_SRC_LSTICK_LEFT] = half_axis(-(int)r.lx);
    r.src[INPUT_SRC_LSTICK_RIGHT] = half_axis(r.lx);
    r.src[INPUT_SRC_RSTICK_UP] = half_axis(r.ry);
    r.src[INPUT_SRC_RSTICK_DOWN] = half_axis(-(int)r.ry);
    r.src[INPUT_SRC_RSTICK_LEFT] = half_axis(-(int)r.rx);
    r.src[INPUT_SRC_RSTICK_RIGHT] = half_axis(r.rx);
    xbox_InputMap_PadMap(&g_padmap, &r, out);
    stick_of(g_padmap.left_stick, &r, &out->sThumbLX, &out->sThumbLY);
    stick_of(g_padmap.right_stick, &r, &out->sThumbRX, &out->sThumbRY);
}

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Set while the framebuffer window's main thread pumps SDL events
 * (src/video/fb_present_sdl.c). The pump already updates the controllers,
 * and SDL_GameControllerUpdate pumps events itself, which on macOS must only
 * happen on the main thread; so then this side only reads them. */
static SDL_atomic_t g_main_pump;

void xbox_InputSetMainPump(int on)
{
    SDL_AtomicSet(&g_main_pump, on);
}

static void update_controllers(void)
{
    if (!SDL_AtomicGet(&g_main_pump))
        SDL_GameControllerUpdate();
}

/* Which controller is which port (XBA-123, input_ports.h): first come, kept
 * across a disconnect, RECOMP_PAD_PORTS pins (the app's Controls screen) and
 * RECOMP_PAD_BUILTIN (the handheld's own pad: port 1 unless pinned otherwise).
 * Guarded by the joystick lock, like g_pads. */
static InputPorts g_ports;
static char g_names[XBOX_MAX_CONTROLLERS][64];

static void ports_init(void)
{
    const char *pins = getenv("RECOMP_PAD_PORTS");
    const char *builtin = getenv("RECOMP_PAD_BUILTIN");
    int bad = xbox_InputPortsInit(&g_ports, pins, builtin);

    if ((pins && *pins) || (builtin && *builtin))
        fprintf(stderr, "[INPUT] ports: RECOMP_PAD_PORTS=%s RECOMP_PAD_BUILTIN=%s%s\n", pins ? pins : "",
                builtin ? builtin : "", bad > 0 ? " (entries not understood, ignored)" : "");
}

/* Set up once, by whoever comes first: xbox_InputInit (the tests) or the
 * first read. Nothing in the runtime calls xbox_InputInit, so RECOMP_PAD_MAP
 * (padmap_init) never applied in a real run before this (XBA-123). */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void init_once(void)
{
    padmap_init();
    ports_init();
}

static void ensure_init(void)
{
    pthread_once(&g_once, init_once);
}

static void port_opened_locked(int port, int index, SDL_GameController *controller, const char *key)
{
    const char *name = SDL_JoystickNameForIndex(index);

    g_pads[port] = controller;
    g_controller_connected[port] = TRUE;
    snprintf(g_names[port], sizeof g_names[port], "%s", name ? name : "Controller");
    fprintf(stderr, "[INPUT] port %d: %s (%s) connected\n", port + 1, g_names[port], key);
}

/* A controller that went away: the port stays its own (input_ports.h).
 * Called with the joysticks locked. */
static void port_closed_locked(int port)
{
    xbox_InputPortsDetach(&g_ports, g_ports.id[port]);
    SDL_GameControllerClose(g_pads[port]);
    g_pads[port] = NULL;
    g_controller_connected[port] = FALSE;
    fprintf(stderr, "[INPUT] port %d: %s disconnected (the port waits for it)\n", port + 1, g_names[port]);
}

static int controller_is_open_locked(SDL_JoystickID instance_id)
{
    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot]) {
            SDL_Joystick *joystick = SDL_GameControllerGetJoystick(g_pads[slot]);
            if (joystick && SDL_JoystickInstanceID(joystick) == instance_id)
                return 1;
        }
    }
    return 0;
}

#define MAX_SEEN 16

static void refresh_controllers_locked(void)
{
    int count, n = 0;
    int index[MAX_SEEN], ids[MAX_SEEN], ports[MAX_SEEN];
    char keys[MAX_SEEN][INPUT_PORT_KEY];
    const char *keyp[MAX_SEEN];

    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
        if (g_pads[slot] && !SDL_GameControllerGetAttached(g_pads[slot]))
            port_closed_locked(slot);

    count = SDL_NumJoysticks();
    for (int i = 0; i < count && n < MAX_SEEN; i++) {
        SDL_JoystickID instance_id;

        if (!SDL_IsGameController(i))
            continue;
        instance_id = SDL_JoystickGetDeviceInstanceID(i);
        if (instance_id < 0 || controller_is_open_locked(instance_id))
            continue;
        index[n] = i;
        ids[n] = (int)instance_id;
        xbox_InputPortsKey(keys[n], SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i));
        keyp[n] = keys[n];
        n++;
    }
    /* The first look packs whoever is there from port 1 (pins, the built-in
     * pad, the rest); later arrivals take a port by the hotplug rules. */
    if (!g_ports.started) {
        xbox_InputPortsStart(&g_ports, n, keyp, ids, ports);
    } else {
        for (int i = 0; i < n; i++)
            ports[i] = xbox_InputPortsAttach(&g_ports, keys[i], ids[i]);
    }
    for (int i = 0; i < n; i++) {
        SDL_GameController *controller;

        if (ports[i] < 0)
            continue;   /* a fifth controller: no port */
        controller = SDL_GameControllerOpen(index[i]);
        if (!controller) {
            xbox_InputPortsDetach(&g_ports, ids[i]);
            continue;
        }
        port_opened_locked(ports[i], index[i], controller, keys[i]);
    }
}

static SDL_GameController *controller_for_port_locked(DWORD port)
{
    SDL_GameController *controller = g_pads[port];

    if (!controller || !SDL_GameControllerGetAttached(controller)) {
        refresh_controllers_locked();
        controller = g_pads[port];
    }
    g_controller_connected[port] =
        controller && SDL_GameControllerGetAttached(controller);
    return g_controller_connected[port] ? controller : NULL;
}

/* Whether any controller is open: SDL must stay up for it at shutdown. */
int xbox_InputAnyOpen(void)
{
    int any = 0;

    ensure_init();
    update_controllers();
    SDL_LockJoysticks();
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        if (g_pads[i] && !SDL_GameControllerGetAttached(g_pads[i]))
            port_closed_locked(i);
        any |= g_pads[i] != NULL;
    }
    SDL_UnlockJoysticks();
    return any;
}

void xbox_InputInit(void)
{
    /* Keep the default SIGINT/SIGTERM: SDL would turn them into an SDL_QUIT
     * that nothing reads without a window, and Ctrl-C would stop working. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    ensure_init();
    /* Again on every call: the tests change the variables between calls. The
     * ports start over, with whoever is attached now. */
    padmap_init();
    SDL_LockJoysticks();
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        if (g_pads[i])
            SDL_GameControllerClose(g_pads[i]);
        g_pads[i] = NULL;
        g_controller_connected[i] = FALSE;
    }
    ports_init();
    refresh_controllers_locked();
    SDL_UnlockJoysticks();
}

unsigned xbox_InputPortMask(void)
{
    unsigned mask = 0;
    XBOX_GAMEPAD pad;

    ensure_init();
    update_controllers();
    SDL_LockJoysticks();
    refresh_controllers_locked();
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_pads[i] && SDL_GameControllerGetAttached(g_pads[i]))
            mask |= 1u << i;
    SDL_UnlockJoysticks();
    /* The stand-ins of port 1: the keyboard, the touch overlay. */
    {
        const char *kb = getenv("RECOMP_KEYBOARD");
        if (kb && *kb && *kb != '0')
            mask |= 1u;
    }
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (xbox_InputOverlayGet(i, &pad, NULL))
            mask |= 1u << i;
    return mask;
}

int xbox_InputPortName(DWORD dwPort, char *out, int size)
{
    int on;

    if (!out || size <= 0)
        return 0;
    out[0] = 0;
    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return 0;
    ensure_init();
    SDL_LockJoysticks();
    on = g_pads[dwPort] != NULL;
    if (on)
        snprintf(out, (size_t)size, "%s", g_names[dwPort]);
    SDL_UnlockJoysticks();
    return on;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    ensure_init();
    update_controllers();
    SDL_LockJoysticks();
    SDL_GameController *c = controller_for_port_locked(dwPort);
    if (!c) {
        SDL_UnlockJoysticks();
        if (dwPort == 0 && xbox_InputKeyboardPoll(pState)) {
            xbox_InputOverlayMerge(dwPort, pState);
            return ERROR_SUCCESS;
        }
        if (xbox_InputOverlayStandIn(dwPort, pState))
            return ERROR_SUCCESS;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    if (g_padmap_on) {
        padmap_read(c, &pState->Gamepad);
        SDL_UnlockJoysticks();
        if (dwPort == 0)
            keyboard_merge(pState);
        xbox_InputOverlayMerge(dwPort, pState);
        return ERROR_SUCCESS;
    }

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
    SDL_UnlockJoysticks();

    if (dwPort == 0)
        keyboard_merge(pState);
    xbox_InputOverlayMerge(dwPort, pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    ensure_init();
    update_controllers();
    SDL_LockJoysticks();
    SDL_GameController *c = controller_for_port_locked(dwPort);
    if (!c) {
        SDL_UnlockJoysticks();
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    SDL_UnlockJoysticks();
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    BOOL connected;

    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return FALSE;
    ensure_init();
    update_controllers();
    SDL_LockJoysticks();
    connected = controller_for_port_locked(dwPort) != NULL;
    SDL_UnlockJoysticks();
    return connected;
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    ensure_init();
    update_controllers();
    SDL_LockJoysticks();
    if (!controller_for_port_locked(dwPort)) {
        SDL_UnlockJoysticks();
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    SDL_UnlockJoysticks();
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
