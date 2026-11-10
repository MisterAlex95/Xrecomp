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
 *   window, Back twice to quit (flush, drain logcat, _exit(0): « clean
 *   exit ») and the lifecycle;
 * - otherwise the shell's own black, fullscreen, landscape window. With
 *   default.xbe found but no title linked, a message box says so (NO_TITLE)
 *   instead of a bare black screen. When
 *   default.xbe is not there (NO_GAME_DATA) it hands over to the game picker
 *   (ShellActivity.openGamePicker, XBA-54), which names the folder and lets
 *   the player choose the game; a message box if that call fails. Back once shows
 *   QUIT_PROMPT (a toast), twice within 2 s quits: SDL_main returns and
 *   SDLActivity finishes; "lifecycle: paused" / "resumed after N ms" when
 *   the app leaves and comes back (XBA-24).
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

/* The four symbols a title provides (its generated dispatch and one title's
 * front-end hooks). libmain links the whole toolkit, so that loading it on a
 * device proves bionic resolves every symbol the runtime uses; without a
 * title these stand in. Weak: a title's own definitions win. */
typedef void (*recomp_func_t)(void);
__attribute__((weak)) recomp_func_t recomp_lookup(unsigned int va)        { (void)va; return 0; }
__attribute__((weak)) recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }
__attribute__((weak)) int g_fe_cursor;
__attribute__((weak)) int fe_menu_get_pb_state(void) { return 0; }

/* The title's boot (recomp_app.h). Weak and undefined without a title: the
 * address is then NULL and the shell runs its own window. */
extern int recomp_title_main(const recomp_storage *store) __attribute__((weak));

static const recomp_storage *s_title_store;

/* xbox_HostWindowMain's guest_main, on the guest thread. */
static int title_boot(void)
{
    return recomp_title_main(s_title_store);
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

/* NO_GAME_DATA: ShellActivity.openGamePicker(dir) (XBA-54) starts the game
 * picker naming the folder and finishes this activity, so SDL_QUIT follows.
 * Called on SDLThread, a Java thread: SDL_AndroidGetJNIEnv is its env. The
 * method is looked up on the activity's own class, not with FindClass. */
static int open_game_picker(const char *dir)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = env ? (jobject)SDL_AndroidGetActivity() : NULL;
    int rc = -1;

    if (!act)
        return -1;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetMethodID(env, cls, "openGamePicker", "(Ljava/lang/String;)V") : NULL;
    jstring jdir = m ? (*env)->NewStringUTF(env, dir) : NULL;
    if (jdir) {
        (*env)->CallVoidMethod(env, act, m, jdir);
        rc = 0;
        (*env)->DeleteLocalRef(env, jdir);
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
    /* Start-up steps, shared with every title (recomp_app.h). */
    recomp_storage store;
    int have_game = recomp_app_start(SDL_AndroidGetExternalStoragePath(),
                                     SDL_AndroidGetInternalStoragePath(), &store);
    int run_title = have_game > 0 && recomp_title_main != NULL;

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
    if (have_game <= 0) {
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
