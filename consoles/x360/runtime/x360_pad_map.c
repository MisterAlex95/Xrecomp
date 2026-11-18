/* x360_pad_map.c -- x360_runtime.h's pure helpers (XBA-280): the title key of an XEX and a host pad as an Xbox 360
 * controller. libc and console_pad.h only (tests link it without SDL). */
#include <stdio.h>
#include <string.h>

#include "x360_runtime.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

/* The key comes from the XEX2 header's execution info (optional header 0x00040006: media id, version, base version,
 * title id, platform, executable table, disc number / count, save game id), which a plain rewrite keeps: the same
 * title gives the same key encrypted or plain. */
int x360_runtime_key(const uint8_t *xex, size_t size, char *out, size_t out_n)
{
    if (!xex || size < 24 || memcmp(xex, "XEX2", 4) || out_n < 24)
        return -1;
    uint32_t count = be32(xex + 20);
    if (count > 64 || 24 + (size_t)count * 8 > size)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *h = xex + 24 + 8 * i;
        if (be32(h) != 0x00040006u)
            continue;
        uint32_t at = be32(h + 4);
        if (at > size || size - at < 24)
            return -1;
        snprintf(out, out_n, "X360-%08X-%08X", be32(xex + at + 12), crc32_bytes(xex + at, 24));
        return 0;
    }
    return -1;
}

uint16_t x360_runtime_buttons_of(uint32_t t)
{
    static const uint16_t bit[X360_T_COUNT] = {
        0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0, 0, 0x0040, 0x0080, 0x0020, 0x0010,
        0x0001, 0x0002, 0x0004, 0x0008,
    };
    uint16_t b = 0;
    for (int i = 0; i < X360_T_COUNT; i++)
        if ((t >> i) & 1)
            b |= bit[i];
    if ((b & 0x3) == 0x3)
        b &= (uint16_t)~0x3;
    if ((b & 0xC) == 0xC)
        b &= (uint16_t)~0xC;
    return b;
}

static int16_t flip(int v)
{
    v = -v;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static uint8_t trigger(int v)
{
    v = v < 0 ? 0 : v > 32767 ? 32767 : v;
    return (uint8_t)((v * 255 + 16383) / 32767);
}

void x360_runtime_pad_of(const ConsolePadMap *m, const ConsolePadOut *o, int lt, int rt, x360_pad *out)
{
    memset(out, 0, sizeof *out);
    out->buttons = x360_runtime_buttons_of(o->buttons);
    /* the analog triggers: the host triggers when lt / rt are mapped to them, full when pressed through another
     * source */
    int lt_host = m && (m->source[X360_T_LT] & CPS_BIT(CPS_LEFT_TRIGGER));
    int rt_host = m && (m->source[X360_T_RT] & CPS_BIT(CPS_RIGHT_TRIGGER));
    out->left_trigger = (o->buttons >> X360_T_LT) & 1 ? (lt_host && lt ? trigger(lt) : 255) : 0;
    out->right_trigger = (o->buttons >> X360_T_RT) & 1 ? (rt_host && rt ? trigger(rt) : 255) : 0;
    if (lt_host && lt > 0 && !out->left_trigger)
        out->left_trigger = trigger(lt); /* below the button threshold: still analog */
    if (rt_host && rt > 0 && !out->right_trigger)
        out->right_trigger = trigger(rt);
    if (!m || m->stick[0] != CONSOLE_STICK_NONE)
        out->thumb_lx = o->stick_x[0], out->thumb_ly = flip(o->stick_y[0]);
    if (!m || m->stick[1] != CONSOLE_STICK_NONE)
        out->thumb_rx = o->stick_x[1], out->thumb_ry = flip(o->stick_y[1]);
}
