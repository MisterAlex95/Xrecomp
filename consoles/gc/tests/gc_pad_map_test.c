/* gc_pad_map_test -- the GameCube runtime's pure helpers (gc_pad_map.c, XBA-259): a host pad through the player's
 * mapping (console_pad, CONSOLE_PAD_GC) as a GameCube controller, and the DOL title key. No SDL, no corpus. */
#include <stdio.h>
#include <string.h>

#include "gc_runtime.h"

static int g_fail, g_checks;

#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        g_checks++;                                                                                                   \
        if (!(c)) {                                                                                                   \
            g_fail++;                                                                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                     \
        }                                                                                                             \
    } while (0)

static void apply(const ConsolePadMap *m, uint32_t buttons, int lx, int ly, int rx, int ry, int lt, int rt,
                  gc_pad_state *out)
{
    HostPadState st;
    ConsolePadOut o;
    memset(&st, 0, sizeof st);
    st.connected = 1;
    st.buttons = buttons;
    st.axis[HOST_PAD_AXIS_LX] = (int16_t)lx;
    st.axis[HOST_PAD_AXIS_LY] = (int16_t)ly;
    st.axis[HOST_PAD_AXIS_RX] = (int16_t)rx;
    st.axis[HOST_PAD_AXIS_RY] = (int16_t)ry;
    st.axis[HOST_PAD_AXIS_LT] = (int16_t)lt;
    st.axis[HOST_PAD_AXIS_RT] = (int16_t)rt;
    console_pad_apply(m, &st, &o);
    gc_runtime_pad_of(m, &o, lt, rt, out);
}

int main(void)
{
    ConsolePadMap m;
    gc_pad_state s;
    char key[32];

    /* axes: rest, ends, y inverted */
    CHECK(gc_runtime_axis(0, 0) == 128);
    CHECK(gc_runtime_axis(32767, 0) == 255);
    CHECK(gc_runtime_axis(-32768, 0) == 1);
    CHECK(gc_runtime_axis(-32767, 0) == 1);
    CHECK(gc_runtime_axis(32767, 1) == 1);
    CHECK(gc_runtime_axis(-32768, 1) == 255);
    CHECK(gc_runtime_axis(16384, 0) == 128 + 64);

    console_pad_defaults(&m, CONSOLE_PAD_GC);
    apply(&m, 0, 0, 0, 0, 0, 0, 0, &s);
    CHECK(s.connected && s.buttons == 0 && s.stick_x == 128 && s.stick_y == 128 && s.cstick_x == 128 &&
          s.cstick_y == 128 && s.trigger_l == 0 && s.trigger_r == 0);
    /* every button by name: A south, B east, X west, Y north, Z right shoulder, Start, the cross */
    apply(&m, HOST_PAD_A | HOST_PAD_B | HOST_PAD_X | HOST_PAD_Y | HOST_PAD_RSHOULDER | HOST_PAD_START |
                  HOST_PAD_DPAD_UP | HOST_PAD_DPAD_LEFT,
          0, 0, 0, 0, 0, 0, &s);
    CHECK(s.buttons == (GC_PAD_A | GC_PAD_B | GC_PAD_X | GC_PAD_Y | GC_PAD_Z | GC_PAD_START | GC_PAD_UP |
                        GC_PAD_LEFT));
    /* opposite directions cancel */
    apply(&m, HOST_PAD_DPAD_UP | HOST_PAD_DPAD_DOWN | HOST_PAD_DPAD_RIGHT, 0, 0, 0, 0, 0, 0, &s);
    CHECK(s.buttons == GC_PAD_RIGHT);
    /* sticks: the main stick is the left one, the C stick the right one, up is up */
    apply(&m, 0, 32767, -32768, -32768, 32767, 0, 0, &s);
    CHECK(s.stick_x == 255 && s.stick_y == 255 && s.cstick_x == 1 && s.cstick_y == 1);
    CHECK(s.buttons == 0); /* sticks are analog only */
    /* triggers: analog all the way, the click (digital L / R) only past 85 %, full then */
    apply(&m, 0, 0, 0, 0, 0, 16384, 0, &s);
    CHECK(s.trigger_l == 128 && s.trigger_r == 0 && !(s.buttons & GC_PAD_L));
    apply(&m, 0, 0, 0, 0, 0, 27000, 32767, &s);
    CHECK(s.trigger_l == 210 && !(s.buttons & GC_PAD_L));
    CHECK(s.trigger_r == 255 && (s.buttons & GC_PAD_R));
    /* a remapped L on a shoulder button: digital and analog follow the button */
    CHECK(console_pad_load(&m, CONSOLE_PAD_GC, "l=left_shoulder,stick=right,c_stick=left") == 0);
    apply(&m, HOST_PAD_LSHOULDER, 32767, 0, 0, -32768, 32767, 0, &s);
    CHECK((s.buttons & GC_PAD_L) && s.trigger_l == 255);
    CHECK(s.stick_x == 128 && s.stick_y == 255 && s.cstick_x == 255 && s.cstick_y == 128);
    apply(&m, 0, 0, 0, 0, 0, 32767, 0, &s);
    CHECK(!(s.buttons & GC_PAD_L) && s.trigger_l == 0); /* the host trigger is no longer L */

    /* the DOL's key */
    CHECK(gc_runtime_key((const uint8_t *)"123456789", 9, key, sizeof key) == 0);
    CHECK(strcmp(key, "DOL-CBF43926") == 0); /* CRC32 check value */
    CHECK(gc_runtime_key((const uint8_t *)"x", 1, key, 8) == -1);

    printf("gc_pad_map_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
