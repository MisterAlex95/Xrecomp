/*
 * Fable: The Lost Chapters (Xbox, 2005), title ID 4D5300D1 -- the title layer.
 *
 * Optional enhancements only (profile-free WP6): the title runs without this
 * layer, on the toolkit's defaults (docs/technical/profiles.md). tools.prepare
 * calls the two hooks below (title.json "hooks") from xba_title_configure /
 * xba_title_start; --no-title-layer / XBA_NO_TITLE_LAYER=1 (the launcher's
 * « Enhancements » off) leaves them out. What they add:
 *   - 16:9 scenes (fable_aspect_configure, overrides.c);
 *   - the front end at the display rate instead of the game's own 20 fps.
 * History: NOTES.md in this folder.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern void xbox_SetDelayOverride(uint32_t ret_va, uint32_t from_ms, uint32_t to_ms);

/* ── Aspect ratio ─────────────────────────────────────────────────────────
 *
 * Fable can draw its 3D scenes for a 16:9 screen (overrides.c: the camera's
 * display-size override and the cutscene bars); the presenter then shows
 * those frames stretched to 16:9 and keeps menus and movies at 4:3. The
 * calls are src/video's (aspect_policy.c, declared in video_player.h; Win32
 * has stubs):
 *   xbox_AspectSetting()            RECOMP_ASPECT: 0 auto, 1 4:3, 2 16:9
 *   xbox_FramebufferWindowSetWide() this title's 3D frames are anamorphic 16:9 */
extern int xbox_AspectSetting(void);
extern void xbox_FramebufferWindowSetWide(int on);
extern int g_fable_aspect_wide;     /* overrides.c */

static void fable_aspect_configure(void)
{
    int setting = xbox_AspectSetting();
    int wide;

    /* Auto: wide on Android, where the screen is a 16:9 handheld (the AYN
     * Thor); 4:3 on a desktop, whose window keeps the console's shape. */
#ifdef __ANDROID__
    wide = setting != 1;
#else
    wide = setting == 2;
#endif
    if (!wide)
        return;
    g_fable_aspect_wide = 1;
    xbox_FramebufferWindowSetWide(1);
    fprintf(stderr, "[ASPECT] 16:9 scenes (setting %d)\n", setting);
}

void fable_configure(void)
{
    /* Nothing here is needed to run: below are the two enhancements. What
     * this hook used to set is the
     * toolkit's default now: the 608 MB reserve and the 4 GB window
     * (XBA-103), the stack pool (XBA-104), deferred file-I/O APCs (XBA-102),
     * the XDK switches, the D3D service and the DMA walk (recomp_xdk_defaults,
     * src/kernel/recomp_d3d_service.c: the device, its semaphore and
     * RunPushBuffer's methods found at run time), and the frame rate
     * (RECOMP_PACING=auto, WP4). */
    /* The front end (title screen, menus, loading screens) runs
     * do { draw; Sleep(50); } while (!done) in sub_0020BBB5, the two
     * `push 0x32; call Sleep` at 0x0020BD93 and 0x0020C3C5: 20 fps at most
     * by design, with the draw on top -- 7-8 fps on an AYN Thor, where the
     * draw is ~80 ms. The loading itself runs on other threads, which the
     * loop only waits for. FABLE_FRONTEND_SLEEP_MS=<ms> sets the sleep
     * (default 16; 50 is the game's own). */
    {
        const char *e = getenv("FABLE_FRONTEND_SLEEP_MS");
        int ms = e ? atoi(e) : 16;
        if (ms >= 0 && ms < 50) {
            xbox_SetDelayOverride(0x0020BD9Au, 50, (uint32_t)ms);   /* return addresses */
            xbox_SetDelayOverride(0x0020C3CCu, 50, (uint32_t)ms);
        }
    }
    /* Aspect ratio, above. */
    fable_aspect_configure();
}

/* The first run's background unpacking (d:\Data\...\*.biz to z:\data\...\*.big,
 * ~550 MB on the PAL disc, while the menus already run) used to have its own
 * progress thread here, with a percentage read from the .biz headers. The
 * runtime now says "preparing data <n> MB" in the window title, the overlay
 * and the log for any title that fills its utility drive
 * (src/kernel/prep_progress.h, WP7), so nothing is left to start. */
void fable_start(const char *game_dir, const char *save_dir)
{
    (void)game_dir;
    (void)save_dir;
}
