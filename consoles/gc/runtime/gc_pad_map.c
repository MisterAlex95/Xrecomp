/* gc_pad_map.c -- a host pad to a GameCube controller (XBA-259), see gc_runtime.h. Pure: libc and console_pad.h
 * only, so the tests link it without SDL. */
#include "gc_runtime.h"

#include <stdio.h>
#include <string.h>

uint16_t gc_runtime_buttons_of(uint32_t t)
{
    static const uint16_t bits[GC_T_COUNT] = {
        [GC_T_A] = GC_PAD_A, [GC_T_B] = GC_PAD_B, [GC_T_X] = GC_PAD_X, [GC_T_Y] = GC_PAD_Y, [GC_T_Z] = GC_PAD_Z,
        [GC_T_L] = GC_PAD_L, [GC_T_R] = GC_PAD_R, [GC_T_START] = GC_PAD_START, [GC_T_UP] = GC_PAD_UP,
        [GC_T_DOWN] = GC_PAD_DOWN, [GC_T_LEFT] = GC_PAD_LEFT, [GC_T_RIGHT] = GC_PAD_RIGHT,
    };
    uint16_t b = 0;
    for (int i = 0; i < GC_T_COUNT; i++)
        if (t & (1u << i))
            b |= bits[i];
    /* opposite directions at once: a controller's cross cannot */
    if ((b & (GC_PAD_UP | GC_PAD_DOWN)) == (GC_PAD_UP | GC_PAD_DOWN))
        b &= (uint16_t)~(GC_PAD_UP | GC_PAD_DOWN);
    if ((b & (GC_PAD_LEFT | GC_PAD_RIGHT)) == (GC_PAD_LEFT | GC_PAD_RIGHT))
        b &= (uint16_t)~(GC_PAD_LEFT | GC_PAD_RIGHT);
    return b;
}

uint8_t gc_runtime_axis(int v, int invert)
{
    long x = invert ? -(long)v : v;
    if (x < -32767)
        x = -32767;
    if (x > 32767)
        x = 32767;
    /* -32767..32767 -> 1..255, 0 -> 128 (rounded half away from zero, symmetric) */
    long q = x >= 0 ? (x * 127 + 16383) / 32767 : -((-x * 127 + 16383) / 32767);
    return (uint8_t)(128 + q);
}

/* A trigger's analog value: the host trigger(s) the target's sources name, full when its button is down. */
static uint8_t trigger_of(uint32_t sources, int lt, int rt, int pressed)
{
    int v = 0;
    if ((sources & CPS_BIT(CPS_LEFT_TRIGGER)) && lt > v)
        v = lt;
    if ((sources & CPS_BIT(CPS_RIGHT_TRIGGER)) && rt > v)
        v = rt;
    if (pressed)
        return 255;
    if (v <= 0)
        return 0;
    return (uint8_t)(v >= 32767 ? 255 : (v * 255 + 16383) / 32767);
}

void gc_runtime_pad_of(const ConsolePadMap *m, const ConsolePadOut *o, int lt, int rt, gc_pad_state *out)
{
    memset(out, 0, sizeof *out);
    out->connected = true;
    out->buttons = gc_runtime_buttons_of(o->buttons);
    out->stick_x = gc_runtime_axis(o->stick_x[0], 0);
    out->stick_y = gc_runtime_axis(o->stick_y[0], 1); /* SDL +y down, the GameCube's +y up */
    out->cstick_x = gc_runtime_axis(o->stick_x[1], 0);
    out->cstick_y = gc_runtime_axis(o->stick_y[1], 1);
    uint32_t ls = m && m->console == CONSOLE_PAD_GC ? m->source[GC_T_L] : CPS_BIT(CPS_LEFT_TRIGGER);
    uint32_t rs = m && m->console == CONSOLE_PAD_GC ? m->source[GC_T_R] : CPS_BIT(CPS_RIGHT_TRIGGER);
    out->trigger_l = trigger_of(ls, lt, rt, (out->buttons & GC_PAD_L) != 0);
    out->trigger_r = trigger_of(rs, lt, rt, (out->buttons & GC_PAD_R) != 0);
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

int gc_runtime_key(const uint8_t *dol, size_t size, char *out, size_t out_n)
{
    if (!out || out_n < 24)
        return -1;
    snprintf(out, out_n, "DOL-%08X", (unsigned)crc32_of(dol ? dol : (const uint8_t *)"", dol ? size : 0));
    return 0;
}
