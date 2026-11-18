/* The app shell's SDL_main (XBA-18): what every title's APK starts from.
 *
 * recomp_app_start() (src/platform/recomp_app.c, XBA-52) does the start-up:
 * - stdout / stderr go to logcat (tag "xboxrecomp") before anything prints;
 * - RECOMP_* options from <external files dir>/recomp.cfg, before any library
 *   starts (XBA-26), then the Android defaults (RECOMP_FB_WINDOW,
 *   RECOMP_PB_EXEC on);
 * - the game data in <external files dir>/game/ (or RECOMP_GAME_DIR), the
 *   saves in <internal files dir>/save/, handed to xbox_path_init (XBA-23);
 * - the runtime on the fast cores of a big.LITTLE SoC, before any thread is
 *   created, so every thread inherits it (XBA-25);
 * - the trapped-MMIO fault handler, chained to debuggerd / ART so real
 *   crashes keep their tombstone (XBA-22); checked again here after SDL_Init.
 *
 * With RECOMP_SHELL_SELFTEST=1 in recomp.cfg, the self-test (XBA-51,
 * src/selftest/recomp_selftest.h) runs next, after SDL_Init, in this app
 * process: the memory layout on the synthetic XBE shipped as the asset
 * selftest.xbe, its mirrors, trapped MMIO, execute protections, the save
 * dir; one « selftest NAME: PASS / FAIL » line each and a summary. The shell
 * then goes on as below.
 *
 * Then "xboxrecomp ready" with the build version, and:
 * - a title linked in (it defines recomp_title_main, XBA-52) and default.xbe
 *   found: xbox_HostWindowMain runs the title on its guest thread, with the
 *   window and the lifecycle; quitting is the in-game menu's (Android Back
 *   opens it in ShellActivity: GameMenu, menu_jni.c), which ends the
 *   process with recomp_quit (flush, drain logcat, _exit(0): « clean
 *   exit »); a title that leaves by itself (the dashboard, an image this
 *   build cannot start) goes back to the launcher, which says why
 *   (exit_via_launcher, ShellActivity.titleExited), then recomp_quit;
 * - no title linked in and default.xbe found: the title pack for its title
 *   ID, <internal files dir>/packs/<TITLEID>/ (pack_loader.h), dlopened and
 *   run the same way (PACK_LOADED). An invalid pack or a dlopen error
 *   (PACK_INVALID, PACK_DLOPEN) hands back to the launcher
 *   (ShellActivity.openLauncherBadPack), which drops the pack and offers
 *   Prepare again; no pack (PACK_MISSING) goes on as below;
 * - a Game Boy Advance ROM (RECOMP_GAME_DIR names the file, or "fd:N" for
 *   a document; XBA-157): the GBA runtime plays it, no pack needed
 *   (gba_runtime_start: window, pads, audio, saves under <internal>/save/
 *   gba-<KEY>/), then the process ends (recomp_quit); a ROM that cannot be
 *   read goes back to the launcher (exit_via_launcher);
 * - a PlayStation disc or EXE (the same way; XBA-168): the PS1 runtime
 *   plays it with the OpenBIOS the APK carries (ps1_runtime_start: memory
 *   cards under <internal>/save/ps1-<KEY>/);
 * - a Nintendo 64 cartridge (.z64 / .v64 / .n64, the same way; XBA-174): the
 *   N64 runtime plays it, no BIOS (n64_runtime_start: saves under
 *   <internal>/save/n64-<KEY>/);
 * - otherwise the shell's own black, fullscreen, landscape window. With
 *   default.xbe found but no title linked and no pack, a message box says
 *   so (NO_TITLE) instead of a bare black screen. When
 *   default.xbe is not there (NO_GAME_DATA) it hands over to the game picker
 *   (ShellActivity.openGamePicker, XBA-54), which names the folder and lets
 *   the player choose the game; a message box if that call fails. In the
 *   app, ShellActivity takes Android Back first (the in-game menu, whose
 *   Quit ends the process); the Back-twice loop below (QUIT_PROMPT toast,
 *   twice within 2 s returns from SDL_main) only runs where nothing above
 *   SDL takes Back. "lifecycle: paused" / "resumed after N ms" when the app
 *   leaves and comes back (XBA-24).
 *
 * A title does not replace this file: it is linked next to it (android/app's
 * CMake option XBA_TITLE_DIR, docs/android/app-shell.md). */
#include <SDL.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/recomp_app.h"
#include "platform/recomp_lifecycle.h"
#include "platform/recomp_quit.h"
#include "recomp_selftest.h"
#include "kernel/xbox_memory_layout.h"
#include "video/video_player.h"     /* xbox_HostWindowMain */

#include "pack_loader.h"
#include "gba_runtime.h"
#include "n64_runtime.h"
#include "gc_disc.h"
#include "gc_runtime.h"
#include "x360_runtime.h"
#include "ps1_runtime.h"
#include "ps1_identify.h"

#ifndef XBA_VERSION
#define XBA_VERSION "dev"
#endif

#define BACK_WINDOW_MS 2000

/* User-facing text, named by the key the spec and the tests use. English
 * only until the app has a strings table of its own. */
#define TEXT_QUIT_PROMPT "Press Back again to quit"
#define TEXT_NO_GAME_DATA \
    "No game data found.\n\n" \
    "Copy the extracted disc (default.xbe and its files) to:\n%s\n\n" \
    "for example: adb push <disc folder>/. %s/"
#define TEXT_NO_TITLE \
    "Game data found:\n%s\n\n" \
    "This APK has no recompiled game in it (the plain shell), so there is " \
    "nothing to run and the screen stays black. Install the APK built for " \
    "this game."
#define TEXT_BAD_PACK \
    "The prepared game cannot be loaded:\n%s\n\n" \
    "Open the app again and prepare the game again."

/* The four symbols a title provides (its generated dispatch and one title's
 * front-end hooks: recomp_lookup, recomp_lookup_manual, g_fe_cursor,
 * fe_menu_get_pb_state) are weak forwarders in pack_loader.c: a title linked
 * in defines them strong and wins; otherwise they reach the loaded pack, or
 * stand in. libmain links the whole toolkit, so that loading it on a device
 * proves bionic resolves every symbol the runtime uses. */

/* The title's boot (recomp_app.h). Weak and undefined without a title: the
 * address is then NULL and the shell looks for a pack. */
extern int recomp_title_main(const recomp_storage *store) __attribute__((weak));

static const recomp_storage *s_title_store;
static xba_title_main_fn s_title_main;

/* xbox_HostWindowMain's guest_main, on the guest thread. */
static int title_boot(void)
{
    return s_title_main(s_title_store);
}

/* RECOMP_SHELL_SELFTEST=1 (recomp.cfg, XBA-51). The synthetic XBE of
 * tools/conformance (ours, no game content) is the APK asset selftest.xbe
 * (android/app/build.gradle); SDL_LoadFile reads a relative path through the
 * asset manager on Android. The layout takes the bytes, so no copy on disk. */
static void run_selftest(const recomp_storage *store)
{
    size_t len = 0;
    void *xbe = SDL_LoadFile("selftest.xbe", &len);

    if (!xbe)
        fprintf(stderr, "selftest: cannot read the asset selftest.xbe: %s\n", SDL_GetError());
    recomp_selftest_run(xbe, xbe ? len : 0, store->save_dir[0] ? store->save_dir : NULL);
    SDL_free(xbe);
}

static int selftest_wanted(void)
{
    const char *e = getenv("RECOMP_SHELL_SELFTEST");
    return e && *e && strcmp(e, "0") != 0;
}

/* Calls void ShellActivity.<method>(String arg). Called on SDLThread, a Java
 * thread: SDL_AndroidGetJNIEnv is its env. The method is looked up on the
 * activity's own class, not with FindClass. 0 when the call was made. */
static int call_activity(const char *method, const char *arg)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = env ? (jobject)SDL_AndroidGetActivity() : NULL;
    int rc = -1;

    if (!act)
        return -1;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetMethodID(env, cls, method, "(Ljava/lang/String;)V") : NULL;
    jstring jarg = m ? (*env)->NewStringUTF(env, arg) : NULL;
    if (jarg) {
        (*env)->CallVoidMethod(env, act, m, jarg);
        rc = 0;
        (*env)->DeleteLocalRef(env, jarg);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        rc = -1;
    }
    if (cls)
        (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return rc;
}

/* NO_GAME_DATA: ShellActivity.openGamePicker(dir) (XBA-54) starts the game
 * picker naming the folder and finishes this activity, so SDL_QUIT follows. */
static int open_game_picker(const char *dir)
{
    return call_activity("openGamePicker", dir);
}

/* PACK_INVALID / PACK_DLOPEN: ShellActivity.openLauncherBadPack(reason)
 * opens the launcher, which drops the pack and offers Prepare again, and
 * finishes this activity (SDL_QUIT follows). */
static int open_launcher_bad_pack(const char *reason)
{
    return call_activity("openLauncherBadPack", reason);
}

/* Quick reboot into the running title (src/kernel/kernel_reboot.c). The
 * kernel has written the launch data to the save dir; a process cannot start
 * the runtime twice, so ShellActivity.restartGame asks the :launcher process
 * to end this one and start the game activity again (LauncherActivity
 * ACTION_RESTART_GAME). Called on the guest thread: SDL_AndroidGetJNIEnv
 * attaches it. Then this thread waits to be ended; if the launcher has not
 * done it within RESTART_WAIT_MS the process exits by itself, leaving the
 * launch data for the next start. Returns (the kernel then exits as before)
 * only when the call could not be made. Declared here rather than through
 * kernel.h, which brings the Xbox types. */
typedef void (*xbox_restart_handler)(void);
void xbox_SetRestartHandler(xbox_restart_handler fn);

#define RESTART_WAIT_MS 10000

static void restart_via_launcher(void)
{
    if (call_activity("restartGame", "") != 0) {
        fprintf(stderr, "xboxrecomp: quick reboot: ShellActivity.restartGame failed\n");
        return;
    }
    printf("xboxrecomp: quick reboot: launcher asked to restart the game\n");
    fflush(stdout);
    SDL_Delay(RESTART_WAIT_MS);
    printf("xboxrecomp: quick reboot: not restarted after %d ms, exiting\n", RESTART_WAIT_MS);
    recomp_quit(0);
}

/* XLaunchNewImage to another image of the disc (XBA-82): the kernel has
 * written the launch data and looked the image up in the pack's images.map;
 * the next start runs that image's library (pack_loader.c), the restart is
 * the quick reboot's. Returns only when it could not. */
typedef void (*xbox_image_handler)(const char *target);
void xbox_SetImageHandler(xbox_image_handler fn);

static void image_via_launcher(const char *target)
{
    /* Named by default.xbe but not prepared yet (lazy, docs/android/prepare.md
     * « Images »): <work>/image.pending, then ShellActivity.prepareImage opens
     * the launcher, which offers « Prepare <image> » and starts that image
     * once it is in the pack. The launch data stays in the save dir for that
     * start (this does not return, so the kernel does not delete it). */
    if (xba_pack_image_state(target) == RECOMP_PACK_IMAGE_LAZY) {
        if (xba_pack_set_pending_image(target) != 0) {
            fprintf(stderr, "xboxrecomp: launch of %s: cannot write image.pending\n", target);
            return;
        }
        if (call_activity("prepareImage", target) != 0) {
            fprintf(stderr, "xboxrecomp: launch of %s: ShellActivity.prepareImage failed\n", target);
            return;
        }
        printf("PACK_IMAGE_LAZY %s: not prepared yet, the launcher offers to prepare it\n", target);
        recomp_quit(0);
    }
    if (xba_pack_set_next_image(target) != 0) {
        fprintf(stderr, "xboxrecomp: launch of %s: cannot write image.next\n", target);
        return;
    }
    printf("xboxrecomp: launch of %s: restarting into it\n", target);
    restart_via_launcher();
}

/* A quick reboot of a secondary image restarts that image, not default.xbe. */
static void restart_image_via_launcher(void)
{
    const char *img = getenv("RECOMP_IMAGE");
    if (img && *img && xba_pack_set_next_image(img) != 0)
        fprintf(stderr, "xboxrecomp: quick reboot of %s: cannot write image.next\n", img);
    restart_via_launcher();
}

/* The title left by itself (HalReturnToFirmware to the dashboard, an image
 * this build cannot start, HalInitiateShutdown; xbox_title_exit in
 * src/kernel/kernel_reboot.c). ShellActivity.titleExited(why) opens the
 * launcher, which shows why, and finishes this activity; it returns once the
 * launcher was asked. The kernel then ends this process with recomp_quit
 * (flush, drain logcat, _exit): no exit() under the running guest threads,
 * no tombstone. Called on the guest thread. */
typedef void (*xbox_exit_handler)(const char *why);
void xbox_SetExitHandler(xbox_exit_handler fn);

static void exit_via_launcher(const char *why)
{
    if (call_activity("titleExited", why ? why : "") != 0) {
        fprintf(stderr, "xboxrecomp: title exit: ShellActivity.titleExited failed\n");
        return;
    }
    printf("xboxrecomp: title exit: launcher opened (%s)\n", why ? why : "");
}

/* Background / foreground (XBA-24): the same watch as the title's window
 * (fb_present_sdl.c), so logcat shows "lifecycle: paused" / "resumed after
 * N ms" on the shell too. */
static int SDLCALL lifecycle_watch(void *user, SDL_Event *ev)
{
    (void)user;
    if (ev->type == SDL_APP_WILLENTERBACKGROUND)
        recomp_lifecycle_pause();
    else if (ev->type == SDL_APP_DIDENTERFOREGROUND)
        recomp_lifecycle_resume();
    return 1;
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    /* A game of another console (a GBA ROM, XBA-157) named by RECOMP_GAME_DIR, a path or "fd:N" from
     * ShellActivity: no Xbox disc image to mount from it (the mount would close that descriptor on its way out). */
    const char *gd = getenv("RECOMP_GAME_DIR");
    ps1_identify_install();   /* a .chd PlayStation disc gets its title key through the PS1 disc reader (XBA-159) */
    gc_disc_identify_install();   /* a .gcz GameCube disc through the GameCube disc readers (XBA-261) */
    if (gd && *gd && xba_pack_rom_console(gd) != RECOMP_CONSOLE_UNKNOWN)
        setenv("RECOMP_DISC_IMAGE", "0", 1);
    /* Start-up steps, shared with every title (recomp_app.h). */
    recomp_storage store;
    int have_game = recomp_app_start(SDL_AndroidGetExternalStoragePath(),
                                     SDL_AndroidGetInternalStoragePath(), &store);
    s_title_main = recomp_title_main;

    /* Back is ours: the shell decides when it quits (twice within 2 s). */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    /* SDL may have installed handlers of its own: make sure ours is still
     * in front (it chains to theirs). */
    xbox_PosixInstallMmioFaultHandler();

    /* In the app process, with SDL and ART's handlers in place, before any
     * title maps its own layout (the self-test shuts its one down). */
    if (selftest_wanted())
        run_selftest(&store);

    /* No title linked in: the pack the on-device Prepare built for this
     * default.xbe, if any (pack_loader.h). */
    int pack_rc = RECOMP_PACK_MISSING;
    char pack_reason[512] = "";
    if (have_game > 0 && !s_title_main)
        pack_rc = xba_pack_load(SDL_AndroidGetInternalStoragePath(), store.xbe, &s_title_main,
                                pack_reason, sizeof pack_reason);
    /* Another console's game (XBA-147): RECOMP_GAME_DIR names its file (a
     * .gba, a PS1 disc, an N64 cartridge), which is no Xbox disc, so
     * have_game is 0; it goes to that console's runtime (pack_loader.h).
     * The GBA runtime plays it (XBA-157: no pack needed, saves under
     * <internal>/save/gba-<KEY>/) and returns once the player quit: the
     * process ends then, or goes back to the launcher saying why when the
     * game could not start. The PS1 runtime the same (XBA-168: its disc or
     * EXE through the OpenBIOS asset, memory cards under
     * <internal>/save/ps1-<KEY>/). The N64 runtime the same (XBA-174: the
     * cartridge as it is, no BIOS, saves under <internal>/save/n64-<KEY>/).
     * The GameCube runtime the same (XBA-259: a DOL or a disc, no IPL,
     * memory cards under <internal>/save/gc-<KEY>/, XBA-264). The Xbox 360
     * runtime (XBA-280) needs its pack (bringup --app-pack): without one
     * the shell says PACK_MISSING and goes back to the launcher. */
    int console = !s_title_main && have_game <= 0 && store.game_dir[0] ? xba_pack_rom_console(store.game_dir)
                                                                        : RECOMP_CONSOLE_UNKNOWN;
    int console_game = console != RECOMP_CONSOLE_UNKNOWN;
    if (console_game) {
        int console_rc = -1;
        if (console == RECOMP_CONSOLE_GBA)
            gba_runtime_set_save_dir(store.save_dir);
        if (console == RECOMP_CONSOLE_PS1)
            ps1_runtime_set_save_dir(store.save_dir);   /* memory cards: <internal>/save/ps1-<KEY>/card1.mcr */
        if (console == RECOMP_CONSOLE_N64)
            n64_runtime_set_save_dir(store.save_dir);   /* <internal>/save/n64-<KEY>/game.eep, pak1.mpk... */
        if (console == RECOMP_CONSOLE_GC)
            gc_runtime_set_save_dir(store.save_dir);    /* memory cards: <internal>/save/gc-<KEY>/cardA.raw */
        if (console == RECOMP_CONSOLE_X360)
            x360_runtime_set_save_dir(store.save_dir);  /* XBA-280: states in <internal>/save/x360-<KEY>/ */
        pack_rc = xba_pack_load_rom(SDL_AndroidGetInternalStoragePath(), store.game_dir, &console_rc,
                                    pack_reason, sizeof pack_reason);
        if (pack_rc == RECOMP_PACK_OK)
            printf("xboxrecomp: %s runtime returned %d\n", recomp_pack_console_name(console), console_rc);
        else
            printf("xboxrecomp: no usable pack for %s (%s)\n", store.game_dir, pack_reason);
        if ((console == RECOMP_CONSOLE_GBA || console == RECOMP_CONSOLE_PS1 || console == RECOMP_CONSOLE_N64
             || console == RECOMP_CONSOLE_GC || console == RECOMP_CONSOLE_X360)
            && pack_rc == RECOMP_PACK_OK) {
            if (console_rc != 0)
                exit_via_launcher(console == RECOMP_CONSOLE_PS1
                                      ? "the game cannot start: its disc or the PlayStation BIOS cannot be read"
                                      : "the game file cannot be read");
            recomp_quit(console_rc != 0 ? 1 : 0);
        }
        if (console == RECOMP_CONSOLE_X360 && pack_rc != RECOMP_PACK_OK) {
            /* XBA-280: an Xbox 360 title runs only from its pack (its code recompiled ahead) */
            exit_via_launcher(pack_rc == RECOMP_PACK_MISSING
                                  ? "this Xbox 360 game has no pack yet: make one on a computer "
                                    "(python3 -m x360_tools.bringup ... --app-pack) and copy it into files/packs/"
                                  : "this Xbox 360 game's pack cannot be used (see the log)");
            recomp_quit(1);
        }
    }
    int run_title = have_game > 0 && s_title_main != NULL;
    if (run_title && xba_pack_image_xbe())   /* another image of the disc (XBA-82) */
        SDL_strlcpy(store.xbe, xba_pack_image_xbe(), sizeof store.xbe);

    SDL_version sdl;
    SDL_GetVersion(&sdl);
    if (run_title) {
        printf("xboxrecomp ready (build %s, SDL %d.%d.%d, title)\n",
               XBA_VERSION, sdl.major, sdl.minor, sdl.patch);
        printf("xboxrecomp: game data: %s\n", store.xbe);
        printf("xboxrecomp: starting the title\n");
        /* The title's window owns this thread from here: Back twice ends the
         * process there (recomp_quit). It returns only when the window is
         * off (RECOMP_HEADLESS=1, RECOMP_FB_WINDOW=0, SDL video failing) and
         * the title returned. */
        s_title_store = &store;
        xbox_SetRestartHandler(xba_pack_image_xbe() ? restart_image_via_launcher : restart_via_launcher);
        xbox_SetImageHandler(image_via_launcher);
        xbox_SetExitHandler(exit_via_launcher);
        int rc = xbox_HostWindowMain(title_boot);
        printf("xboxrecomp: title returned %d\n", rc);
        recomp_quit(rc);
    }

    SDL_AddEventWatch(lifecycle_watch, NULL);
    SDL_Window *win = SDL_CreateWindow("xboxrecomp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
                            : NULL;
    if (!ren) {
        fprintf(stderr, "window or renderer failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);
    printf("xboxrecomp ready (build %s, SDL %d.%d.%d, %dx%d)\n",
           XBA_VERSION, sdl.major, sdl.minor, sdl.patch, w, h);

    /* No title data: say where it goes instead of starting a title that
     * cannot open its files. It shows the message and stays on its black
     * screen (the app is restarted once the data is pushed). */
    if (console_game) {
        /* Another console's game: its runtime ran (or its pack is missing),
         * the lines above say which; no Xbox data to ask for. */
    } else if (have_game <= 0) {
        char msg[3 * RECOMP_STORAGE_PATH_MAX];
        const char *dir = have_game == 0 ? store.game_dir : "(no storage)";
        printf("NO_GAME_DATA: default.xbe not found in %s\n", dir);
        if (open_game_picker(dir) == 0) {
            printf("xboxrecomp: NO_GAME_DATA: game picker opened\n");
        } else {
            SDL_snprintf(msg, sizeof msg, TEXT_NO_GAME_DATA, dir, dir);
            if (SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "xboxrecomp", msg, win) != 0)
                fprintf(stderr, "message box failed: %s\n", SDL_GetError());
        }
    } else if (pack_rc == RECOMP_PACK_INVALID || pack_rc == RECOMP_PACK_DLOPEN) {
        /* A pack that cannot run: back to the launcher, which drops it and
         * offers Prepare again (its own, lighter check passed it). */
        char msg[2 * RECOMP_STORAGE_PATH_MAX];
        if (open_launcher_bad_pack(pack_reason) == 0) {
            printf("xboxrecomp: pack not usable, launcher opened\n");
        } else {
            SDL_snprintf(msg, sizeof msg, TEXT_BAD_PACK, pack_reason);
            if (SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "xboxrecomp", msg, win) != 0)
                fprintf(stderr, "message box failed: %s\n", SDL_GetError());
        }
    } else {
        char msg[2 * RECOMP_STORAGE_PATH_MAX];
        printf("xboxrecomp: game data: %s (no title linked)\n", store.xbe);
        /* NO_TITLE: the picker worked but this APK cannot run anything; a
         * bare black screen reads as a crash, so say it on screen. */
        SDL_snprintf(msg, sizeof msg, TEXT_NO_TITLE, store.game_dir);
        if (SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "xboxrecomp", msg, win) != 0)
            fprintf(stderr, "message box failed: %s\n", SDL_GetError());
    }

    /* RECOMP_SHELL_CRASH_TEST=1 (recomp.cfg): a deliberate null write outside
     * guest MMIO, to check on the device that the crash still reaches
     * debuggerd and gives a tombstone whose backtrace names this function. */
    if (getenv("RECOMP_SHELL_CRASH_TEST")) {
        /* No delay before the write any more: the fault handler drains the
         * log pump before it chains to debuggerd (XBA-52), so this line must
         * be in logcat above the tombstone. */
        printf("xboxrecomp: RECOMP_SHELL_CRASH_TEST: writing to NULL now\n");
        *(volatile int *)0 = 0x58424121;     /* "XBA!" */
    }

    Uint32 back_at = 0;
    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = 0;
            } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat &&
                       ev.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
                Uint32 now = SDL_GetTicks();
                if (back_at && now - back_at <= BACK_WINDOW_MS) {
                    printf("xboxrecomp: Back pressed twice, quitting\n");
                    running = 0;
                } else {
                    back_at = now;
                    printf("xboxrecomp: QUIT_PROMPT shown (press Back again to quit)\n");
                    SDL_AndroidShowToast(TEXT_QUIT_PROMPT, 0, -1, 0, 0);
                }
            }
        }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf("xboxrecomp: clean exit\n");
    return 0;
}
