/* host_audio.c -- see host_audio.h (XBA-145). */
#include "host_audio.h"
#include "recomp_lifecycle.h"

#include <SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID s_dev;
static int s_rate, s_channels;
static int s_listening;
static int s_volume = 100;

int host_audio_env_buffer_samples(void)
{
    const char *e = getenv("RECOMP_AUDIO_BUF_SAMPLES");
    int v;

    if (!e || !*e)
        return HOST_AUDIO_BUF_SAMPLES_DEFAULT;
    v = atoi(e);
    return v < 128 ? 128 : v > 4096 ? 4096 : v;
}

void host_audio_set_volume(int percent)
{
    __atomic_store_n(&s_volume, percent < 0 ? 0 : percent > 100 ? 100 : percent, __ATOMIC_RELAXED);
}

int host_audio_volume(void)
{
    return __atomic_load_n(&s_volume, __ATOMIC_RELAXED);
}

void host_audio_scale(int16_t *dst, const int16_t *src, uint32_t count, int percent)
{
    if (percent >= 100) {
        if (dst != src)
            memmove(dst, src, (size_t)count * sizeof *dst);
        return;
    }
    if (percent < 0)
        percent = 0;
    /* Q15: (percent / 100)^2; at most 32767 * 32768 >> 15, no overflow. */
    int32_t gain = (int32_t)((int64_t)percent * percent * 32768 / 10000);
    for (uint32_t i = 0; i < count; i++)
        dst[i] = (int16_t)(((int32_t)src[i] * gain) >> 15);
}

/* The app in the background: the device stops with it (recomp_lifecycle.h). */
static void on_lifecycle(int paused, void *user)
{
    (void)user;
    pthread_mutex_lock(&s_mu);
    if (s_dev)
        SDL_PauseAudioDevice(s_dev, paused);
    pthread_mutex_unlock(&s_mu);
}

int host_audio_open(int rate, int channels)
{
    SDL_AudioSpec want, have;

    pthread_mutex_lock(&s_mu);
    if (s_dev) {
        pthread_mutex_unlock(&s_mu);
        return 1;
    }
    if (rate <= 0)
        rate = 48000;
    if (channels != 1)
        channels = 2;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "  [HOST] audio: SDL audio: %s\n", SDL_GetError());
        pthread_mutex_unlock(&s_mu);
        return 0;
    }
    SDL_zero(want);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples = (Uint16)host_audio_env_buffer_samples();
    want.callback = NULL;   /* queued: SDL_QueueAudio */
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s_dev) {
        fprintf(stderr, "  [HOST] audio: no output device: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        pthread_mutex_unlock(&s_mu);
        return 0;
    }
    s_rate = have.freq;
    s_channels = have.channels;
    SDL_PauseAudioDevice(s_dev, recomp_lifecycle_paused());
    pthread_mutex_unlock(&s_mu);
    if (!s_listening && recomp_lifecycle_listen(on_lifecycle, NULL) == 0)
        s_listening = 1;
    fprintf(stderr, "  [HOST] audio: %d Hz, %d channel(s), buffer %u frames, %s\n", s_rate, s_channels,
            (unsigned)have.samples, SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    return 1;
}

int host_audio_queue(const int16_t *frames, uint32_t nframes)
{
    int ok = 0, vol = host_audio_volume();

    pthread_mutex_lock(&s_mu);
    if (s_dev && frames && nframes) {
        uint32_t left = nframes * (uint32_t)s_channels;
        ok = 1;
        if (vol >= 100) {
            ok = SDL_QueueAudio(s_dev, frames, left * (uint32_t)sizeof(int16_t)) == 0;
        } else {
            /* The volume: scaled through a small buffer, the caller's samples left alone. */
            int16_t tmp[2048];
            while (ok && left) {
                uint32_t n = left < 2048 ? left : 2048;
                host_audio_scale(tmp, frames, n, vol);
                ok = SDL_QueueAudio(s_dev, tmp, n * (uint32_t)sizeof(int16_t)) == 0;
                frames += n;
                left -= n;
            }
        }
    } else if (s_dev) {
        ok = 1;
    }
    pthread_mutex_unlock(&s_mu);
    return ok;
}

uint32_t host_audio_queued(void)
{
    uint32_t n = 0;

    pthread_mutex_lock(&s_mu);
    if (s_dev && s_channels)
        n = SDL_GetQueuedAudioSize(s_dev) / ((uint32_t)s_channels * (uint32_t)sizeof(int16_t));
    pthread_mutex_unlock(&s_mu);
    return n;
}

int host_audio_rate(void)
{
    return s_rate;
}

int host_audio_channels(void)
{
    return s_channels;
}

void host_audio_close(void)
{
    pthread_mutex_lock(&s_mu);
    if (s_dev) {
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
        s_rate = s_channels = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    pthread_mutex_unlock(&s_mu);
}
