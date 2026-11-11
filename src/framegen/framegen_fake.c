/* framegen_fake.c -- the fake frame generator: a linear blend of the previous
 * and the new frame, on the CPU. For tests and headless runs
 * (RECOMP_LSFG_FAKE=1); RECOMP_LSFG_FAKE_MS=N adds N ms per generation. */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime, nanosleep, setenv */
#include "framegen_pacer.h"
#include "framegen_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Fake {
    uint8_t *prev;
    uint32_t w, h, multiplier;
    int have_prev;
    long delay_ms;
} Fake;

static Fake s_fake;

static int fake_open(void *user, uint32_t w, uint32_t h, uint32_t multiplier,
                     char *err, size_t errlen)
{
    Fake *f = (Fake *)user;
    f->prev = (uint8_t *)malloc((size_t)w * h * 4);
    if (!f->prev) {
        snprintf(err, errlen, "fake generator: out of memory");
        return -1;
    }
    f->w = w;
    f->h = h;
    f->multiplier = multiplier < 2 ? 2 : multiplier;
    f->have_prev = 0;
    f->delay_ms = framegen_policy_env_long("RECOMP_LSFG_FAKE_MS", 0, 0, 1000);
    return 0;
}

static void keep(Fake *f, const uint8_t *frame, uint32_t pitch)
{
    for (uint32_t y = 0; y < f->h; y++)
        memcpy(f->prev + (size_t)y * f->w * 4, frame + (size_t)y * pitch, (size_t)f->w * 4);
    f->have_prev = 1;
}

static int fake_generate(void *user, const uint8_t *frame, uint32_t pitch,
                         uint8_t *const *out, uint32_t out_pitch)
{
    Fake *f = (Fake *)user;
    int n = 0;
    if (!f->prev)
        return -1;
    if (f->delay_ms) {
        struct timespec ts = { f->delay_ms / 1000, (f->delay_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    if (out && f->have_prev) {
        uint32_t m = f->multiplier;
        for (uint32_t k = 1; k < m; k++) {
            for (uint32_t y = 0; y < f->h; y++) {
                const uint8_t *a = f->prev + (size_t)y * f->w * 4;
                const uint8_t *b = frame + (size_t)y * pitch;
                uint8_t *d = out[k - 1] + (size_t)y * out_pitch;
                for (uint32_t x = 0; x < f->w * 4; x++)
                    d[x] = (uint8_t)((a[x] * (m - k) + b[x] * k + m / 2) / m);
            }
        }
        n = (int)(m - 1);
    }
    keep(f, frame, pitch);
    return n;
}

static void fake_reset(void *user)
{
    ((Fake *)user)->have_prev = 0;
}

static void fake_close(void *user)
{
    Fake *f = (Fake *)user;
    free(f->prev);
    f->prev = NULL;
    f->have_prev = 0;
}

const FramegenGenerator framegen_fake_generator = {
    "fake", &s_fake, fake_open, fake_generate, fake_reset, fake_close,
};
