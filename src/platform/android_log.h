/* Android: stdout and stderr into logcat.
 *
 * The runtime reports through fprintf(stderr, ...) (hundreds of call sites);
 * on Android both streams go to /dev/null unless something reads them. Call
 * xbox_android_log_init() first thing in SDL_main: from then on every line
 * written to either stream shows in logcat under the tag "xboxrecomp"
 * (stdout at INFO, stderr at WARN). A no-op on other hosts. */
#ifndef XBOXRECOMP_ANDROID_LOG_H
#define XBOXRECOMP_ANDROID_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_ANDROID_LOG_TAG "xboxrecomp"

/* Starts the forwarding threads. Returns 0 on success (or on non-Android
 * hosts), -1 if a pipe or thread could not be created. Safe to call twice. */
int xbox_android_log_init(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_ANDROID_LOG_H */
