/* framegen_policy.h -- what frame generation changes outside the pacer.
 *
 * Header only, libc only, so the executor (src/kernel/nv2a_pb_exec.c) and the
 * GL back end (src/gpu/nv2a_gl.c) can apply the rules without linking the
 * framegen library. Both include it only when the build has XBOXRECOMP_LSFG
 * (the root CMakeLists defines it on xbox_kernel and xbox_gpu); at run time
 * the rules apply only under RECOMP_LSFG=1.
 *
 *  - Frame cap: while framegen is on, the effective RECOMP_GPU_FPS_CAP is
 *    min(cap, display rate / multiplier): 30 for a 60 Hz display at 2x. A cap
 *    of 0 (off) becomes display rate / multiplier. The display rate is
 *    RECOMP_LSFG_DISPLAY_HZ (default 60) until the presenter reports the real
 *    one (WP3).
 *  - Present mode: RECOMP_GPU_PRESENT=sync is forced, since the async path
 *    delivers frame N at flip N+1 and the pacer times frames by their flip.
 *
 * Both are read once at start-up by their callers; a pacer that falls back
 * to pass-through later keeps the cap and the sync read-back (harmless).
 */
#ifndef FRAMEGEN_POLICY_H
#define FRAMEGEN_POLICY_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int framegen_policy_env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0") != 0;
}

static inline long framegen_policy_env_long(const char *name, long def, long lo, long hi)
{
    const char *e = getenv(name);
    long v = def;
    if (e && *e)
        v = strtol(e, NULL, 10);
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}

/* RECOMP_LSFG=1: frame generation requested. */
static inline int framegen_policy_enabled(void)
{
    return framegen_policy_env_on("RECOMP_LSFG");
}

/* RECOMP_LSFG_MULT: shown frames per real frame (2 = one generated frame). */
static inline uint32_t framegen_policy_multiplier(void)
{
    return (uint32_t)framegen_policy_env_long("RECOMP_LSFG_MULT", 2, 2, 4);
}

/* RECOMP_LSFG_DISPLAY_HZ: the display's refresh rate. */
static inline uint32_t framegen_policy_display_hz(void)
{
    return (uint32_t)framegen_policy_env_long("RECOMP_LSFG_DISPLAY_HZ", 60, 24, 480);
}

/* The cap rule, pure: cap <= 0 means "no cap". Never returns less than 1. */
static inline int framegen_policy_cap_rule(int cap, uint32_t display_hz, uint32_t multiplier)
{
    int limit = multiplier ? (int)(display_hz / multiplier) : (int)display_hz;
    if (limit < 1)
        limit = 1;
    return (cap <= 0 || cap > limit) ? limit : cap;
}

/* RECOMP_GPU_FPS_CAP as the executor reads it (same platform default: 60 on
 * macOS and Android, off elsewhere; keep in step with gpu_fps_cap()). */
static inline int framegen_policy_env_fps_cap(void)
{
#if defined(__APPLE__) || defined(__ANDROID__)
    long def = 60;
#else
    long def = 0;
#endif
    return (int)framegen_policy_env_long("RECOMP_GPU_FPS_CAP", def, 0, 1000);
}

/* The executor's hook: the cap it computed, adjusted while framegen is on. */
static inline int framegen_policy_fps_cap(int cap)
{
    if (!framegen_policy_enabled())
        return cap;
    return framegen_policy_cap_rule(cap, framegen_policy_display_hz(),
                                    framegen_policy_multiplier());
}

/* The GL back end's hook: 1 when the presented surface must be read back at
 * its own flip (RECOMP_GPU_PRESENT=sync). */
static inline int framegen_policy_force_present_sync(void)
{
    return framegen_policy_enabled();
}

#endif /* FRAMEGEN_POLICY_H */
