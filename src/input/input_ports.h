/*
 * input_ports -- which host controller is which Xbox controller port (XBA-123).
 *
 * Pure logic, no SDL and no globals: the SDL backend (xinput_device.c) keeps
 * one InputPorts, calls Start once with the controllers present when it first
 * looks, then Attach / Detach as controllers come and go. The Android app
 * (PadPorts.java) predicts the same start for its Controls screen, so the two
 * follow the same rules (tests/input_pad_ports and PadPortsTest share cases).
 *
 * A controller is known by a key, "vvvv:pppp" (USB vendor and product, lower
 * case hex): both SDL and Android's InputDevice give those, where names differ
 * (SDL renames known pads) and serials are rare. Two pads of the same model
 * share a key; they are interchangeable, and take ports in the order they come.
 *
 * Rules
 *  - Start: the controllers present are packed from port 1, no holes: those
 *    pinned in RECOMP_PAD_PORTS first, in the order of their ports; then the
 *    handheld's built-in pad (RECOMP_PAD_BUILTIN, from the Android app); then
 *    the rest in host order. A pin whose controller is absent leaves no hole.
 *  - A port belongs to the controller that had it for the rest of the session:
 *    unplugged, it waits for that controller (same key) to come back.
 *  - A controller that arrives later takes, in this order: a free port that was
 *    its own; a free port pinned to it nobody has used; the lowest free port
 *    nobody has used and nobody pinned; the lowest free port nobody has used;
 *    the lowest free port (the absent owner loses it). None free: no port.
 */
#ifndef XBOX_INPUT_PORTS_H
#define XBOX_INPUT_PORTS_H

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_PORTS     4
#define INPUT_PORT_KEY  16      /* "vvvv:pppp" and its NUL, with room */

typedef struct InputPorts {
    char pin[INPUT_PORTS][INPUT_PORT_KEY];     /* preferred controller, "" = first come */
    char owner[INPUT_PORTS][INPUT_PORT_KEY];   /* whose port it is this session, "" = nobody yet */
    int  id[INPUT_PORTS];                      /* the attached controller's host id, -1 = none */
    char builtin[INPUT_PORT_KEY];              /* the handheld's own pad, "" = none */
    int  started;
} InputPorts;

/* "vvvv:pppp" into out (INPUT_PORT_KEY bytes). */
void xbox_InputPortsKey(char *out, unsigned vendor, unsigned product);

/* Reset, then read the pins and the built-in key. pins: RECOMP_PAD_PORTS,
 * "1=045e:0b13,2=2dc8:3106", each entry optionally followed by ":<name>" (the
 * app's label, ignored here); NULL or "" = no pins. builtin: "vvvv:pppp" or
 * NULL. Returns how many entries were not understood (ignored). */
int xbox_InputPortsInit(InputPorts *p, const char *pins, const char *builtin);

/* The controllers present at the first look, in host order: keys[i], ids[i].
 * out_port[i] gets each one's port (0..3) or -1 (more than four). */
void xbox_InputPortsStart(InputPorts *p, int n, const char *const keys[], const int ids[], int out_port[]);

/* A controller arrived (after Start): its port, or -1 when all four are taken. */
int xbox_InputPortsAttach(InputPorts *p, const char *key, int id);

/* A controller left: the port it had (kept as its own), or -1 if it had none. */
int xbox_InputPortsDetach(InputPorts *p, int id);

/* The port of an attached controller, or -1. */
int xbox_InputPortsPortOf(const InputPorts *p, int id);

#ifdef __cplusplus
}
#endif

#endif
