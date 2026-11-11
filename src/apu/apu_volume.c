/* apu_volume.h. */
#include "apu_volume.h"

static volatile int s_volume = 100;

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

void xbox_ApuApplyOutputVolume(int16_t *samples, int count)
{
    int v = xbox_ApuGetOutputVolume(), i;
    int32_t gain;

    if (v >= 100 || !samples)
        return;
    /* Q15: (v / 100)^2. At most 32767 * 32767 >> 15, so no overflow. */
    gain = (int32_t)((int64_t)v * v * 32768 / 10000);
    for (i = 0; i < count; i++)
        samples[i] = (int16_t)(((int32_t)samples[i] * gain) >> 15);
}
