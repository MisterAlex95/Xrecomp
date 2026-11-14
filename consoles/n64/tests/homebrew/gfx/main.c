/* N64 gfx (XBA-149): one deterministic 320x240 RGBA16 frame drawn by the RDP through rdpq:
 * fill rectangles (fill mode), a 16x16 RGBA16 texture blitted 1:1 (copy mode) and scaled 4x
 * (standard mode), and a gouraud triangle (standard mode, shade combiner). The program reads a few
 * framebuffer pixels back and checks them. Capture point: stage == 1 (README.md). Our own code. */
#include <libdragon.h>
#include <stdint.h>

#include "../common/xba_result.h"

static volatile uint32_t vis;
static void on_vi(void)
{
    vis++;
    XBA_RESULT->frames = vis;
}

static void wait_frames(uint32_t n)
{
    uint32_t end = vis + n;
    while (vis < end) {
    }
}

static uint16_t rgba16(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | 1); }

static uint16_t pixel(const surface_t *s, int x, int y)
{
    const volatile uint16_t *p = (const volatile uint16_t *)UncachedAddr(s->buffer);
    return p[y * (s->stride / 2) + x];
}

int main(void)
{
    xba_begin(XBA_ID_GFX);
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
    rdpq_init();
    register_VI_handler(on_vi);

    surface_t tex = surface_alloc(FMT_RGBA16, 16, 16);
    uint16_t *texels = (uint16_t *)UncachedAddr(tex.buffer);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            texels[y * (tex.stride / 2) + x] =
                ((x >> 2) ^ (y >> 2)) & 1 ? rgba16(255, 255, 255) : rgba16(255, 0, 128);

    surface_t *d = display_get();
    rdpq_attach_clear(d, NULL); /* black */

    rdpq_set_mode_fill(RGBA32(255, 0, 0, 255));
    rdpq_fill_rectangle(16, 16, 80, 64);
    rdpq_set_mode_fill(RGBA32(0, 0, 255, 255));
    rdpq_fill_rectangle(40, 80, 140, 120);

    rdpq_set_mode_copy(false);
    rdpq_tex_blit(&tex, 120, 16, NULL);

    rdpq_set_mode_standard();
    rdpq_tex_blit(&tex, 200, 16, &(rdpq_blitparms_t){ .scale_x = 4, .scale_y = 4 });

    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    float v1[] = { 160, 130, 1, 0, 0, 1 };
    float v2[] = { 300, 220, 0, 1, 0, 1 };
    float v3[] = { 40, 220, 0, 0, 1, 1 };
    rdpq_triangle(&TRIFMT_SHADE, v1, v2, v3);

    rdpq_detach_wait();
    display_show(d);

    uint32_t i = 0;
    xba_check(i++, pixel(d, 20, 20), rgba16(255, 0, 0));  /* fill rect 1 */
    xba_check(i++, pixel(d, 100, 100), rgba16(0, 0, 255)); /* fill rect 2 */
    xba_check(i++, pixel(d, 5, 5) >> 1, 0);                /* clear colour, alpha ignored */
    xba_check(i++, pixel(d, 121, 17), texels[1 * (tex.stride / 2) + 1]); /* 1:1 copy blit */
    uint16_t g = pixel(d, 160, 200);                       /* gouraud: all channels present */
    xba_check(i++, (g >> 11) > 2 && ((g >> 6) & 31) > 2 && ((g >> 1) & 31) > 2, 1);
    xba_check(i++, g, g);                                  /* raw, informative */

    wait_frames(2);
    XBA_RESULT->stage = 1; /* capture point */
    wait_frames(30);
    xba_finish();
}
