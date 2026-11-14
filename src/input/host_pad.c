/* host_pad.c -- see host_pad.h (XBA-145). */
#include "host_pad.h"
#include "console_pad.h"
#include "input_ports.h"

#include <SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static int s_init;
static InputPorts s_ports;
static SDL_GameController *s_pad[HOST_PAD_PORTS];

static void key_of(SDL_GameController *c, char *key)
{
    xbox_InputPortsKey(key, SDL_GameControllerGetVendor(c), SDL_GameControllerGetProduct(c));
}

static int port_of_instance_locked(SDL_JoystickID id)
{
    for (int p = 0; p < HOST_PAD_PORTS; p++)
        if (s_pad[p] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pad[p])) == id)
            return p;
    return -1;
}

static void attach_locked(int index, int at_start)
{
    SDL_GameController *c;
    SDL_JoystickID id;
    char key[INPUT_PORT_KEY];
    int port;

    if (!SDL_IsGameController(index))
        return;
    id = SDL_JoystickGetDeviceInstanceID(index);
    if (id >= 0 && port_of_instance_locked(id) >= 0)
        return;   /* already open: the ADDED event SDL queues for pads present at init */
    c = SDL_GameControllerOpen(index);
    if (!c)
        return;
    id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
    key_of(c, key);
    if (at_start) {
        const char *keys[1] = { key };
        int ids[1] = { (int)id };
        int out[1] = { -1 };
        xbox_InputPortsStart(&s_ports, 1, keys, ids, out);
        port = out[0];
    } else {
        port = xbox_InputPortsAttach(&s_ports, key, (int)id);
    }
    if (port < 0 || port >= HOST_PAD_PORTS || s_pad[port]) {
        SDL_GameControllerClose(c);
        return;
    }
    s_pad[port] = c;
    fprintf(stderr, "  [HOST] pad %d: %s (%s)\n", port + 1, SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?",
            key);
}

int host_pad_init(void)
{
    pthread_mutex_lock(&s_mu);
    if (s_init) {
        pthread_mutex_unlock(&s_mu);
        return 1;
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "  [HOST] pads: SDL game controllers: %s\n", SDL_GetError());
        pthread_mutex_unlock(&s_mu);
        return 0;
    }
    xbox_InputPortsInit(&s_ports, getenv("RECOMP_PAD_PORTS"), getenv("RECOMP_PAD_BUILTIN"));
    {
        /* The controllers present, packed from port 1 in one Start (input_ports.h). */
        int n = SDL_NumJoysticks(), k = 0;
        const char *keys[16];
        char keybuf[16][INPUT_PORT_KEY];
        int ids[16], out[16];
        SDL_GameController *open[16];
        for (int i = 0; i < n && k < 16; i++) {
            if (!SDL_IsGameController(i))
                continue;
            open[k] = SDL_GameControllerOpen(i);
            if (!open[k])
                continue;
            key_of(open[k], keybuf[k]);
            keys[k] = keybuf[k];
            ids[k] = (int)SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(open[k]));
            k++;
        }
        xbox_InputPortsStart(&s_ports, k, keys, ids, out);
        for (int i = 0; i < k; i++) {
            if (out[i] >= 0 && out[i] < HOST_PAD_PORTS && !s_pad[out[i]]) {
                s_pad[out[i]] = open[i];
                fprintf(stderr, "  [HOST] pad %d: %s (%s)\n", out[i] + 1,
                        SDL_GameControllerName(open[i]) ? SDL_GameControllerName(open[i]) : "?", keys[i]);
            } else {
                SDL_GameControllerClose(open[i]);
            }
        }
    }
    s_init = 1;
    pthread_mutex_unlock(&s_mu);
    return 1;
}

void host_pad_handle_event(const union SDL_Event *ev)
{
    if (!ev)
        return;
    if (ev->type == SDL_CONTROLLERDEVICEADDED) {
        pthread_mutex_lock(&s_mu);
        if (s_init)
            attach_locked(ev->cdevice.which, !s_ports.started);
        pthread_mutex_unlock(&s_mu);
    } else if (ev->type == SDL_CONTROLLERDEVICEREMOVED) {
        pthread_mutex_lock(&s_mu);
        int p = port_of_instance_locked((SDL_JoystickID)ev->cdevice.which);
        if (p >= 0) {
            xbox_InputPortsDetach(&s_ports, (int)ev->cdevice.which);
            SDL_GameControllerClose(s_pad[p]);
            s_pad[p] = NULL;
            fprintf(stderr, "  [HOST] pad %d: unplugged\n", p + 1);
        }
        pthread_mutex_unlock(&s_mu);
    }
}

void host_pad_update(void)
{
    SDL_Event ev;

    if (!s_init)
        return;
    SDL_PumpEvents();
    while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMOVED) > 0)
        host_pad_handle_event(&ev);
}

unsigned host_pad_mask(void)
{
    unsigned m = 0;
    pthread_mutex_lock(&s_mu);
    for (int p = 0; p < HOST_PAD_PORTS; p++)
        if (s_pad[p])
            m |= 1u << p;
    pthread_mutex_unlock(&s_mu);
    return m;
}

int host_pad_read(int port, HostPadState *out)
{
    SDL_GameController *c;

    if (!out)
        return 0;
    memset(out, 0, sizeof *out);
    if (port < 0 || port >= HOST_PAD_PORTS)
        return 0;
    pthread_mutex_lock(&s_mu);
    c = s_pad[port];
    if (c) {
        out->connected = 1;
        for (int b = 0; b < 15; b++)
            if (SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b))
                out->buttons |= 1u << b;
        out->axis[HOST_PAD_AXIS_LX] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        out->axis[HOST_PAD_AXIS_LY] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        out->axis[HOST_PAD_AXIS_RX] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
        out->axis[HOST_PAD_AXIS_RY] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
        out->axis[HOST_PAD_AXIS_LT] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        out->axis[HOST_PAD_AXIS_RT] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        snprintf(out->name, sizeof out->name, "%s", SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "");
    }
    pthread_mutex_unlock(&s_mu);
    return out->connected;
}

int host_pad_rumble(int port, uint16_t low, uint16_t high, uint32_t ms)
{
    int ok = 0;
    if (port < 0 || port >= HOST_PAD_PORTS)
        return 0;
    pthread_mutex_lock(&s_mu);
    if (s_pad[port])
        ok = SDL_GameControllerRumble(s_pad[port], low, high, ms) == 0;
    pthread_mutex_unlock(&s_mu);
    return ok;
}

int host_pad_rumble_env(void)
{
    const char *e = getenv("RECOMP_RUMBLE");
    return !(e && e[0] == '0');
}

static pthread_mutex_t s_overlay_mu = PTHREAD_MUTEX_INITIALIZER;
static HostPadState s_overlay;
static int s_overlay_on;

void host_pad_overlay_set(const HostPadState *st)
{
    pthread_mutex_lock(&s_overlay_mu);
    if (st) {
        s_overlay = *st;
        s_overlay.connected = 1;
    } else {
        memset(&s_overlay, 0, sizeof s_overlay);
    }
    s_overlay_on = st != NULL;
    pthread_mutex_unlock(&s_overlay_mu);
}

int host_pad_overlay_get(HostPadState *out)
{
    int on;
    if (!out)
        return 0;
    pthread_mutex_lock(&s_overlay_mu);
    on = s_overlay_on;
    *out = s_overlay;
    pthread_mutex_unlock(&s_overlay_mu);
    return on;
}

int console_pad_read(const ConsolePadMap *m, int port, ConsolePadOut *out)
{
    HostPadState pad, overlay;
    int have_pad = host_pad_read(port, &pad);
    int have_overlay = port == 0 && host_pad_overlay_get(&overlay);
    return console_pad_merge(m, have_pad ? &pad : NULL, have_overlay ? &overlay : NULL, out);
}

void host_pad_shutdown(void)
{
    pthread_mutex_lock(&s_mu);
    if (s_init) {
        for (int p = 0; p < HOST_PAD_PORTS; p++) {
            if (s_pad[p])
                SDL_GameControllerClose(s_pad[p]);
            s_pad[p] = NULL;
        }
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        s_init = 0;
    }
    pthread_mutex_unlock(&s_mu);
}
