/* apu_volume.h: the output volume the Android in-game menu sets. 100 leaves
 * the mix untouched, 0 is silence, the steps between are the square of the
 * fraction, the extremes do not overflow, and the setting is clamped. */
#include "apu_volume.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    int16_t s[6], ref[6] = { 0, 1000, -1000, 32767, -32768, 12345 };

    /* Default: full volume, samples untouched. */
    CHECK(xbox_ApuGetOutputVolume() == 100);
    memcpy(s, ref, sizeof s);
    xbox_ApuApplyOutputVolume(s, 6);
    CHECK(memcmp(s, ref, sizeof s) == 0);

    /* Mute. */
    xbox_ApuSetOutputVolume(0);
    memcpy(s, ref, sizeof s);
    xbox_ApuApplyOutputVolume(s, 6);
    for (int i = 0; i < 6; i++)
        CHECK(s[i] == 0);

    /* 50 %: a quarter of the amplitude (-12 dB). */
    xbox_ApuSetOutputVolume(50);
    memcpy(s, ref, sizeof s);
    xbox_ApuApplyOutputVolume(s, 6);
    CHECK(s[1] == 250);
    CHECK(s[2] == -250);
    CHECK(s[3] == 8191);
    CHECK(s[4] == -8192);

    /* 99 %: just under, never above, no wrap at the extremes. */
    xbox_ApuSetOutputVolume(99);
    memcpy(s, ref, sizeof s);
    xbox_ApuApplyOutputVolume(s, 6);
    CHECK(s[3] > 32000 && s[3] <= 32767);
    CHECK(s[4] < -32000 && s[4] >= -32768);

    /* Clamped; a count of 0 and a NULL buffer are no-ops. */
    xbox_ApuSetOutputVolume(250);
    CHECK(xbox_ApuGetOutputVolume() == 100);
    xbox_ApuSetOutputVolume(-3);
    CHECK(xbox_ApuGetOutputVolume() == 0);
    xbox_ApuApplyOutputVolume(NULL, 6);
    xbox_ApuApplyOutputVolume(s, 0);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("apu_volume: all checks passed\n");
    return 0;
}
