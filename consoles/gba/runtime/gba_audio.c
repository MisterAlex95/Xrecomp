/* gba_audio.c -- audio output of the GBA core (G-6, XBA-155). Placeholder until the APU lands: silence. */
#include "gba_core.h"

size_t gba_audio_pull(gba_machine *m, int16_t *out, size_t max_frames)
{
    (void)m;
    (void)out;
    (void)max_frames;
    return 0;
}
