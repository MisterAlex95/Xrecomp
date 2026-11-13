/* The status file (XBA-79): recomp_status.h has the format. Depends only on
 * the discovery writer, so a test builds the two alone. */
#include "recomp_status.h"
#include "recomp_discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

static volatile int s_on;               /* 1 once started with a file */
static FILE *s_file;
static uint64_t s_t0_ms;
static unsigned s_period_ms = 1000;

/* One writer each (executor, audio, USB, kernel threads): plain counters,
 * read once a second; a torn read is one stale number. */
static volatile uint64_t s_flips, s_frames, s_audio_samples, s_pad_reads, s_threads;
static volatile uint32_t s_hash, s_lit;
static const char *volatile s_net;      /* a literal, or NULL: no NIC touched */

/* "unsupported" and "unmodelled" (XBA-120): names by pointer, appended by
 * one writer; the entry is filled before the count of entries grows, so the
 * status thread never reads a half-made one. */
#define STATUS_GAPS 32
#define STATUS_UNMOD 16
typedef struct { const char *name; volatile uint64_t count; } StatusNamed;
static StatusNamed s_gaps[STATUS_GAPS], s_unmod[STATUS_UNMOD];
static volatile int s_ngaps, s_nunmod;

static void named_add(StatusNamed *t, volatile int *n, int max, const char *name,
                      uint64_t v, int set)
{
    int i, have = *n;

    for (i = 0; i < have; i++)
        if (t[i].name == name) {
            t[i].count = set ? v : t[i].count + v;
            return;
        }
    if (have >= max)
        return;
    t[have].name = name;
    t[have].count = v;
#ifdef _WIN32
    MemoryBarrier();
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
    *n = have + 1;
}

void recomp_status_unsupported(const char *name, uint32_t n)
{
    if (s_on && name)
        named_add(s_gaps, &s_ngaps, STATUS_GAPS, name, n, 0);
}

void recomp_status_unmodelled(const char *name, uint64_t count)
{
    if (s_on && name)
        named_add(s_unmod, &s_nunmod, STATUS_UNMOD, name, count, 1);
}

/* ,"key":{"name":count,...} for a non-empty table, into f. */
static void named_write(FILE *f, const char *key, const StatusNamed *t, int n)
{
    int i;

    if (!n)
        return;
#ifdef _WIN32
    MemoryBarrier();
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
    fprintf(f, ",\"%s\":{", key);
    for (i = 0; i < n; i++)
        fprintf(f, "%s\"%s\":%llu", i ? "," : "", t[i].name, (unsigned long long)t[i].count);
    fputc('}', f);
}

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

void recomp_status_write_now(void)
{
    if (!s_on || !s_file)
        return;
    fprintf(s_file, "{\"ms\":%llu,\"flips\":%llu,\"frames\":%llu,\"hash\":\"0x%08X\",\"lit\":%u,"
                    "\"audio_ms\":%llu,\"pad_reads\":%llu,\"threads\":%llu,\"discovered\":%u,\"seeds\":%u",
            (unsigned long long)(now_ms() - s_t0_ms), (unsigned long long)s_flips,
            (unsigned long long)s_frames, s_hash, s_lit ? 1u : 0u,
            (unsigned long long)(s_audio_samples / 48u), (unsigned long long)s_pad_reads,
            (unsigned long long)s_threads, recomp_discovery_written(), recomp_discovery_seeds());
    named_write(s_file, "unsupported", s_gaps, s_ngaps);
    named_write(s_file, "unmodelled", s_unmod, s_nunmod);
    if (s_net)
        fprintf(s_file, ",\"net\":\"%s\"", s_net);   /* XBA-117 */
    fputs("}\n", s_file);
    fflush(s_file);
}

#ifdef _WIN32
static DWORD WINAPI status_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(s_period_ms);
        recomp_status_write_now();
    }
    return 0;
}
#else
static void *status_thread(void *unused)
{
    (void)unused;
    for (;;) {
        usleep(s_period_ms * 1000u);
        recomp_status_write_now();
    }
    return NULL;
}
#endif

static int open_file(void)
{
    const char *p = getenv("RECOMP_STATUS"), *ms = getenv("RECOMP_STATUS_MS");
    if (!p || !*p || !strcmp(p, "0"))
        return 0;
    s_file = fopen(p, "a");
    if (!s_file) {
        fprintf(stderr, "[STATUS] cannot open %s: no status file\n", p);
        return 0;
    }
    if (ms && atoi(ms) >= 50)
        s_period_ms = (unsigned)atoi(ms);
    s_t0_ms = now_ms();
    s_on = 1;
    fprintf(stderr, "[STATUS] every %u ms to %s\n", s_period_ms, p);
    return 1;
}

void recomp_status_start(void)
{
    static volatile int started;
#ifdef _WIN32
    if (InterlockedCompareExchange((volatile LONG *)&started, 1, 0) != 0)
        return;
    if (open_file()) {
        HANDLE t = CreateThread(NULL, 0, status_thread, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
    }
#else
    pthread_t t;
    if (__sync_val_compare_and_swap(&started, 0, 1) != 0)
        return;
    if (open_file() && pthread_create(&t, NULL, status_thread, NULL) == 0)
        pthread_detach(t);
#endif
}

void recomp_status_frame(const void *px, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp)
{
    uint32_t hash = 2166136261u, lit = 0, gx, gy;

    if (!s_on)
        return;
    s_flips++;
    if (!px || !w || !h || !pitch || (bpp != 2 && bpp != 4 && bpp != 8))
        return;
    /* FNV-1a over a 32x24 grid: enough to tell a changing picture from a
     * frozen one, cheap enough for every flip. */
    for (gy = 0; gy < 24; gy++) {
        const uint8_t *row = (const uint8_t *)px + (size_t)(gy * h / 24) * pitch;
        for (gx = 0; gx < 32; gx++) {
            uint32_t x = gx * w / 32, v, r, g, b;
            if (bpp == 2) {
                v = ((const uint16_t *)row)[x];
                r = (v >> 11) << 3; g = ((v >> 5) & 0x3F) << 2; b = (v & 0x1F) << 3;
            } else {
                v = ((const uint32_t *)row)[bpp == 8 ? x * 2 : x];
                r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF;
            }
            v = r << 16 | g << 8 | b;
            if (r > 0x18 || g > 0x18 || b > 0x18)
                lit = 1;
            for (int k = 0; k < 3; k++) {
                hash ^= (v >> (k * 8)) & 0xFF;
                hash *= 16777619u;
            }
        }
    }
    if (hash != s_hash)
        s_frames++;
    s_hash = hash;
    s_lit = lit;
}

void recomp_status_audio(const int16_t *stereo, int count)
{
    uint64_t loud = 0;
    if (!s_on || !stereo)
        return;
    for (int i = 0; i < count; i++) {
        int l = stereo[2 * i], r = stereo[2 * i + 1];
        if (l > 64 || l < -64 || r > 64 || r < -64)
            loud++;
    }
    s_audio_samples += loud;
}

void recomp_status_pad_read(void)
{
    if (s_on)
        s_pad_reads++;
}

void recomp_status_thread_started(void)
{
    if (s_on)
        s_threads++;
}

void recomp_status_net(const char *what)
{
    s_net = what;               /* kept with the status off too: cheap, and */
}                               /* a later start still reports it */

void recomp_status_reset(void)
{
    if (s_file)
        fclose(s_file);
    s_file = NULL;
    s_on = 0;
    s_flips = s_frames = s_audio_samples = s_pad_reads = s_threads = 0;
    s_hash = s_lit = 0;
    s_ngaps = s_nunmod = 0;
    s_net = NULL;
    s_period_ms = 1000;
    (void)open_file();          /* tests call write_now themselves: no thread */
}
