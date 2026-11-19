/* A console runtime's frame loop and its worker threads as one performance hint session.
 *
 * Android's ADPF (APerformanceHint, API 33+) lets an app tell the CPU governor how long a frame may take (the
 * target) and how long each one really took (the work, not the sleep after it); the governor then raises or lowers
 * the clocks of the cores those threads run on to meet the target, rather than guessing from load. The runtime's
 * emulation thread opens the session (recomp_perf_hint_begin), reports each frame's work time
 * (recomp_perf_hint_frame) and closes it when the game ends; the threads it waits on each frame (the N64 RDP's
 * scanline bands, the PS1 GPU's shadow-pass workers) join it as they start (recomp_host_worker_hook).
 *
 * The API is looked up in libandroid.so at run time (dlopen), so the APK still runs on Android 12 and earlier,
 * where everything here does nothing; elsewhere (Linux, macOS) it does nothing as well. Only the time spent working
 * is reported, never the sleep to the next frame: the hint never asks for more clock than the frames need. */
#ifndef XBOXRECOMP_HOST_PERF_HINT_H
#define XBOXRECOMP_HOST_PERF_HINT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The calling thread runs the frame loop: it is part of the session, which opens at the first frame with a frame
 * period of target_ns. On Android the thread's priority is also raised to that of a display thread
 * (nice -8, THREAD_PRIORITY_URGENT_DISPLAY). Returns 1 when the hint is available, 0 when it is not (the calls
 * below then do nothing). */
int recomp_perf_hint_begin(int64_t target_ns);

/* One frame done: work_ns is the time it took, from the top of the loop to just before the sleep to the next
 * frame; target_ns the period it had (the session's target follows it when it moves by more than 1%). */
void recomp_perf_hint_frame(int64_t work_ns, int64_t target_ns);

/* The frame loop has ended: the session closes (worker threads still registered stay so for the next one). */
void recomp_perf_hint_end(void);

/* The calling thread joins / leaves the session (and the next ones while it lives). */
void recomp_perf_hint_thread_join(void);
void recomp_perf_hint_thread_leave(void);

/* The calling thread joins in the place of the one *slot names (0: none), which leaves; *slot then names the calling
 * thread. For a role one thread holds at a time but that may move to a new thread: the Xbox's guest main thread,
 * started again on a save state's load while the old one is parked or gone. */
void recomp_perf_hint_thread_take(int32_t *slot);

/* A frame loop whose frames are not timed from inside (the Xbox: one frame per flip, the executor's sleep to the
 * frame cap the only wait it knows): each frame's work from the time between two frames, less what the loop slept
 * meanwhile. Call at every frame with the monotonic time and the loop's sleep so far (a running total); returns the
 * work in ns, or 0 when there is none to report: the first frame, or a gap longer than gap_ns (a load, a pause, the
 * app in the background), which restarts the count. Pure: `w` holds the state (zeroed to start). */
typedef struct recomp_perf_hint_work {
    uint64_t last_ns, last_slept_ns;
} recomp_perf_hint_work;
int64_t recomp_perf_hint_work_at(recomp_perf_hint_work *w, uint64_t now_ns, uint64_t slept_total_ns,
                                 uint64_t gap_ns);

/* For a runtime device's thread-start hook (n64_rdp_set_thread_hook, ps1_gpu_set_thread_hook): starting != 0 when a
 * worker begins, 0 as it ends. A beginning worker is noted by host_cpus (its fast-core mask, put back after a
 * resume: recomp_host_cpus_track) and joins the session; an ending one leaves both. `user` is unused. */
void recomp_host_worker_hook(int starting, void *user);

/* Tests: the ADPF entry points as function pointers, in place of libandroid's (NULL: back to them). Each takes and
 * returns what the NDK's APerformanceHint_* of the same name does; set_threads may be NULL (API 33 has none: the
 * session is then opened again when its threads change). */
typedef struct recomp_perf_hint_api {
    void *(*get_manager)(void);
    void *(*create_session)(void *manager, const int32_t *tids, size_t n, int64_t target_ns);
    int (*update_target)(void *session, int64_t target_ns);
    int (*report_actual)(void *session, int64_t work_ns);
    void (*close_session)(void *session);
    int (*set_threads)(void *session, const int32_t *tids, size_t n);
} recomp_perf_hint_api;
void recomp_perf_hint_set_api(const recomp_perf_hint_api *api);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_HOST_PERF_HINT_H */
