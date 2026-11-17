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
 * called from any thread.
 *
 * A pause has reasons (RECOMP_HOLD_*): the app in the background, the
 * Android in-game menu open (docs/android/app-shell.md « In-game menu »).
 * The guest stays held while any reason is set, so the menu open across a
 * trip to the background keeps the game paused when the app comes back.
 * Listeners see the first reason set and the last one cleared, not each. */
#ifndef XBOXRECOMP_RECOMP_LIFECYCLE_H
#define XBOXRECOMP_RECOMP_LIFECYCLE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*recomp_lifecycle_fn)(int paused, void *user);

#define RECOMP_HOLD_BACKGROUND 0x1u   /* recomp_lifecycle_pause / resume */
#define RECOMP_HOLD_MENU       0x2u   /* the Android in-game menu */
#define RECOMP_HOLD_ALL        0xFFFFFFFFu

/* Idempotent: a second pause (or resume) in a row does nothing and calls no
 * listener. Listeners are called on the calling thread, in registration
 * order on pause and in reverse order on resume. A listener must not call
 * pause or resume itself. pause / resume are the BACKGROUND reason. */
void recomp_lifecycle_pause(void);
void recomp_lifecycle_resume(void);
int  recomp_lifecycle_paused(void);

/* Sets / clears the reasons in mask. Paused while any reason is set:
 * listeners run when the first reason is set and when the last one is
 * cleared. resume_for(RECOMP_HOLD_ALL) lets the guest go whatever held it
 * (the window closing). */
void recomp_lifecycle_pause_for(unsigned reasons);
void recomp_lifecycle_resume_for(unsigned reasons);
unsigned recomp_lifecycle_reasons(void);

/* Lets the guest run although reasons are set, until the matching lift_end (an Xbox save state needs every guest
 * thread to reach a kernel call, which a thread waiting behind the held flip never does). The reasons are kept:
 * a pause_for or resume_for meanwhile changes them as usual, and at lift_end the guest is held again only when a
 * reason is still set. Listeners see a resume and a pause as for any other. Lifts nest. */
void recomp_lifecycle_lift_begin(void);
void recomp_lifecycle_lift_end(void);

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
