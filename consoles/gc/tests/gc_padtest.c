/* gc_padtest -- our libogc pad test (consoles/gc/tests/homebrew/padtest, XBA-259) on the core, headless, with
 * controller input injected on the serial interface (gc_set_pad), read back from what the program itself saw:
 * its `report` (magic "XBAPADT1", the values PAD_Read gave it), found in MEM1.
 *
 *   gc_padtest PADTEST.dol
 *
 *   1. port 1: A Z L <, sticks and triggers off centre -> the program reads each value, rumbles (Z held: the
 *      motor command reaches the SI), ports 2-4 say no controller
 *   2. port 1: every other button, sticks at their ends -> the new values, the motor stopped
 *   3. port 1 unplugged, port 4 plugged in -> port 1 gone, port 4 found (libogc's PAD_Reset probes it)
 *   4. the same program with the IPL's PAL state -> libogc picks PAL, 50 Hz fields, a 576-line XFB
 * and on the way the field lengths (NTSC 60 Hz, PAL 50 Hz) and the YUV scan-out (gc_xfb_yuv) against the RGB one.
 * Exit 0, 1 on a mismatch, 77 (skipped) when the DOL is not built (tools/consoles/build_homebrew.sh gc). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_core.h"

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

/* The program's struct report (padtest/main.c), big-endian in MEM1. */
typedef struct report {
    uint32_t fields, tv_mode;
    int32_t err[4];
    uint16_t button[4];
    int8_t stick_x[4], stick_y[4], substick_x[4], substick_y[4];
    uint8_t trigger_l[4], trigger_r[4];
    uint32_t rumble;
} report;

static uint32_t find_report(gc_machine *m)
{
    for (uint32_t a = 0x80003100u; a < 0x81800000u - 64; a += 4)
        if (gc_peek32(m, a) == 0x58424150u && gc_peek32(m, a + 4) == 0x41445431u) /* "XBAPADT1" */
            return a;
    return 0;
}

static uint8_t byte_at(gc_machine *m, uint32_t a)
{
    return (uint8_t)(gc_peek32(m, a & ~3u) >> (24 - 8 * (a & 3)));
}

static void read_report(gc_machine *m, uint32_t at, report *r)
{
    uint32_t p = at + 8;
    r->fields = gc_peek32(m, p);
    r->tv_mode = gc_peek32(m, p + 4);
    for (int i = 0; i < 4; i++)
        r->err[i] = (int32_t)gc_peek32(m, p + 8 + 4 * i);
    p += 24;
    for (int i = 0; i < 4; i++)
        r->button[i] = (uint16_t)(byte_at(m, p + 2 * i) << 8 | byte_at(m, p + 2 * i + 1));
    p += 8;
    for (int i = 0; i < 4; i++) {
        r->stick_x[i] = (int8_t)byte_at(m, p + i);
        r->stick_y[i] = (int8_t)byte_at(m, p + 4 + i);
        r->substick_x[i] = (int8_t)byte_at(m, p + 8 + i);
        r->substick_y[i] = (int8_t)byte_at(m, p + 12 + i);
        r->trigger_l[i] = byte_at(m, p + 16 + i);
        r->trigger_r[i] = byte_at(m, p + 20 + i);
    }
    r->rumble = gc_peek32(m, p + 24);
}

static int run(gc_machine *m, int fields)
{
    for (int f = 0; f < fields; f++)
        if (gc_run_frame(m)) {
            printf("FAIL: the machine stopped: %s\n", gc_error(m));
            gc_dump_state(m, stdout);
            return -1;
        }
    return 0;
}

/* The pad state as PAD_Read reports it: sticks less their origin (128), triggers as they are. */
static void expect_pad(const report *r, int port, const gc_pad_state *s, const char *what)
{
    CHECK(r->err[port] == 0, "%s: port %d err %d, want 0 (ok)", what, port + 1, r->err[port]);
    CHECK(r->button[port] == s->buttons, "%s: port %d buttons %04X, want %04X", what, port + 1, r->button[port],
          s->buttons);
    CHECK(r->stick_x[port] == s->stick_x - 128 && r->stick_y[port] == s->stick_y - 128,
          "%s: port %d stick %d %d, want %d %d", what, port + 1, r->stick_x[port], r->stick_y[port], s->stick_x - 128,
          s->stick_y - 128);
    CHECK(r->substick_x[port] == s->cstick_x - 128 && r->substick_y[port] == s->cstick_y - 128,
          "%s: port %d C stick %d %d, want %d %d", what, port + 1, r->substick_x[port], r->substick_y[port],
          s->cstick_x - 128, s->cstick_y - 128);
    CHECK(r->trigger_l[port] == s->trigger_l && r->trigger_r[port] == s->trigger_r,
          "%s: port %d triggers %u %u, want %u %u", what, port + 1, r->trigger_l[port], r->trigger_r[port],
          s->trigger_l, s->trigger_r);
}

static void check_yuv(gc_machine *m, int want_h)
{
    static uint8_t rgb[GC_XFB_MAX_W * GC_XFB_MAX_H * 3], yuv[GC_XFB_MAX_W * GC_XFB_MAX_H * 2];
    int w = 0, h = 0, w2 = 0, h2 = 0;
    CHECK(gc_xfb_rgb(m, rgb, &w, &h) == 0 && gc_xfb_yuv(m, yuv, &w2, &h2) == 0, "no XFB");
    CHECK(w == 640 && h == want_h && w2 == w && h2 == h, "XFB %dx%d / %dx%d, want 640x%d", w, h, w2, h2, want_h);
    /* the YUV lines converted the way gc_xfb_rgb does give its pixels (the GPU path's input is that picture) */
    int bad = 0, lit = 0;
    for (int y = 0; y < h && y < want_h; y++)
        for (int x = 0; x < w; x += 2) {
            const uint8_t *q = yuv + ((size_t)y * w + x) * 2;
            int cu = q[1] - 128, cv = q[3] - 128;
            for (int k = 0; k < 2; k++) {
                int yy = 76309 * (q[2 * k] - 16) + 32768, c[3];
                c[0] = (yy + 104597 * cv) >> 16;
                c[1] = (yy - 25675 * cu - 53279 * cv) >> 16;
                c[2] = (yy + 132201 * cu) >> 16;
                const uint8_t *o = rgb + ((size_t)y * w + x + k) * 3;
                for (int i = 0; i < 3; i++) {
                    int v = c[i] < 0 ? 0 : c[i] > 255 ? 255 : c[i];
                    bad += v != o[i];
                }
                lit += o[0] > 128;
            }
        }
    CHECK(bad == 0, "%d components differ between gc_xfb_yuv and gc_xfb_rgb", bad);
    CHECK(lit > 1000, "the screen is blank (%d lit pixels)", lit);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: gc_padtest PADTEST.dol\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("skipped: %s not built (tools/consoles/build_homebrew.sh gc)\n", argv[1]);
        return 77;
    }
    static uint8_t dol[4 << 20];
    size_t n = fread(dol, 1, sizeof dol, f);
    fclose(f);
    char why[160];
    gc_machine *m = gc_create(dol, n, why, sizeof why);
    if (!m) {
        printf("FAIL: %s: %s\n", argv[1], why);
        return 1;
    }

    /* 1 */
    gc_pad_state p1 = {true, GC_PAD_A | GC_PAD_Z | GC_PAD_L | GC_PAD_LEFT, 200, 30, 128, 255, 77, 255};
    gc_set_pad(m, 0, &p1);
    uint64_t ns0 = gc_field_ns(m);
    if (run(m, 1))
        return 1;
    uint64_t ns1 = gc_field_ns(m);
    CHECK(ns0 + ns1 > 33300000 && ns0 + ns1 < 33400000, "NTSC frame %llu + %llu ns, want 1/30 s",
          (unsigned long long)ns0, (unsigned long long)ns1);
    if (run(m, 59))
        return 1;
    uint32_t at = find_report(m);
    CHECK(at != 0, "no report in memory: the program did not start");
    if (!at)
        return 1;
    report r;
    read_report(m, at, &r);
    printf("report at %08X: %u fields, TV mode %u\n", at, r.fields, r.tv_mode);
    CHECK(r.fields >= 50, "only %u fields drawn in 60", r.fields);
    CHECK(r.tv_mode == 0, "TV mode %u, want 0 (NTSC)", r.tv_mode);
    expect_pad(&r, 0, &p1, "1");
    for (int p = 1; p < 4; p++)
        CHECK(r.err[p] == -1, "1: port %d err %d, want -1 (no controller)", p + 1, r.err[p]);
    CHECK(r.rumble == 1 && gc_pad_motor(m, 0) == 1, "1: Z held: rumble %X, motor %d, want 1 1", r.rumble,
          gc_pad_motor(m, 0));
    check_yuv(m, 480);

    /* 2 */
    gc_pad_state p2 = {true,
                       GC_PAD_B | GC_PAD_X | GC_PAD_Y | GC_PAD_START | GC_PAD_UP | GC_PAD_DOWN | GC_PAD_RIGHT | GC_PAD_R,
                       0, 255, 1, 128, 255, 0};
    gc_set_pad(m, 0, &p2);
    if (run(m, 10))
        return 1;
    read_report(m, at, &r);
    expect_pad(&r, 0, &p2, "2");
    CHECK(r.rumble == 0 && gc_pad_motor(m, 0) == 0, "2: Z released: rumble %X, motor %d, want 0 0", r.rumble,
          gc_pad_motor(m, 0));

    /* 3 */
    gc_pad_state p4 = {true, GC_PAD_START, 128, 128, 128, 128, 0, 0};
    gc_set_pad(m, 0, NULL);
    gc_set_pad(m, 3, &p4);
    if (run(m, 130))
        return 1;
    read_report(m, at, &r);
    CHECK(r.err[0] == -1, "3: port 1 err %d after unplugging, want -1", r.err[0]);
    expect_pad(&r, 3, &p4, "3");
    gc_destroy(m);

    /* 4 */
    m = gc_create(dol, n, why, sizeof why);
    gc_set_tv_mode(m, GC_TV_PAL);
    gc_set_pad(m, 0, &p1);
    ns0 = gc_field_ns(m);
    if (run(m, 1))
        return 1;
    ns1 = gc_field_ns(m);
    CHECK(ns0 + ns1 > 39900000 && ns0 + ns1 < 40100000, "PAL frame %llu + %llu ns, want 1/25 s",
          (unsigned long long)ns0, (unsigned long long)ns1);
    if (run(m, 59))
        return 1;
    at = find_report(m);
    CHECK(at != 0, "PAL: no report in memory");
    if (at) {
        read_report(m, at, &r);
        printf("PAL: %u fields, TV mode %u\n", r.fields, r.tv_mode);
        CHECK(r.tv_mode == 1, "PAL: TV mode %u, want 1 (VI_PAL)", r.tv_mode);
        CHECK(r.fields >= 50, "PAL: only %u fields drawn in 60", r.fields);
        expect_pad(&r, 0, &p1, "PAL");
        check_yuv(m, 576);
    }
    gc_destroy(m);
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("gc_padtest: ok (4 ports, buttons, sticks, triggers, rumble, hot plug, NTSC 60 Hz, PAL 50 Hz)\n");
    return 0;
}
