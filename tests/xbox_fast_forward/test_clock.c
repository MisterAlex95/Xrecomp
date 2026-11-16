/* Fast-forward for Xbox titles (XBA-240), the pure parts on a fake clock:
 * guest_clock.c (the guest clock's scaling, continuity and monotonicity
 * across speed changes, guest spans on the host, presentation decimation, the
 * untouched 1x path) and xbox_pad_hotkeys.h (an Xbox pad's hotkey sources and
 * swallowing). guest_clock.c alone: xbox_HostNowNs is this file's. */
#include "guest_clock.h"
#include "input/xbox_pad_hotkeys.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint64_t s_now = 5000000000ull;   /* the fake host clock, ns */
static unsigned s_reads;
uint64_t xbox_HostNowNs(void) { s_reads++; return s_now; }

static void pure_clock(void)
{
    XboxGuestClock c;
    uint64_t t;

    xbox_GuestClockInit(&c);
    CHECK(c.speed == 1);
    CHECK(xbox_GuestClockExtraAt(&c, 0) == 0);
    CHECK(xbox_GuestClockExtraAt(&c, 123456789) == 0);

    /* x4 from t = 1 s: 3 ns gained per host ns. */
    xbox_GuestClockRetime(&c, 1000000000ull, 4);
    CHECK(xbox_GuestClockExtraAt(&c, 1000000000ull) == 0);
    CHECK(xbox_GuestClockExtraAt(&c, 1500000000ull) == 1500000000ull);   /* 0.5 s host = 2 s guest */
    CHECK(xbox_GuestClockExtraAt(&c, 999999999ull) == 0);                /* before the change */

    /* Back to x1 at 2 s: the 3 s gained stay, the clock goes on at 1:1. */
    xbox_GuestClockRetime(&c, 2000000000ull, 1);
    CHECK(c.speed == 1);
    CHECK(xbox_GuestClockExtraAt(&c, 2000000000ull) == 3000000000ull);
    CHECK(xbox_GuestClockExtraAt(&c, 9000000000ull) == 3000000000ull);

    /* x16 cap, 0 -> 1; a change "before" the last one is taken at it. */
    xbox_GuestClockRetime(&c, 3000000000ull, 99);
    CHECK(c.speed == XBOX_GUEST_CLOCK_MAX_SPEED);
    xbox_GuestClockRetime(&c, 2500000000ull, 0);
    CHECK(c.speed == 1 && c.host0 == 3000000000ull);
    CHECK(xbox_GuestClockExtraAt(&c, 3000000000ull) == 3000000000ull);

    /* Guest time never steps back across a run of changes. */
    xbox_GuestClockInit(&c);
    {
        uint64_t last = 0, h;
        uint32_t speeds[] = {1, 4, 2, 1, 16, 3, 1, 8};
        unsigned i = 0;
        for (h = 0; h < 4000000000ull; h += 7777777ull) {
            uint64_t g = h + xbox_GuestClockExtraAt(&c, h);
            CHECK(g >= last);
            last = g;
            if (h % 3 == 0)
                xbox_GuestClockRetime(&c, h, speeds[i++ % 8]);
        }
    }

    /* Units: whole seconds and the remainder, no overflow at the TSC's rate. */
    CHECK(xbox_GuestClockUnits(1500000000ull, 1000) == 1500);
    CHECK(xbox_GuestClockUnits(999999ull, 1000) == 0);
    CHECK(xbox_GuestClockUnits(12345678900ull, 10000000u) == 123456789);
    t = 3600ull * 1000000000ull;   /* an hour */
    CHECK(xbox_GuestClockUnits(t, 733333333ull) == 3600ull * 733333333ull);
    CHECK(xbox_GuestClockUnits(500000000ull, 733333333ull) == 366666666ull);

    /* Guest spans on the host: divided, rounded up, INFINITE kept, x1 as is. */
    CHECK(xbox_GuestClockHostSpan(100, 1) == 100);
    CHECK(xbox_GuestClockHostSpan(100, 0) == 100);
    CHECK(xbox_GuestClockHostSpan(100, 4) == 25);
    CHECK(xbox_GuestClockHostSpan(101, 4) == 26);
    CHECK(xbox_GuestClockHostSpan(1, 16) == 1);
    CHECK(xbox_GuestClockHostSpan(0, 4) == 0);
    CHECK(xbox_GuestClockHostMsAt(0xFFFFFFFFu, 4) == 0xFFFFFFFFu);
    CHECK(xbox_GuestClockHostMsAt(10, 4) == 3);
    CHECK(xbox_GuestClockHostMsAt(10, 1) == 10);
}

static void decimation(void)
{
    uint32_t n = 0, last = 1;
    int shown = 0, i;

    for (i = 0; i < 10; i++)
        shown += xbox_GuestClockShowNth(&n, &last, 1);
    CHECK(shown == 10);
    /* x4: one in four, the first at once. */
    shown = 0;
    CHECK(xbox_GuestClockShowNth(&n, &last, 4) == 1);
    for (i = 0; i < 11; i++)
        shown += xbox_GuestClockShowNth(&n, &last, 4);
    CHECK(shown == 2);   /* flips 5 and 9 of 12 */
    /* A new speed shows its first flip. */
    CHECK(xbox_GuestClockShowNth(&n, &last, 2) == 1);
    CHECK(xbox_GuestClockShowNth(&n, &last, 2) == 0);
    CHECK(xbox_GuestClockShowNth(&n, &last, 2) == 1);
    CHECK(xbox_GuestClockShowNth(&n, &last, 1) == 1);
    CHECK(xbox_GuestClockShowNth(&n, &last, 1) == 1);
}

static void process_clock(void)
{
    xbox_GuestClockReset();
    /* Never fast-forwarded: no clock read, nothing added, the host's time. */
    s_reads = 0;
    CHECK(!xbox_GuestClockActive());
    CHECK(xbox_GuestClockSpeed() == 1);
    CHECK(xbox_GuestClockExtraNs() == 0);
    CHECK(xbox_GuestClockExtraUnits(1000) == 0);
    CHECK(xbox_GuestClockHostMs(16) == 16);
    CHECK(xbox_GuestClockHostNs(16666667) == 16666667);
    CHECK(s_reads == 0);
    CHECK(xbox_GuestNowNs() == s_now);
    CHECK(xbox_GuestClockShowFrame() && xbox_GuestClockShowFrame());
    xbox_GuestClockSetSpeed(1);              /* no change: still never active */
    CHECK(!xbox_GuestClockActive());

    /* x4 for 100 ms: the guest's clock moves 400 ms. */
    xbox_GuestClockSetSpeed(4);
    CHECK(xbox_GuestClockActive());
    CHECK(xbox_GuestClockSpeed() == 4);
    {
        uint64_t g0 = xbox_GuestNowNs();
        s_now += 100000000ull;
        CHECK(xbox_GuestNowNs() - g0 == 400000000ull);
    }
    CHECK(xbox_GuestClockExtraNs() == 300000000ull);
    CHECK(xbox_GuestClockExtraUnits(1000) == 300);
    CHECK(xbox_GuestClockExtraUnits(10000000u) == 3000000);
    CHECK(xbox_GuestClockHostMs(16) == 4);
    CHECK(xbox_GuestClockHostNs(16666667) == 4166667);
    /* Decimated: one flip in four shown. */
    {
        int shown = 0, i;
        for (i = 0; i < 8; i++)
            shown += xbox_GuestClockShowFrame();
        CHECK(shown == 2);
    }

    /* Off: the gain stays, the clock moves 1:1 again. */
    xbox_GuestClockSetSpeed(1);
    CHECK(xbox_GuestClockActive());
    s_now += 50000000ull;
    CHECK(xbox_GuestClockExtraNs() == 300000000ull);
    CHECK(xbox_GuestNowNs() == s_now + 300000000ull);
    CHECK(xbox_GuestClockHostMs(16) == 16);

    /* Clamped to x16. */
    xbox_GuestClockSetSpeed(1000);
    CHECK(xbox_GuestClockSpeed() == XBOX_GUEST_CLOCK_MAX_SPEED);
    xbox_GuestClockReset();
    CHECK(!xbox_GuestClockActive() && xbox_GuestClockExtraNs() == 0);
}

static void pad_sources(void)
{
    XBOX_GAMEPAD g, ref;
    uint32_t s;

    memset(&g, 0, sizeof g);
    CHECK(xbox_pad_hotkey_sources(&g) == 0);
    CHECK(xbox_pad_hotkey_sources(NULL) == 0);
    /* The default ff_hold chord: Back + right trigger past half travel. */
    g.wButtons = XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_DPAD_UP;
    g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = 200;
    g.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 100;   /* not past half */
    g.bAnalogButtons[XBOX_BUTTON_A] = 255;
    g.bAnalogButtons[XBOX_BUTTON_WHITE] = 255;
    g.sThumbLY = 30000;    /* up, the Xbox's +y */
    g.sThumbRX = -30000;
    g.sThumbLX = 1000;     /* inside half travel */
    s = xbox_pad_hotkey_sources(&g);
    CHECK(s == (CPS_BIT(CPS_BACK) | CPS_BIT(CPS_DPAD_UP) | CPS_BIT(CPS_RIGHT_TRIGGER) | CPS_BIT(CPS_SOUTH)
                | CPS_BIT(CPS_RIGHT_SHOULDER) | CPS_BIT(CPS_LSTICK_UP) | CPS_BIT(CPS_RSTICK_LEFT)));

    /* Nothing swallowed: untouched. */
    ref = g;
    xbox_pad_hotkey_swallow(&g, 0);
    CHECK(memcmp(&g, &ref, sizeof g) == 0);
    /* The chord's two sources go; everything else stays. */
    xbox_pad_hotkey_swallow(&g, CPS_BIT(CPS_BACK) | CPS_BIT(CPS_RIGHT_TRIGGER));
    CHECK(g.wButtons == XBOX_GAMEPAD_DPAD_UP);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 100);
    CHECK(g.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(g.sThumbLY == 30000 && g.sThumbRX == -30000);
    /* A stick direction: centred only on that side. */
    xbox_pad_hotkey_swallow(&g, CPS_BIT(CPS_LSTICK_DOWN) | CPS_BIT(CPS_RSTICK_LEFT));
    CHECK(g.sThumbLY == 30000);
    CHECK(g.sThumbRX == 0);
}

int main(void)
{
    pure_clock();
    decimation();
    process_clock();
    pad_sources();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("xbox_fast_forward clock: all checks passed\n");
    return 0;
}
