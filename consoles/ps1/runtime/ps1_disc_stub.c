/* Placeholder disc reader (ps1_devices.h), built when ps1_disc.c (P-1, XBA-159) is absent: no image opens. */
#include "ps1_devices.h"

#include <stdio.h>

ps1_disc *ps1_disc_open(const char *path, char *err, size_t errlen)
{
    if (err && errlen)
        snprintf(err, errlen, "%s: disc images are not supported in this build (no ps1_disc.c)", path ? path : "?");
    return NULL;
}

void ps1_disc_close(ps1_disc *d)
{
    (void)d;
}

int ps1_disc_read_sector(ps1_disc *d, uint32_t lba, uint8_t out[2352])
{
    (void)d;
    (void)lba;
    (void)out;
    return -1;
}

int ps1_disc_track_count(ps1_disc *d)
{
    (void)d;
    return 0;
}

int ps1_disc_track(ps1_disc *d, int t, uint32_t *start_lba, int *audio)
{
    (void)d;
    (void)t;
    (void)start_lba;
    (void)audio;
    return -1;
}

uint32_t ps1_disc_size(ps1_disc *d)
{
    (void)d;
    return 0;
}
