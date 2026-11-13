/* The status file (XBA-79): one JSON line per period with what a headless
 * run did -- flips, how often the picture changed, sound, pad reads. */
#define _POSIX_C_SOURCE 200809L
#include "recomp_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <unistd.h>
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char *last_line(const char *path)
{
    static char line[2048], buf[2048];
    FILE *f = fopen(path, "r");
    line[0] = 0;
    if (!f)
        return line;
    while (fgets(buf, sizeof buf, f))
        strcpy(line, buf);
    fclose(f);
    return line;
}

static unsigned long field(const char *line, const char *name)
{
    char key[64];
    const char *p;
    snprintf(key, sizeof key, "\"%s\":", name);
    p = strstr(line, key);
    return p ? strtoul(p + strlen(key), NULL, 0) : 99999999ul;
}

int main(void)
{
    static uint32_t black[64 * 48], lit[64 * 48], lit2[64 * 48];
    static int16_t audio[2 * 480];
    char path[300];
    const char *tmp = getenv("TMPDIR");
    char *l;

    snprintf(path, sizeof path, "%s/xba_status_%d.jsonl", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    remove(path);

    /* Off: nothing counted, nothing written. */
    set_env("RECOMP_STATUS", "0");
    recomp_status_reset();
    recomp_status_frame(lit, 64, 48, 64 * 4, 4);
    recomp_status_write_now();
    CHECK(fopen(path, "r") == NULL);

    set_env("RECOMP_STATUS", path);
    recomp_status_reset();
    for (int i = 0; i < 64 * 48; i++) {
        lit[i] = 0x00808080u;
        lit2[i] = i & 1 ? 0x00FF0000u : 0x000000FFu;
    }
    /* Three flips of a black frame: one picture (the first differs from the
     * empty start), not lit. */
    for (int i = 0; i < 3; i++)
        recomp_status_frame(black, 64, 48, 64 * 4, 4);
    recomp_status_write_now();
    l = last_line(path);
    CHECK(field(l, "flips") == 3 && field(l, "frames") == 1 && field(l, "lit") == 0);

    /* A lit frame, the same again, another one, and a flip with no picture. */
    recomp_status_frame(lit, 64, 48, 64 * 4, 4);
    recomp_status_frame(lit, 64, 48, 64 * 4, 4);
    recomp_status_frame(lit2, 64, 48, 64 * 4, 4);
    recomp_status_frame(NULL, 0, 0, 0, 0);
    /* 16-bit and anti-aliased surfaces are read too. */
    recomp_status_frame(lit, 32, 48, 64 * 2, 2);
    recomp_status_frame(lit, 32, 48, 64 * 4, 8);
    /* 480 stereo frames, half of them loud: 240 samples = 5 ms at 48 kHz. */
    for (int i = 0; i < 480; i++)
        audio[2 * i] = audio[2 * i + 1] = (int16_t)(i < 240 ? 3000 : 10);
    recomp_status_audio(audio, 480);
    recomp_status_audio(audio, 480);
    recomp_status_pad_read();
    recomp_status_pad_read();
    recomp_status_thread_started();
    recomp_status_write_now();
    l = last_line(path);
    CHECK(field(l, "flips") == 9);
    CHECK(field(l, "frames") >= 3 && field(l, "frames") <= 5);
    CHECK(field(l, "lit") == 1);
    CHECK(field(l, "audio_ms") == 10);
    CHECK(field(l, "pad_reads") == 2 && field(l, "threads") == 1 && field(l, "discovered") == 0);
    CHECK(field(l, "seeds") == 0);   /* XBA-129: code the analysis missed, written by this run */
    CHECK(strstr(l, "\"hash\":\"0x") != NULL && l[strlen(l) - 2] == '}');
    /* Nothing unsupported or unmodelled yet: neither key. */
    CHECK(strstr(l, "unsupported") == NULL && strstr(l, "unmodelled") == NULL);

    /* What the renderer could not draw (XBA-120): counted by name, in the
     * order first met; unmodelled methods carry the count last set. */
    {
        static const char *pts = "points", *fmt = "texture format 0x26 LU_IMAGE_A8CR8CB8Y8",
                          *lop = "SET_LOGIC_OP";
        recomp_status_unsupported(pts, 2);
        recomp_status_unsupported(fmt, 5);
        recomp_status_unsupported(pts, 1);
        recomp_status_unmodelled(lop, 7);
        recomp_status_unmodelled(lop, 9);
        recomp_status_write_now();
        l = last_line(path);
        CHECK(strstr(l, ",\"unsupported\":{\"points\":3,\"texture format 0x26 LU_IMAGE_A8CR8CB8Y8\":5}") != NULL);
        CHECK(strstr(l, ",\"unmodelled\":{\"SET_LOGIC_OP\":9}}") != NULL);
        CHECK(field(l, "flips") == 9 && l[strlen(l) - 2] == '}');
    }

    recomp_status_reset();
    remove(path);
    if (failures)
        return 1;
    printf("recomp_status: ok\n");
    return 0;
}
