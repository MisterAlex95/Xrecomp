/* apu_volume.h. */
#include "apu_volume.h"

static volatile int s_volume = 100;
static volatile int s_ff_mute;   /* XBA-240: fast-forward runs */

void xbox_ApuSetOutputVolume(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
#if defined(_MSC_VER)
    s_volume = percent;
#else
    __atomic_store_n(&s_volume, percent, __ATOMIC_RELAXED);
#endif
}

int xbox_ApuGetOutputVolume(void)
{
#if defined(_MSC_VER)
    return s_volume;
#else
    return __atomic_load_n(&s_volume, __ATOMIC_RELAXED);
#endif
}

void xbox_ApuSetFastForwardMute(int on)
{
#if defined(_MSC_VER)
    s_ff_mute = on != 0;
#else
    __atomic_store_n(&s_ff_mute, on != 0, __ATOMIC_RELAXED);
#endif
}

int xbox_ApuFastForwardMuted(void)
{
#if defined(_MSC_VER)
    return s_ff_mute;
#else
    return __atomic_load_n(&s_ff_mute, __ATOMIC_RELAXED);
#endif
}

void xbox_ApuApplyOutputVolume(int16_t *samples, int count)
{
    int v = xbox_ApuGetOutputVolume(), i;
    int32_t gain;

    if (xbox_ApuFastForwardMuted())
        v = 0;
    if (v >= 100 || !samples)
        return;
    /* Q15: (v / 100)^2. At most 32767 * 32767 >> 15, so no overflow. */
    gain = (int32_t)((int64_t)v * v * 32768 / 10000);
    for (i = 0; i < count; i++)
        samples[i] = (int16_t)(((int32_t)samples[i] * gain) >> 15);
}
