/* dev_test.h -- shared helpers of the PS1 device tests (test_disc.c, test_cdrom.c, test_spu.c, test_mdec.c): CHECK,
 * a scratch directory, synthetic sectors. Our own code. */
#ifndef PS1_DEV_TEST_H
#define PS1_DEV_TEST_H

#define _POSIX_C_SOURCE 200809L
#include "ps1_devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures __attribute__((unused));

#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            failures++;                                                                                        \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                                        \
            printf(__VA_ARGS__);                                                                               \
            printf("\n");                                                                                      \
        }                                                                                                      \
    } while (0)

/* A fresh scratch directory under $TMPDIR (or /tmp), removed by scratch_cleanup. */
static char scratch_dir[512];

static inline const char *scratch(void)
{
    const char *t = getenv("TMPDIR");
    snprintf(scratch_dir, sizeof scratch_dir, "%s/ps1devXXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(scratch_dir)) {
        perror("mkdtemp");
        exit(1);
    }
    return scratch_dir;
}

static inline void scratch_cleanup(void)
{
    char cmd[600];
    if (!scratch_dir[0])
        return;
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", scratch_dir);
    if (system(cmd) != 0)
        printf("warning: could not remove %s\n", scratch_dir);
}

static inline void path_in(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", scratch_dir, name); }

static inline void write_file(const char *name, const void *data, size_t len)
{
    char p[700];
    path_in(p, sizeof p, name);
    FILE *f = fopen(p, "wb");
    if (!f || fwrite(data, 1, len, f) != len) {
        printf("cannot write %s\n", p);
        exit(1);
    }
    fclose(f);
}

static inline uint8_t to_bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* A raw Mode 2 Form 1 sector at lba: sync, header, subheader (file / channel / submode / coding), payload pattern. */
static inline void make_data_sector(uint8_t *s, uint32_t lba, uint8_t submode)
{
    uint32_t a = lba + 150;
    memset(s, 0, 2352);
    memset(s + 1, 0xFF, 10);
    s[12] = to_bcd(a / 4500);
    s[13] = to_bcd((a / 75) % 60);
    s[14] = to_bcd(a % 75);
    s[15] = 2;
    s[16] = s[20] = 1;     /* file */
    s[17] = s[21] = 0;     /* channel */
    s[18] = s[22] = submode;
    for (int i = 24; i < 2352; i++)
        s[i] = (uint8_t)(lba * 7 + i);
}

/* A CD-DA sector: 588 stereo frames, left = lba * 16 + i, right = -(left). */
static inline void make_audio_sector(uint8_t *s, uint32_t lba)
{
    for (int i = 0; i < 588; i++) {
        int16_t l = (int16_t)(lba * 16 + i), r = (int16_t)-l;
        s[4 * i] = (uint8_t)l;
        s[4 * i + 1] = (uint8_t)((uint16_t)l >> 8);
        s[4 * i + 2] = (uint8_t)r;
        s[4 * i + 3] = (uint8_t)((uint16_t)r >> 8);
    }
}

#endif
