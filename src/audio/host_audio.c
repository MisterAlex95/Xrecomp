/* host_audio.c -- see host_audio.h (XBA-145). */
#include "host_audio.h"
#include "recomp_lifecycle.h"

#include <SDL.h>

#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID s_dev;
static int s_rate, s_channels;
static int s_listening;

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
    want.samples = 1024;
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
    fprintf(stderr, "  [HOST] audio: %d Hz, %d channel(s), %s\n", s_rate, s_channels,
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    return 1;
}

int host_audio_queue(const int16_t *frames, uint32_t nframes)
{
    int ok = 0;

    pthread_mutex_lock(&s_mu);
    if (s_dev && frames && nframes)
        ok = SDL_QueueAudio(s_dev, frames, nframes * (uint32_t)s_channels * (uint32_t)sizeof(int16_t)) == 0;
    else if (s_dev)
        ok = 1;
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
