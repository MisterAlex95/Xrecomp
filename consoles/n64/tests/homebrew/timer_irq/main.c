/* N64 timer_irq (XBA-149): VI interrupt counting plus a continuous libdragon timer (COUNT/COMPARE
 * interrupt) at 100 Hz, for 60 frames; the COUNT register is checked against the frame count.
 * Result protocol: README.md. Our own code. */
#include <libdragon.h>
#include <stdint.h>

#include "../common/xba_result.h"

#define FRAMES 60

static volatile uint32_t vis, timer_hits, counting;

static void on_vi(void)
{
    if (counting) {
        vis++;
        XBA_RESULT->frames = vis;
    }
}

static void on_timer(int ovfl)
{
    (void)ovfl;
    timer_hits += counting;
}

int main(void)
{
    xba_begin(XBA_ID_TIMER);
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    surface_t *d = display_get();
    graphics_fill_screen(d, graphics_make_color(0, 0, 64, 255));
    display_show(d);

    timer_init();
    register_VI_handler(on_vi);

    /* Start on a VI edge. */
    counting = 1;
    while (vis == 0) {
    }
    vis = 0;
    timer_hits = 0;
    timer_link_t *t = new_timer(TIMER_TICKS(10000), TF_CONTINUOUS, on_timer);
    uint64_t t0 = get_ticks();
    while (vis < FRAMES) {
    }
    uint64_t dt = get_ticks() - t0;
    counting = 0;
    delete_timer(t);
    unregister_VI_handler(on_vi);

    /* NTSC/MPAL 60 frames = 1.001 s, PAL 50 Hz = 1.2 s. COUNT runs at 46.875 MHz. */
    int pal = get_tv_type() == TV_PAL;
    uint32_t secs_x1000 = pal ? 1200 : 1001;
    uint32_t hits_lo = secs_x1000 / 10 - 5, hits_hi = secs_x1000 / 10 + 3;
    uint64_t ticks = (uint64_t)TICKS_PER_SECOND * secs_x1000 / 1000;
    uint32_t i = 0;
    xba_check(i++, vis, FRAMES);
    xba_check(i++, timer_hits >= hits_lo && timer_hits <= hits_hi, 1);
    xba_check(i++, dt > ticks * 95 / 100 && dt < ticks * 105 / 100, 1);
    xba_check(i++, timer_hits, timer_hits); /* raw values, informative */
    xba_check(i++, (uint32_t)dt, (uint32_t)dt);
    xba_check(i++, (uint32_t)pal, (uint32_t)pal);

    graphics_fill_screen(d, XBA_RESULT->detail ? graphics_make_color(160, 0, 0, 255)
                                               : graphics_make_color(0, 160, 0, 255));
    xba_finish();
}
