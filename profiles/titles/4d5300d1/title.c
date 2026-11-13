/*
 * Fable: The Lost Chapters (Xbox, 2005), title ID 4D5300D1 -- the title profile.
 *
 * The title layer: tools.prepare puts it on top of the XDK layers and calls
 * the two hooks below (title.json "hooks") after theirs. Everything here was
 * found bringing the title up on macOS arm64 (see NOTES.md in this folder).
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
    /* Fable reserves 608 MB of address space for its own allocator and
     * sub-allocates across all of it. That used to need xbox_SetMapSize(1 GB)
     * here, and RECOMP_GUEST_WINDOW=1 in title.json because the desktop layout
     * then lost the contiguous window. Neither is set any more (XBA-103): the
     * whole 4 GB window is the default layout on every 64-bit host, and the
     * reserve is granted whole as a lazy reservation at the top of user space
     * (RECOMP_LAZY_RESERVE). Its 12+ worker threads alive during the
     * front-end load need no setting either: the stack pool grows with the
     * guest heap (XBA-104). title.json on Android caps the GPU renderer at
     * 30 fps, evenly paced: in
     * Oakvale a frame takes the executor 17-22 ms on an M1 and more on the
     * AYN Thor, so a 60 cap only lets the rate wander between 20 and 40
     * (docs/technical/gpu-backend.md, Android; the launcher's Frame cap wins).
     *
     * The XDK layers (title.json "layers") set the pushbuffer executor, the
     * AC'97 codec, the paced vblanks (the XMV decoder's clock is
     * D3DDevice_GetDisplayFieldStatus, sub_0084F650, which reads the vblank
     * count at device+0x1DE8), the one-CPU DISPATCH gate for XPP
     * (0x0086E480-0x00876C3C), the pad, and service D3D's GPU semaphore and
     * deferred RunPushBuffer (0x00855900; dispatcher sub_00859C40, helper
     * sub_00855860) with the addresses in title.json "params". */
    /* File-I/O completion APCs run at the next alertable wait, as the hardware
     * does: the runtime's default since XBA-102 (inline, the loader's
     * completion ran before the read call had returned and its bookkeeping
     * was set up). RECOMP_INLINE_APC=1 restores the inline delivery. */
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
