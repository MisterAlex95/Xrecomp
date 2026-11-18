/* x360_input.c -- controllers and cheats' memory (XBA-270): what the host layer will drive (pads, cheats) once the
 * Xbox 360 is in the app, and what headless runs drive now (x360_run --pad-script).
 *
 * Pads: four XInput-style controllers. A host sets one (x360_set_pad: buttons in XINPUT's bits, triggers 0..255,
 * thumbs -32768..32767) or gives a script of timed states (virtual milliseconds since the title started, applied by
 * the scheduler: runs stay reproducible). XamInputGetState / GetCapabilities / SetState answer for a connected pad
 * (X_INPUT_STATE: packet number, then the gamepad; the packet number changes with the state), and
 * ERROR_DEVICE_NOT_CONNECTED for the others, as before.
 *
 * Cheats: 32-bit big-endian words at guest addresses, read and written only where memory is allocated (the image,
 * the heaps, the physical views); lifted code is compiled ahead, so patched code does not change what runs (data
 * cheats only, as for the other recompiled consoles' lifted code). */
#include <stdlib.h>
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))
#define X_ERROR_DEVICE_NOT_CONNECTED 0x48Fu

void x360_set_pad(x360_machine *x, int user, const x360_pad *p)
{
    if (user < 0 || user > 3)
        return;
    x360_pad_slot *s = &x->pads[user];
    if (!p) {
        s->connected = 0;
        return;
    }
    if (!s->connected || memcmp(&s->state, p, sizeof *p))
        s->packet++;
    s->state = *p;
    s->connected = 1;
}

int x360_set_pad_script(x360_machine *x, const x360_pad_event *events, unsigned n)
{
    free(x->pad_script);
    x->pad_script = NULL;
    x->pad_script_n = x->pad_script_at = 0;
    if (!n)
        return 0;
    x->pad_script = malloc(n * sizeof *events);
    if (!x->pad_script)
        return -1;
    memcpy(x->pad_script, events, n * sizeof *events);
    x->pad_script_n = n;
    return 0;
}

void x360_input_poll(x360_machine *x)
{
    uint64_t ms = x360_now(x) / 10000;
    while (x->pad_script_at < x->pad_script_n && x->pad_script[x->pad_script_at].ms <= ms) {
        const x360_pad_event *e = &x->pad_script[x->pad_script_at++];
        x360_set_pad(x, e->user, e->connected ? &e->pad : NULL);
    }
}

uint64_t x360_input_next_event(x360_machine *x)
{
    return x->pad_script_at < x->pad_script_n ? (uint64_t)x->pad_script[x->pad_script_at].ms * 10000 + 1 : 0;
}

static void put_gamepad(x360_machine *x, uint32_t p, const x360_pad *g)
{
    x360_w16(x, p, g->buttons);
    x360_w8(x, p + 2, g->left_trigger);
    x360_w8(x, p + 3, g->right_trigger);
    x360_w16(x, p + 4, (uint16_t)g->thumb_lx);
    x360_w16(x, p + 6, (uint16_t)g->thumb_ly);
    x360_w16(x, p + 8, (uint16_t)g->thumb_rx);
    x360_w16(x, p + 10, (uint16_t)g->thumb_ry);
}

static int user_of(uint32_t u)
{
    return u == 0xFF ? 0 : u < 4 ? (int)u : -1; /* 0xFF: any user (the first connected one) */
}

void x360_x_XamInputGetState(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int u = user_of(A(0) & 0xFF);
    if (u < 0 || !x->pads[u].connected) {
        RET(X_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    if (A(2)) { /* X_INPUT_STATE: packet number, gamepad */
        x360_w32(x, A(2), x->pads[u].packet);
        put_gamepad(x, A(2) + 4, &x->pads[u].state);
    }
    RET(0);
}

void x360_x_XamInputGetCapabilities(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (user, flags, X_INPUT_CAPABILITIES: type, subtype, flags, gamepad (the controls it has), vibration) */
    int u = user_of(A(0) & 0xFF);
    if (u < 0 || !x->pads[u].connected) {
        RET(X_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    uint32_t p = A(2);
    if (p) {
        x360_pad all = {0xF3FF, 0xFF, 0xFF, -1, -1, -1, -1};
        memset(x->base + p, 0, 20);
        x360_w8(x, p, 1);     /* XINPUT_DEVTYPE_GAMEPAD */
        x360_w8(x, p + 1, 1); /* XINPUT_DEVSUBTYPE_GAMEPAD */
        put_gamepad(x, p + 4, &all);
        x360_w16(x, p + 16, 0xFFFF);
        x360_w16(x, p + 18, 0xFFFF);
    }
    RET(0);
}

void x360_x_XamInputSetState(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int u = user_of(A(0) & 0xFF);
    /* XINPUT_VIBRATION: left (low frequency) and right (high frequency) motor speeds, 16 bits each */
    if (u >= 0 && x->pads[u].connected && A(2))
        x360_pad_set_motors(x, u, x360_r16(x, A(2)) >> 8, x360_r16(x, A(2) + 2) >> 8);
    RET(u >= 0 && x->pads[u].connected ? 0 : X_ERROR_DEVICE_NOT_CONNECTED);
}

void x360_x_XamInputGetCapabilitiesEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (unknown, user, flags, capabilities*): the plain call's arguments one register later */
    for (int r = 3; r < 10; r++)
        c->gpr[r] = c->gpr[r + 1];
    x360_x_XamInputGetCapabilities(x, t, c);
}

void x360_x_XamInputGetKeystrokeEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user*, flags, keystroke*) */
    uint32_t u = A(0) ? x360_r32(x, A(0)) : 0xFF;
    c->gpr[3] = u;
    x360_x_XamInputGetKeystroke(x, t, c);
}

void x360_x_XamInputGetKeystroke(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int u = user_of(A(0) & 0xFF);
    RET(u >= 0 && x->pads[u].connected ? 0x10D2u /* ERROR_EMPTY: no keystroke queued */ : X_ERROR_DEVICE_NOT_CONNECTED);
}

/* ---- cheats' memory ---- */

static int allocated(x360_machine *x, uint32_t a)
{
    if (a >= x->image_base && a - x->image_base < x->image_size)
        return 1;
    x360_heap *h = x360_heap_of(x, a);
    if (!h)
        return 0;
    uint32_t off = h->physical ? (a & 0x1FFFFFFFu) : a;
    if (off < h->base || off - h->base >= h->size)
        return 0;
    return h->used[(off - h->base) / h->page];
}

uint32_t x360_cheat_read32(x360_machine *x, uint32_t a)
{
    a &= ~3u;
    return allocated(x, a) ? x360_r32(x, a) : 0;
}

void x360_cheat_write32(x360_machine *x, uint32_t a, uint32_t v)
{
    a &= ~3u;
    if (allocated(x, a))
        x360_w32(x, a, v);
}
