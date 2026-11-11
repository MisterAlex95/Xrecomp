/* The output volume (POSIX and Windows hosts): one gain applied to every
 * mixed EP period before it reaches the audio device (apu_core.c), after the
 * WAV capture, which keeps the raw mix.
 *
 * Set from any thread (the Android in-game menu's volume, through JNI,
 * docs/android/app-shell.md « In-game menu »); read by the APU frame thread
 * once per period. 100 (the default) leaves the samples untouched; 0 is
 * silence. The gain is the square of the fraction, so the steps sound
 * evenly spaced (50 % is about -12 dB). */
#ifndef XBOX_APU_VOLUME_H
#define XBOX_APU_VOLUME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clamped to 0..100. */
void xbox_ApuSetOutputVolume(int percent);
int  xbox_ApuGetOutputVolume(void);

/* Scales count interleaved samples in place by the current volume. */
void xbox_ApuApplyOutputVolume(int16_t *samples, int count);

#ifdef __cplusplus
}
#endif

#endif
