/* framegen_pacer.h -- frame generation pacer (LSFG WP2).
 *
 * The presenter hands every real frame to the pacer at its flip; the pacer's
 * thread asks a generator for the in-between frame(s) and hands real and
 * generated frames back to the presenter's sink, each with the host time it
 * is due on screen. C, libc + pthreads, no Vulkan: the generator is a vtable
 * (FramegenGenerator) so the LSFG bridge plugs in from outside and the tests
 * use a fake one.
 *
 * Pacing, for real frame N flipped at T (CLOCK_MONOTONIC ns), multiplier M:
 *  - P = moving average of the last 8 flip intervals, clamped to
 *    [cap period, 2 x cap period] (cap = framegen_policy_fps_cap()).
 *  - c = the generator's cost for this pair (measured).
 *  - generated frame k (1..M-1) of G(N-1, N) is due at T + c + (k-1) P / M,
 *    N at T + c + (M-1) P / M: for 2x, G at T + c and N at T + c + P/2.
 *  - interval T - T(N-1) beyond 2 x cap period (a stall, a loading screen):
 *    no generation for that pair, N due at T; the interval is not averaged.
 *  - drop: at hand-over, an entry more than P late is dropped if generated;
 *    a real one only if a newer real frame is already queued.
 *  - framegen_pacer_reset() (lifecycle pause/resume) forgets the history:
 *    the next frame only primes the generator.
 *  - fallback: 30 consecutive generations failed or over budget (budget =
 *    cap period / M, RECOMP_LSFG_BUDGET_MS overrides), or an open failure:
 *    log "[LSFG] off: <reason>" once and pass every frame through (due = T).
 *    framegen_pacer_active() then returns 0 and the presenter may go back to
 *    its own path.
 *
 * Pixels handed to the sink are always 32 bpp BGRX (byte order B, G, R, X),
 * packed (pitch = w * 4): RGBX, R5G6B5 and X1R5G5B5 input is converted at
 * submit (framegen_pacer_submit_format).
 */
#ifndef FRAMEGEN_PACER_H
#define FRAMEGEN_PACER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Generator vtable (the LSFG bridge implements it) ─────────────────────
 *
 * Every call comes from the pacer thread, one at a time.
 *
 * open(user, w, h, multiplier, err, errlen)
 *     Prepare for w x h frames, multiplier - 1 generated frames per pair.
 *     Return 0, or < 0 with a one-line reason in err (it becomes the
 *     "[LSFG] off: <reason>" line). Called again after close() when the frame
 *     size changes.
 * generate(user, frame, pitch, out, out_pitch)
 *     frame: the new real frame, BGRX 32 bpp, w x h, pitch bytes per row.
 *     The generator keeps the previous frame itself (its history).
 *     out == NULL: only push frame into the history, write nothing, return 0
 *       (first frame after open/reset, and pairs the pacer skips).
 *     out != NULL: out[0 .. multiplier-2] are BGRX buffers of out_pitch bytes
 *       per row; write G(prev, frame) at t = k / multiplier into out[k-1] and
 *       return the number written (multiplier - 1), or 0 when there was no
 *       previous frame yet (frame is then history), or < 0 on failure.
 *     Maps 1:1 onto lsfg_bridge_generate(frame_bgrx, pitch, out, out_pitch).
 * reset(user)  (optional, may be NULL)
 *     Forget the history (lifecycle pause/resume). NULL: close() + open().
 * close(user)
 *     Release everything open() made. Safe to call after a failed open().
 */
typedef struct FramegenGenerator {
    const char *name;
    void *user;
    int  (*open)(void *user, uint32_t width, uint32_t height, uint32_t multiplier,
                 char *err, size_t errlen);
    int  (*generate)(void *user, const uint8_t *frame_bgrx, uint32_t pitch,
                     uint8_t *const *out, uint32_t out_pitch);
    void (*reset)(void *user);
    void (*close)(void *user);
} FramegenGenerator;

/* The fake generator: linear blend of the two frames, no GPU. Selected by
 * framegen_pacer_start() under RECOMP_LSFG_FAKE=1 (headless runs); with
 * RECOMP_LSFG_FAKE_MS=N each generate() also sleeps N ms (budget tests). */
extern const FramegenGenerator framegen_fake_generator;

/* ── Sink (the presenter implements it) ───────────────────────────────────
 * show() is called from the pacer thread; px is valid only during the call
 * (copy it into a timed FIFO and return). due_ns: CLOCK_MONOTONIC. */
typedef struct FramegenSink {
    void (*show)(const uint8_t *px, uint32_t w, uint32_t h, uint32_t bpp, uint64_t due_ns);
} FramegenSink;

/* ── Pacer ──────────────────────────────────────────────────────────────── */

/* The generator framegen_pacer_start() uses when RECOMP_LSFG_FAKE is unset
 * (the LSFG bridge registers itself here). Call before start. */
void framegen_pacer_set_generator(const FramegenGenerator *gen);

/* Reads RECOMP_LSFG (must be on), RECOMP_LSFG_MULT, RECOMP_LSFG_FAKE,
 * RECOMP_LSFG_BUDGET_MS, RECOMP_GPU_FPS_CAP / RECOMP_LSFG_DISPLAY_HZ (through
 * framegen_policy_fps_cap) and starts the pacer thread. 0 = started; < 0 =
 * framegen off (not requested, or no generator: "[LSFG] off: ..." logged). */
int  framegen_pacer_start(const FramegenSink *sink);

/* px: the frame as flipped, w x h, bpp 2 (R5G6B5) or 4 (BGRX), pitch bytes
 * per row; copied before return. flip_ns: CLOCK_MONOTONIC time of the flip. */
void framegen_pacer_submit(const uint8_t *px, uint32_t w, uint32_t h, uint32_t bpp,
                           uint32_t pitch, uint64_t flip_ns);

/* Input pixel formats (framegen_pacer_submit_format): byte order in memory,
 * X ignored. BGRX = X8R8G8B8 little-endian words (the guest's 32 bpp surface,
 * desktop GL GL_BGRA); RGBX = GLES GL_RGBA read-backs; the 16 bpp ones are
 * little-endian words. Everything is converted to BGRX at submit. */
enum {
    FRAMEGEN_FMT_BGRX8888 = 1,
    FRAMEGEN_FMT_RGBX8888 = 2,
    FRAMEGEN_FMT_R5G6B5   = 3,
    FRAMEGEN_FMT_X1R5G5B5 = 4,
};

/* framegen_pacer_submit with an explicit format (FRAMEGEN_FMT_*); frames up
 * to 8192 x 8192. */
void framegen_pacer_submit_format(const uint8_t *px, uint32_t w, uint32_t h, uint32_t format,
                                  uint32_t pitch, uint64_t flip_ns);

/* Latched about once a second: real frames per second x 100, frames shown
 * per second x 100, mean generator cost in us. Any pointer may be NULL. */
void framegen_pacer_stats(uint32_t *real_fps_centi, uint32_t *shown_fps_centi, uint32_t *gen_us);

/* Lifecycle pause/resume: drop queued frames, forget the timing history and
 * the generator's history. */
void framegen_pacer_reset(void);

/* 1 while generating, 0 once fallen back to pass-through or stopped. */
int  framegen_pacer_active(void);

/* The fallback reason ("" while active). */
const char *framegen_pacer_off_reason(void);

void framegen_pacer_stop(void);

/* ── Explicit configuration (tests, and presenters that want their own) ─── */
typedef struct FramegenPacerConfig {
    const FramegenGenerator *gen;   /* required */
    uint32_t multiplier;            /* 2..4 */
    uint32_t cap_fps;               /* effective cap (> 0) */
    uint32_t budget_us;             /* 0: cap period / multiplier */
    uint32_t fallback_frames;       /* 0: 30 */
    int threaded;                   /* 0: submit() processes inline (tests) */
    uint64_t (*now_ns)(void *user); /* NULL: CLOCK_MONOTONIC */
    void *clock_user;
} FramegenPacerConfig;

int framegen_pacer_start_ex(const FramegenSink *sink, const FramegenPacerConfig *cfg);

typedef struct FramegenPacerCounters {
    uint64_t submitted;        /* real frames handed in */
    uint64_t dropped_input;    /* replaced in the queue before processing */
    uint64_t generated;        /* generated frames produced */
    uint64_t shown_real, shown_generated;
    uint64_t dropped_real, dropped_generated;   /* the drop rule */
    uint64_t skipped;          /* pairs not generated: gap beyond 2 x cap period */
    uint64_t failed;           /* generate() < 0 */
    uint64_t over_budget;
    uint64_t resets;
    uint32_t period_ns;        /* current P */
    uint32_t last_cost_ns;     /* c of the last generation */
} FramegenPacerCounters;

void framegen_pacer_counters(FramegenPacerCounters *out);

#ifdef __cplusplus
}
#endif

#endif /* FRAMEGEN_PACER_H */
