/* host_audio -- the host layer's audio out: one SDL device fed with signed
 * 16-bit interleaved frames (XBA-145, src/platform/HOST_LAYER.md).
 *
 * A console runtime mixes its own voices and queues the result here; the
 * device pauses while the app is in the background (recomp_lifecycle) and
 * resumes with it. The Xbox runtime keeps its own output (src/apu,
 * apu_sdl2.c), which predates this. Thread-safe. POSIX hosts. */
#ifndef XBR_HOST_AUDIO_H
#define XBR_HOST_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the default output at rate Hz with channels (1 or 2). Returns 1, or
 * 0 when there is no device (SDL_AUDIODRIVER=dummy opens a silent one). A
 * second open returns 1 and keeps the first format. */
int host_audio_open(int rate, int channels);

/* Queues nframes frames (nframes * channels samples). Returns 1, or 0 when
 * the device is not open. */
int host_audio_queue(const int16_t *frames, uint32_t nframes);

/* Frames queued and not yet played: a runtime keeps this near its latency
 * target (e.g. two video frames' worth). */
uint32_t host_audio_queued(void);

/* The rate and channel count the device runs at (0 before open). */
int host_audio_rate(void);
int host_audio_channels(void);

void host_audio_close(void);

#ifdef __cplusplus
}
#endif

#endif /* XBR_HOST_AUDIO_H */
