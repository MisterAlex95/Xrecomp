/* How a POSIX host ends the run when the player quits (XBA-52).
 *
 * The window owns the main thread and lifted code cannot be stopped
 * mid-flight, so quitting ends the process with the guest's threads still
 * running. exit() would then run atexit handlers and destructors under those
 * threads. On Android recomp_quit() instead:
 *
 *   1. runs the exit hook (recomp_exit_hook.h: the kernel's missing-file
 *      summary), the one exit-time report that matters on a device;
 *   2. prints "clean exit", flushes stdout and stderr;
 *   3. drains the log pump (android_log.h), at most RECOMP_QUIT_DRAIN_MS, so
 *      those lines reach logcat;
 *   4. _exit(code): no atexit handlers, no destructors, no tombstone.
 *
 * The other atexit reports (RECOMP_PROFILE, the ICALL feedback dump, a WAV
 * capture's header) are desktop diagnostics and are skipped on Android.
 * On other hosts recomp_quit(code) is exit(code), as before. */
#ifndef XBOXRECOMP_RECOMP_QUIT_H
#define XBOXRECOMP_RECOMP_QUIT_H

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_QUIT_DRAIN_MS 500

__attribute__((noreturn)) void recomp_quit(int code);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_QUIT_H */
