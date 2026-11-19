/*
 * The shader badge's counter (host_shader_progress.c, XBA-314), driven with made-up clocks:
 *   hidden      nothing reported: no text
 *   batch       expect / done: "Shaders 1/304" counted from 1, the last "304/304", gone once all are done; more done
 *               than expected never shows "305/304"; failed loads count as done (the caller's), so it always closes
 *   new batch   an expect after a finished batch starts from 0; an expect during one adds to its total
 *   set         an absolute report (GX, N64) replaces the counts; done > total clamped; total 0 ends the batch
 *   live        "Shaders: compiling... (n)" until 1 s after the last compile, n from 1 again after it went away
 *   precedence  a batch in progress shows over live compiles; live shows again once the batch is done
 *   reset       everything forgotten
 *   threads     4 threads x 10000 done against one expect of 40000: the batch ends exactly, the text never shows a
 *               count past the total while they run
 */
#include "host_shader_progress.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static int text_is(uint64_t now, const char *want)
{
    char t[64];
    int shown = host_shader_progress_text_at(now, t, sizeof t);
    if (strcmp(t, want) != 0 || shown != (want[0] != 0)) {
        printf("    got \"%s\" (shown %d), want \"%s\"\n", t, shown, want);
        return 0;
    }
    return 1;
}

static volatile int s_stop;
static int s_bad_text;

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 10000; i++)
        host_shader_progress_done(1);
    return NULL;
}

static void *reader(void *arg)
{
    (void)arg;
    while (!s_stop) {
        char t[64];
        unsigned a = 0, b = 0;
        if (host_shader_progress_text_at(5000, t, sizeof t) && sscanf(t, "Shaders %u/%u", &a, &b) == 2
            && (a > b || a == 0))
            s_bad_text = 1;
    }
    return NULL;
}

int main(void)
{
    char t[8];

    printf("host_shader_progress\n");
    host_shader_progress_reset();
    check(text_is(1000, ""), "hidden: nothing reported");

    host_shader_progress_expect(304);
    check(text_is(1000, "Shaders 1/304"), "batch: the first one is 1/304");
    host_shader_progress_done(11);
    check(text_is(1000, "Shaders 12/304"), "batch: 11 done shows 12/304");
    host_shader_progress_done(292);
    check(text_is(1000, "Shaders 304/304"), "batch: the last one is 304/304");
    host_shader_progress_done(1);
    check(text_is(1000, ""), "batch: gone once all are done");
    host_shader_progress_done(5);
    check(text_is(1000, ""), "batch: done past the end is ignored");

    host_shader_progress_expect(10);
    check(text_is(1000, "Shaders 1/10"), "new batch: starts from 0");
    host_shader_progress_done(4);
    host_shader_progress_expect(5);
    check(text_is(1000, "Shaders 5/15"), "new batch: an expect during one adds to its total");
    host_shader_progress_done(100);
    check(text_is(1000, ""), "new batch: more done than expected closes it");

    host_shader_progress_set(3, 40);
    check(text_is(1000, "Shaders 4/40"), "set: replaces the counts");
    host_shader_progress_set(50, 40);
    check(text_is(1000, ""), "set: done > total clamped, the batch is over");
    host_shader_progress_set(0, 7);
    host_shader_progress_set(0, 0);
    check(text_is(1000, ""), "set: total 0 ends the batch");

    host_shader_progress_live_at(2000);
    check(text_is(2000, "Shaders: compiling... (1)"), "live: one compile");
    host_shader_progress_live_at(2400);
    host_shader_progress_live_at(2900);
    check(text_is(3000, "Shaders: compiling... (3)"), "live: three within the second");
    check(text_is(3899, "Shaders: compiling... (3)"), "live: still shown 999 ms after the last");
    check(text_is(3900, ""), "live: gone 1 s after the last");
    host_shader_progress_live_at(5000);
    check(text_is(5000, "Shaders: compiling... (1)"), "live: counts from 1 again after it went away");

    host_shader_progress_expect(2);
    host_shader_progress_live_at(5100);
    check(text_is(5100, "Shaders 1/2"), "precedence: a batch shows over live compiles");
    host_shader_progress_done(2);
    check(text_is(5200, "Shaders: compiling... (2)"), "precedence: live again once the batch is done");

    host_shader_progress_reset();
    check(text_is(5200, ""), "reset: everything forgotten");
    check(host_shader_progress_text_at(5200, t, 0) == 0, "reset: a zero-size buffer is left alone");

    {
        pthread_t th[4], rd;
        host_shader_progress_expect(40000);
        s_stop = 0;
        pthread_create(&rd, NULL, reader, NULL);
        for (int i = 0; i < 4; i++)
            pthread_create(&th[i], NULL, worker, NULL);
        for (int i = 0; i < 4; i++)
            pthread_join(th[i], NULL);
        s_stop = 1;
        pthread_join(rd, NULL);
        check(text_is(5000, ""), "threads: 4 x 10000 done close a batch of 40000");
        check(!s_bad_text, "threads: never a count past the total or 0 while they ran");
    }

    check(host_shader_progress_now_ms() > 0, "clock: monotonic ms");
    printf(failures ? "FAILED (%d)\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
