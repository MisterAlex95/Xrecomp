/* N64 hello (XBA-149): draws a fixed text with libdragon's software text renderer and writes known
 * values to the result block. Result protocol: README.md. Our own code. */
#include <libdragon.h>
#include <stdint.h>

#include "../common/xba_result.h"

static volatile uint32_t in_data = 0xC0FFEE01;
static volatile uint32_t in_bss;

int main(void)
{
    xba_begin(XBA_ID_HELLO);

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    surface_t *d = display_get();
    graphics_fill_screen(d, graphics_make_color(0, 0, 64, 255));
    graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
    graphics_draw_text(d, 16, 16, "XBA HELLO N64");
    graphics_draw_text(d, 16, 32, "homebrew corpus XBA-149");
    display_show(d);

    /* table[0] = known constant, table[1] = .data loaded and .bss cleared,
     * table[2] = TV type reported by the boot code (0 PAL, 1 NTSC, 2 MPAL; informative). */
    xba_check(0, 0x12345678, 0x12345678);
    xba_check(1, in_data + in_bss, 0xC0FFEE01);
    uint32_t tv = (uint32_t)get_tv_type();
    xba_check(2, tv, tv);
    xba_finish();
}
