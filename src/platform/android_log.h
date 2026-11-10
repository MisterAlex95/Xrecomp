/* Android: stdout and stderr into logcat.
 *
 * The runtime reports through fprintf(stderr, ...) (hundreds of call sites);
 * on Android both streams go to /dev/null unless something reads them. Call
 * xbox_android_log_init() first thing in SDL_main: from then on every line
 * written to either stream shows in logcat under the tag "xboxrecomp"
 * (stdout at INFO, stderr at WARN). A no-op on other hosts.
 *
 * Underneath is a log pump that every POSIX host has (the tests use it on
 * Linux): stdout and stderr become pipes, and one thread per pipe hands each
 * line to a sink. Lines still in a pipe when the process dies are lost, so
 * the two ways out drain it first (XBA-52): the quit path
 * (recomp_quit.h) and the fault handler, before it chains a real crash to
 * debuggerd (xbox_memory_layout.c). */
#ifndef XBOXRECOMP_ANDROID_LOG_H
#define XBOXRECOMP_ANDROID_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_ANDROID_LOG_TAG "xboxrecomp"

/* Starts the forwarding threads. Returns 0 on success (or on non-Android
 * hosts), -1 if a pipe or thread could not be created. Safe to call twice. */
int xbox_android_log_init(void);

/* One complete line (no '\n') from stdout (is_stderr 0) or stderr (1). Called
 * on the pump's threads, never in a signal handler. */
typedef void (*xbox_log_sink)(int is_stderr, const char *line);

/* The pump itself: stdout and stderr through pipes, one thread each, every
 * line to sink. Once per process (a second call returns the first one's
 * result and keeps its sink). 0 on success, -1 on failure. */
int xbox_log_pump_start(xbox_log_sink sink);

/* Waits, at most timeout_ms, until both pipes are empty and their threads
 * have handed what they read to the sink. A line still being assembled (no
 * '\n' yet) is ended with a '\n' so that it goes out too.
 *
 * Async-signal-safe, for the fault handler: it only reads and writes
 * lock-free atomics and makes plain system calls (ioctl FIONREAD,
 * clock_gettime, nanosleep, and a 1-byte write into an empty pipe, which
 * cannot block). It never logs by itself -- __android_log_write takes locks
 * and may allocate, and the faulting thread may hold them -- the pump threads
 * keep doing that, and this only waits for them. The cost of that choice: a
 * pump thread that is stuck (on a lock the crashing thread holds, say) is
 * not waited for past the timeout, and its lines are lost; a crash is never
 * held up for longer than the timeout. A pump thread that faults itself
 * does not wait for itself. Re-entry (a second fault, or two threads at
 * once) returns at once: the first caller is already draining.
 *
 * stdio buffers are not flushed here (fflush is not async-signal-safe):
 * stderr is unbuffered and stdout line-buffered, so only a partial stdout
 * line printed without '\n' can be left behind. Callers outside a signal
 * handler fflush first.
 *
 * Returns 1 when drained, 0 on timeout or re-entry, -1 when no pump runs
 * (non-Android hosts outside the tests): then it returns at once. */
int xbox_log_drain(int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_ANDROID_LOG_H */
