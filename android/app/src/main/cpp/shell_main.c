/* The app shell's SDL_main (XBA-18): what every title's APK starts from.
 *
 * - stdout / stderr go to logcat (tag "xboxrecomp") before anything prints;
 * - RECOMP_* options from <external files dir>/recomp.cfg, before any library
 *   starts (XBA-26);
 * - the game data in <external files dir>/game/ (or RECOMP_GAME_DIR), the
 *   saves in <internal files dir>/save/, handed to xbox_path_init; a message
 *   (NO_GAME_DATA) when default.xbe is not there (XBA-23);
 * - the runtime on the fast cores of a big.LITTLE SoC, before any thread is
 *   created, so every thread inherits it (XBA-25);
 * - the trapped-MMIO fault handler, chained to debuggerd / ART so real
 *   crashes keep their tombstone, checked again after SDL_Init (XBA-22);
 * - a black, fullscreen, landscape window;
 * - "xboxrecomp ready" with the build version;
 * - Back twice within 2 s quits: SDL_main returns and SDLActivity finishes.
 *
 * A title replaces this file with its own entry (the template's WinMain path)
 * once the generated C is built for Android (XBA-33). */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform/android_log.h"
#include "platform/recomp_cfg.h"
#include "platform/host_cpus.h"
#include "platform/recomp_storage.h"
#include "kernel/xbox_memory_layout.h"

#ifndef XBA_VERSION
#define XBA_VERSION "dev"
#endif

#define BACK_WINDOW_MS 2000

/* kernel/kernel.h declares it, among much that the shell has no use for. */
extern void xbox_path_init(const char *game_dir, const char *save_dir);

/* User-facing text, named by the key the spec and the tests use. English
 * only until the app has a strings table of its own. */
#define TEXT_NO_GAME_DATA \
    "No game data found.\n\n" \
    "Copy the extracted disc (default.xbe and its files) to:\n%s\n\n" \
    "for example: adb push <disc folder>/. %s/"

/* The four symbols a title provides (its generated dispatch and one title's
 * front-end hooks). libmain links the whole toolkit, so that loading it on a
 * device proves bionic resolves every symbol the runtime uses; without a
 * title these stand in. Weak: a title's own definitions win. */
typedef void (*recomp_func_t)(void);
__attribute__((weak)) recomp_func_t recomp_lookup(unsigned int va)        { (void)va; return 0; }
__attribute__((weak)) recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }
__attribute__((weak)) int g_fe_cursor;
__attribute__((weak)) int fe_menu_get_pb_state(void) { return 0; }

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    if (xbox_android_log_init() != 0)
        SDL_Log("xboxrecomp: stdout/stderr redirect failed, runtime messages will be lost");

    /* RECOMP_* switches: no shell environment on Android, so a file in the
     * app's external files dir (adb push recomp.cfg /sdcard/Android/data/<id>/files/). */
    const char *ext = SDL_AndroidGetExternalStoragePath();
    if (ext) {
        char cfg[1024];
        SDL_snprintf(cfg, sizeof cfg, "%s/recomp.cfg", ext);
        if (recomp_cfg_load(cfg) == 0)
            printf("recomp.cfg: none (read from %s when present)\n", cfg);
    }

    /* Game data and saves, after recomp.cfg (which may set RECOMP_GAME_DIR)
     * and before any title code opens a file. */
    recomp_storage store;
    int have_game = recomp_storage_resolve(ext, SDL_AndroidGetInternalStoragePath(), &store);
    if (have_game >= 0)
        xbox_path_init(store.game_dir, store.save_dir);
    else
        fprintf(stderr, "xboxrecomp: no usable storage paths (external %s)\n", ext ? ext : "none");

    /* After recomp.cfg, which may say RECOMP_BIG_CORES=0; before SDL_Init,
     * whose threads should inherit the mask too. */
    recomp_host_cpus_init();

    if (xbox_PosixInstallMmioFaultHandler() != 0)
        fprintf(stderr, "xboxrecomp: could not install the MMIO fault handler\n");

    /* Back is ours: the shell decides when it quits (twice within 2 s). */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("xboxrecomp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
                            : NULL;
    if (!ren) {
        fprintf(stderr, "window or renderer failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* SDL may have installed handlers of its own: make sure ours is still
     * in front (it chains to theirs). */
    xbox_PosixInstallMmioFaultHandler();

    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);
    SDL_version sdl;
    SDL_GetVersion(&sdl);
    printf("xboxrecomp ready (build %s, SDL %d.%d.%d, %dx%d)\n",
           XBA_VERSION, sdl.major, sdl.minor, sdl.patch, w, h);

    /* No title data: say where it goes instead of starting a title that
     * cannot open its files. The shell carries no title yet, so for now it
     * shows the message and stays on its black screen. */
    if (have_game <= 0) {
        char msg[3 * RECOMP_STORAGE_PATH_MAX];
        const char *dir = have_game == 0 ? store.game_dir : "(no storage)";
        SDL_snprintf(msg, sizeof msg, TEXT_NO_GAME_DATA, dir, dir);
        printf("NO_GAME_DATA: default.xbe not found in %s\n", dir);
        if (SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "xboxrecomp", msg, win) != 0)
            fprintf(stderr, "message box failed: %s\n", SDL_GetError());
    } else {
        printf("xboxrecomp: game data: %s\n", store.xbe);
    }

    /* RECOMP_SHELL_CRASH_TEST=1 (recomp.cfg): a deliberate null write outside
     * guest MMIO, to check on the device that the crash still reaches
     * debuggerd and gives a tombstone whose backtrace names this function. */
    if (getenv("RECOMP_SHELL_CRASH_TEST")) {
        printf("xboxrecomp: RECOMP_SHELL_CRASH_TEST: writing to NULL now\n");
        fflush(stdout);
        SDL_Delay(200);                      /* let the line reach logcat */
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
                    printf("xboxrecomp: press Back again to quit\n");
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
