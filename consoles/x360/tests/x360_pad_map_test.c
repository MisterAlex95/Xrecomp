/* x360_pad_map_test (XBA-280): x360_runtime.h's pure helpers -- the title key from an XEX's execution info, the
 * default CONSOLE_PAD_X360 mapping, a host pad as an Xbox 360 controller (buttons, sticks' y flipped, analog
 * triggers, a remap). No SDL. */
#include <stdio.h>
#include <string.h>

#include "x360_runtime.h"

static int failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

int main(void)
{
    /* an XEX2 header with one optional header: the execution info at 0x20, title id 4D5307E6 */
    uint8_t x[64] = "XEX2";
    put32(x + 20, 1);
    put32(x + 24, 0x00040006u);
    put32(x + 28, 0x20);
    put32(x + 0x20, 0x11223344u);      /* media id */
    put32(x + 0x20 + 4, 0x00010000u);  /* version */
    put32(x + 0x20 + 12, 0x4D5307E6u); /* title id */
    char key[48];
    CHECK(x360_runtime_key(x, sizeof x, key, sizeof key) == 0 && !strncmp(key, "X360-4D5307E6-", 14) &&
              strlen(key) == 22, "key %s", key);
    char key2[48];
    x[0x20 + 4] = 2; /* another version: another key */
    CHECK(x360_runtime_key(x, sizeof x, key2, sizeof key2) == 0 && strcmp(key, key2) != 0, "version in the key");
    CHECK(x360_runtime_key((const uint8_t *)"XEX1", 4, key, sizeof key) == -1, "not an XEX");

    ConsolePadMap m;
    CHECK(console_pad_defaults(&m, CONSOLE_PAD_X360) == 0 && m.n_targets == X360_T_COUNT && m.n_sticks == 2,
          "defaults");
    CHECK(console_pad_console("x360") == CONSOLE_PAD_X360 &&
              !strcmp(console_pad_env(CONSOLE_PAD_X360), "RECOMP_X360_PAD_MAP") &&
              !strcmp(console_pad_target_name(CONSOLE_PAD_X360, X360_T_LB), "lb"),
          "names");
    CHECK(x360_runtime_buttons_of(1u << X360_T_A | 1u << X360_T_START | 1u << X360_T_UP | 1u << X360_T_DOWN) ==
              (0x1000 | 0x0010),
          "buttons (up + down cancel)");

    HostPadState st;
    memset(&st, 0, sizeof st);
    st.buttons = HOST_PAD_A | HOST_PAD_RSHOULDER;
    st.axis[HOST_PAD_AXIS_LX] = 12000;
    st.axis[HOST_PAD_AXIS_LY] = -20000; /* SDL: up */
    st.axis[HOST_PAD_AXIS_LT] = 16384;  /* half */
    ConsolePadOut out;
    console_pad_apply(&m, &st, &out);
    x360_pad p;
    x360_runtime_pad_of(&m, &out, st.axis[HOST_PAD_AXIS_LT], st.axis[HOST_PAD_AXIS_RT], &p);
    CHECK(p.buttons == (0x1000 | 0x0200) && p.thumb_lx == 12000 && p.thumb_ly == 20000 && p.left_trigger == 128 &&
              p.right_trigger == 0,
          "pad: buttons %04X lx %d ly %d lt %u rt %u", p.buttons, p.thumb_lx, p.thumb_ly, p.left_trigger,
          p.right_trigger);
    /* a remap: A on east, LT on the right shoulder (pressed: full), the right stick off */
    CHECK(console_pad_parse(&m, "a=east,lt=right_shoulder,right_stick=none") == 0, "parse");
    st.buttons = HOST_PAD_B | HOST_PAD_RSHOULDER;
    st.axis[HOST_PAD_AXIS_RX] = 5000;
    console_pad_apply(&m, &st, &out);
    x360_runtime_pad_of(&m, &out, 0, 0, &p);
    CHECK((p.buttons & 0x1000) && p.left_trigger == 255 && p.thumb_rx == 0, "remap: %04X lt %u rx %d", p.buttons,
          p.left_trigger, p.thumb_rx);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("x360 pad map: all checks passed\n");
    return 0;
}
