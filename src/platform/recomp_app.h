/* The start-up of a title on a host without a shell (Android), in one call
 * (XBA-52).
 *
 * The app shell's SDL_main (android/app/src/main/cpp/shell_main.c) calls
 * recomp_app_start() before anything else, then hands the result to the
 * title's hook:
 *
 *     int recomp_title_main(const recomp_storage *store);
 *
 * which a title defines (templates/new-game/src/main.c does, when built
 * through android/app with XBA_TITLE_DIR) and the shell calls on the guest
 * thread of xbox_HostWindowMain once default.xbe is found. So every title
 * gets the same boot path -- logcat, recomp.cfg, storage, fast cores, the
 * fault handler, the window and the lifecycle -- without a copy of the
 * shell. See docs/android/app-shell.md.
 *
 * POSIX hosts. Links the kernel (xbox_path_init, the fault handler), so it
 * is a library of its own (recomp_app), not part of platform. */
#ifndef XBOXRECOMP_RECOMP_APP_H
#define XBOXRECOMP_RECOMP_APP_H

#include "recomp_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In order:
 *   1. stdout / stderr to logcat (xbox_android_log_init; no-op elsewhere);
 *   2. RECOMP_* options from <external>/recomp.cfg (recomp_cfg.h), when
 *      external is not NULL;
 *   3. the host defaults (recomp_app_set_defaults, below);
 *   4. the game and save directories (recomp_storage_resolve) handed to
 *      xbox_path_init, when both paths are given;
 *   5. the runtime on the fast cores (recomp_host_cpus_init), before any
 *      other thread exists;
 *   6. the trapped-MMIO fault handler (xbox_PosixInstallMmioFaultHandler).
 *      Call that one again after SDL_Init, which may install its own.
 *
 * store is filled (all "" when storage could not be resolved). Returns what
 * recomp_storage_resolve returned: 1 when default.xbe was found, 0 when it
 * was not, -1 when there was no usable storage (or no paths were given). */
int recomp_app_start(const char *external, const char *internal,
                     recomp_storage *store);

/* The host's defaults, for variables neither the environment nor recomp.cfg
 * set. On Android (android != 0):
 *   RECOMP_FB_WINDOW=1  the window and its event loop (xbox_HostWindowMain:
 *                       pause, Back, a 64 MB guest stack);
 *   RECOMP_PB_EXEC=1    the pushbuffer executor, whose flips are what the
 *                       window shows.
 * RECOMP_HEADLESS=1 still keeps the window off. RECOMP_PB_EXEC is read by
 * presence elsewhere in the runtime, so here "=0" (or empty) removes it from
 * the environment: RECOMP_PB_EXEC=0 in recomp.cfg turns the executor off.
 * Elsewhere (android == 0) nothing changes. Prints one line when it set
 * anything. */
void recomp_app_set_defaults(int android);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_APP_H */
