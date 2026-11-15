/* test_vk_warmup.c -- the N64 Vulkan RDP's shader warm-up (n64_rdp.h n64_rdp_vk_warmup) and its start-up progress
 * (n64_rdp_shader_progress), on a real Vulkan device (skipped, 77, without one; Mesa's lavapipe runs it on a machine
 * without a GPU). Two warm-ups into a fresh RECOMP_VK_CACHE_DIR: the first builds and writes pipelines-<uuid>.bin and
 * startup-<f>x.count; the second reads the cache, builds the same start-up set (the count the first one wrote is its
 * progress' total, seen by a thread that polls it as the app's UI does), and with N64_WARMUP_EXPECT_NO_COMPILES=1
 * compiles nothing that takes 5 ms or more (a driver that gives its pipeline cache's data; lavapipe gives none and
 * still JITs from Mesa's disk cache, about 10x faster than the first run).
 *   n64_vk_warmup_test <cache dir> [factor, default 2] */
#include "n64_rdp.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c)                                                                                                        \
    do {                                                                                                                \
        if (!(c)) {                                                                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                \
            fails++;                                                                                                    \
        }                                                                                                               \
    } while (0)

static volatile int s_stop;
static int s_seen_active, s_seen_total, s_seen_done_max;

static void *poll_progress(void *arg)
{
    (void)arg;
    while (!s_stop) {
        int done = 0, total = 0;
        if (n64_rdp_shader_progress(&done, &total)) {
            s_seen_active = 1;
            if (total > s_seen_total)
                s_seen_total = total;
            if (done > s_seen_done_max)
                s_seen_done_max = done;
        }
        usleep(200);
    }
    return NULL;
}

/* rm -rf of a cache directory made by the first run (two levels: mesa/, ssbo-emulation/). */
static void wipe(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    char p[2048];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (remove(p) != 0 && depth < 4)
            wipe(p, depth + 1);
    }
    closedir(d);
    rmdir(dir);
}

/* A pipelines-<uuid>.bin in dir (or its ssbo-emulation/). */
static int has_pipelines_file(const char *dir)
{
    char sub[2048];
    snprintf(sub, sizeof sub, "%s/ssbo-emulation", dir);
    const char *dirs[] = { dir, sub };
    for (int i = 0; i < 2; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d)
            continue;
        struct dirent *e;
        int found = 0;
        while ((e = readdir(d)))
            found |= !strncmp(e->d_name, "pipelines-", 10) && strstr(e->d_name, ".bin") && !strstr(e->d_name, ".tmp");
        closedir(d);
        if (found)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: n64_vk_warmup_test <cache dir> [factor]\n");
        return 2;
    }
    const char *dir = argv[1];
    const int factor = argc > 2 ? atoi(argv[2]) : 2;
    wipe(dir, 0);
    setenv("RECOMP_VK_CACHE_DIR", dir, 1);
    setenv("RECOMP_N64_RDP", "vulkan", 1);

    n64_rdp_warmup_result a, b;
    int done = -1, total = -1;
    CHECK(n64_rdp_shader_progress(&done, &total) == 0);
    const int rc1 = n64_rdp_vk_warmup(factor, &a);
    if (rc1 == 1 || rc1 == 3) {
        printf("n64_vk_warmup: no Vulkan RDP here (%d), skipped\n", rc1);
        wipe(dir, 0);
        return 77;
    }
    CHECK(rc1 == 0);
    CHECK(a.factor >= 1 && a.startup > 0);
    CHECK(n64_rdp_shader_progress(&done, &total) == 0); /* over */
    printf("n64_vk_warmup: first: %dx, %d start-up + %d VI pipelines, %d compiled, cache read %u KB, %.2f s\n",
           a.factor, a.startup, a.extra, a.compiled, a.cache_kb, a.seconds);
    CHECK(a.cache_kb == 0);

    pthread_t th;
    pthread_create(&th, NULL, poll_progress, NULL);
    const int rc2 = n64_rdp_vk_warmup(factor, &b);
    s_stop = 1;
    pthread_join(th, NULL);
    CHECK(rc2 == 0);
    printf("n64_vk_warmup: second: %dx, %d start-up + %d VI pipelines, %d compiled, cache read %u KB, %.2f s; "
           "progress seen: active %d, total %d, done up to %d\n", b.factor, b.startup, b.extra, b.compiled, b.cache_kb,
           b.seconds, s_seen_active, s_seen_total, s_seen_done_max);
    /* pipeline cache loaded, when the driver gives its data (lavapipe gives none: only Mesa's disk cache is kept) */
    if (has_pipelines_file(dir))
        CHECK(b.cache_kb > 0);
    else
        printf("n64_vk_warmup: the driver keeps no pipeline cache data (Mesa's disk cache only)\n");
    CHECK(b.startup == a.startup);      /* the same start-up set: the count is a fair total */
    CHECK(b.factor == a.factor);
    CHECK(s_seen_active);
    CHECK(s_seen_total == a.startup);   /* "7/13": the first run's count */
    CHECK(s_seen_done_max <= a.startup);
    const char *strict = getenv("N64_WARMUP_EXPECT_NO_COMPILES");
    if (strict && atoi(strict))
        CHECK(b.compiled == 0);

    unsetenv("RECOMP_VK_CACHE_DIR");
    CHECK(n64_rdp_vk_warmup(factor, NULL) == 2); /* nothing would be kept */
    wipe(dir, 0);
    if (fails) {
        fprintf(stderr, "n64_vk_warmup: %d failure(s)\n", fails);
        return 1;
    }
    printf("n64_vk_warmup: ok\n");
    return 0;
}
