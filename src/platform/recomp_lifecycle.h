/* App lifecycle: hold the guest while the app is in the background (XBA-24).
 *
 * On Android SDL blocks the thread that pumps its events while the app is in
 * the background (and pauses its audio there), but nothing stops the guest's
 * own threads: the title keeps running its logic and burns the battery with
 * no one looking. The window's event watch calls recomp_lifecycle_pause()
 * on SDL_APP_WILLENTERBACKGROUND and recomp_lifecycle_resume() on
 * SDL_APP_DIDENTERFOREGROUND; the flip calls recomp_lifecycle_hold(), which
 * parks the presenting thread until resume. The title's other threads then
 * stop on their own: they wait for the GPU, which waits at the flip.
 *
 * Listeners run on pause and on resume (the audio device pauses there,
 * without waiting for the next flip). POSIX hosts; every function may be
 * called from any thread. */
#ifndef XBOXRECOMP_RECOMP_LIFECYCLE_H
#define XBOXRECOMP_RECOMP_LIFECYCLE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*recomp_lifecycle_fn)(int paused, void *user);

/* Idempotent: a second pause (or resume) in a row does nothing and calls no
 * listener. Listeners are called on the calling thread, in registration
 * order on pause and in reverse order on resume. A listener must not call
 * pause or resume itself. */
void recomp_lifecycle_pause(void);
void recomp_lifecycle_resume(void);
int  recomp_lifecycle_paused(void);

/* Blocks while paused, without spinning. Returns at once (0) when not
 * paused, 1 after having waited. */
int  recomp_lifecycle_hold(void);

/* Up to 8 listeners; returns 0, or -1 when full. A listener registered
 * while paused is called with paused=1 at once. */
int  recomp_lifecycle_listen(recomp_lifecycle_fn fn, void *user);

/* Number of pauses since start (for logs and tests). */
unsigned recomp_lifecycle_pauses(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_LIFECYCLE_H */
