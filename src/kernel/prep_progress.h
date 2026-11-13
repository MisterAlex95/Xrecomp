/* "Preparing data": a title filling its utility drive (Z:) (WP7).
 *
 * Several titles unpack or build a cache on Z: the first time they run, while
 * the front end already shows (Fable decompresses ~550 MB of d:\Data\*.biz to
 * z:\data\*.big), and on a slow host that takes minutes with nothing on the
 * screen saying why the game is slow. The runtime cannot know the total (it
 * is the title's own format), but it sees every byte the title writes to Z:,
 * so it can say that data is being prepared and how much has been written.
 *
 * A burst: writes to Z: with no gap longer than PREP_IDLE_MS. It is shown
 * once it holds PREP_SHOW_BYTES (a save or a small settings file never
 * shows), and ends PREP_IDLE_MS after the last write. The window title (and
 * the performance overlay, on a phone with no title bar) then carries
 * "preparing data <n> MB", and the log says when it starts, every
 * PREP_LOG_STEP bytes, and how much it wrote in how long when it ends.
 *
 * Pure and header-only, so a test drives it with numbers; prep_progress.c
 * feeds it the writes (kernel_file.c's NtWriteFile) and the clock. */
#ifndef XBOXRECOMP_PREP_PROGRESS_H
#define XBOXRECOMP_PREP_PROGRESS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PREP_SHOW_BYTES (8ull << 20)    /* a burst shows from 8 MB */
#define PREP_IDLE_MS    5000u           /* and ends 5 s after its last write */
#define PREP_LOG_STEP   (64ull << 20)   /* a log line every 64 MB */

typedef struct {
    uint64_t bytes;                     /* written in the current burst */
    uint64_t t_first, t_last;           /* its first and last write */
    uint64_t logged;                    /* bytes at the last progress line */
    int shown;                          /* the burst passed PREP_SHOW_BYTES */
} PrepProgress;

/* What a call reports: an event to log, if any. */
enum { PREP_EV_NONE, PREP_EV_START, PREP_EV_STEP, PREP_EV_END };

/* A burst that went quiet: ends it. Returns PREP_EV_END when a shown one
 * ended (*bytes, *ms: what it wrote, in how long), else PREP_EV_NONE. */
static inline int prep_progress_expire(PrepProgress *p, uint64_t now_ms, uint64_t *bytes,
                                       uint64_t *ms)
{
    int ev = PREP_EV_NONE;
    if (!p->bytes || now_ms - p->t_last < PREP_IDLE_MS)
        return ev;
    if (p->shown) {
        ev = PREP_EV_END;
        if (bytes)
            *bytes = p->bytes;
        if (ms)
            *ms = p->t_last - p->t_first;
    }
    p->bytes = p->logged = 0;
    p->shown = 0;
    return ev;
}

/* n bytes written to Z: at now_ms. Returns PREP_EV_START when the burst
 * becomes shown, PREP_EV_STEP when it passed another PREP_LOG_STEP, or
 * PREP_EV_END when this write comes after a shown burst ended unseen (no
 * poll in between: *bytes, *ms describe that one, and this write starts the
 * next). */
static inline int prep_progress_write(PrepProgress *p, uint64_t now_ms, uint64_t n,
                                      uint64_t *bytes, uint64_t *ms)
{
    int ev = prep_progress_expire(p, now_ms, bytes, ms);
    if (!n)
        return ev;
    if (!p->bytes)
        p->t_first = now_ms;
    p->bytes += n;
    p->t_last = now_ms;
    if (ev != PREP_EV_NONE)
        return ev;
    if (!p->shown && p->bytes >= PREP_SHOW_BYTES) {
        p->shown = 1;
        p->logged = p->bytes;
        return PREP_EV_START;
    }
    if (p->shown && p->bytes - p->logged >= PREP_LOG_STEP) {
        p->logged = p->bytes - (p->bytes - p->logged) % PREP_LOG_STEP;
        return PREP_EV_STEP;
    }
    return PREP_EV_NONE;
}

/* The status text while a shown burst runs ("preparing data 123 MB"):
 * returns 1 and fills buf, else 0 (buf empty). Does not end a burst; call
 * prep_progress_expire first. */
static inline int prep_progress_text(const PrepProgress *p, char *buf, size_t n)
{
    if (!n)
        return 0;
    buf[0] = 0;
    if (!p->shown || !p->bytes)
        return 0;
    snprintf(buf, n, "preparing data %llu MB", (unsigned long long)(p->bytes >> 20));
    return 1;
}

#ifdef __cplusplus
extern "C" {
#endif

/* prep_progress.c. A write of n bytes to the host file host_path: counted
 * when it lies under the utility drive's folder (xbox_path_utility_dir_utf8).
 * RECOMP_PREP_INDICATOR=0 turns the whole indicator off. */
void xbox_UtilityWriteNote(const char *host_path, size_t n);

/* The status text now (see prep_progress_text): 1 while data is being
 * prepared. Also ends a quiet burst and logs it. Any thread. */
int recomp_prep_status(char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
