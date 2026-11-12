/* Which host cores the runtime runs on (host_cpus.h). */
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include "host_cpus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sched.h>
#if defined(__linux__)
#include <errno.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif

/* One number from <root>/cpu<n>/<file>, 0 if absent or unreadable. */
static unsigned long read_cpu_value(const char *root, int cpu, const char *file)
{
    char path[512];
    unsigned long v = 0;
    FILE *f;
    snprintf(path, sizeof path, "%s/cpu%d/%s", root, cpu, file);
    f = fopen(path, "r");
    if (!f) return 0;
    if (fscanf(f, "%lu", &v) != 1) v = 0;
    fclose(f);
    return v;
}

uint64_t recomp_big_core_mask(const char *root, uint64_t allowed,
                              unsigned long *slow_khz, unsigned long *fast_khz)
{
    unsigned long val[64];
    unsigned long lo = 0, hi = 0;
    int cpu, pass;
    uint64_t big = 0;

    if (slow_khz) *slow_khz = 0;
    if (fast_khz) *fast_khz = 0;
    /* pass 0: cpufreq maxima; pass 1: cpu_capacity, if no core had a maximum */
    for (pass = 0; pass < 2; pass++) {
        int seen = 0;
        lo = hi = 0;
        for (cpu = 0; cpu < 64; cpu++) {
            val[cpu] = 0;
            if (!(allowed >> cpu & 1)) continue;
            val[cpu] = read_cpu_value(root, cpu, pass == 0 ? "cpufreq/cpuinfo_max_freq" : "cpu_capacity");
            if (!val[cpu]) continue;
            seen++;
            if (!lo || val[cpu] < lo) lo = val[cpu];
            if (val[cpu] > hi) hi = val[cpu];
        }
        if (seen) break;
    }
    if (pass == 2 || lo == hi)
        return allowed;                 /* nothing readable, or all cores alike */
    for (cpu = 0; cpu < 64; cpu++)
        if ((allowed >> cpu & 1) && val[cpu] > lo)
            big |= (uint64_t)1 << cpu;
    if (slow_khz) *slow_khz = lo;
    if (fast_khz) *fast_khz = hi;
    return big ? big : allowed;
}

static void mask_text(uint64_t m, char *buf, size_t size)
{
    int cpu = 0;
    size_t used = 0;
    buf[0] = '\0';
    while (cpu < 64 && used + 8 < size) {
        int start;
        if (!(m >> cpu & 1)) { cpu++; continue; }
        start = cpu;
        while (cpu + 1 < 64 && (m >> (cpu + 1) & 1)) cpu++;
        used += (size_t)snprintf(buf + used, size - used, "%s%d", used ? "," : "", start);
        if (cpu > start)
            used += (size_t)snprintf(buf + used, size - used, "-%d", cpu);
        cpu++;
    }
}

uint64_t recomp_host_cpus_init(void)
{
#if defined(__linux__)
    const char *env = getenv("RECOMP_BIG_CORES");
    int on;
    cpu_set_t set;
    uint64_t allowed = 0, big;
    unsigned long slow = 0, fast = 0;
    char a[96], b[96];
    int cpu;

# if defined(__ANDROID__)
    on = !(env && *env == '0');
# else
    on = env && *env == '1';
# endif
    if (!on)
        return 0;
    if (sched_getaffinity(0, sizeof set, &set) != 0)
        return 0;
    for (cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &set)) allowed |= (uint64_t)1 << cpu;
    big = recomp_big_core_mask("/sys/devices/system/cpu", allowed, &slow, &fast);
    mask_text(allowed, a, sizeof a);
    if (big == allowed) {
        printf("host cpus: %s, all alike: affinity left alone\n", a);
        fflush(stdout);
        return 0;
    }
    mask_text(big, b, sizeof b);
    CPU_ZERO(&set);
    for (cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; cpu++)
        if (big >> cpu & 1) CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) {
        fprintf(stderr, "host cpus: could not restrict to the fast cores %s\n", b);
        return 0;
    }
    printf("host cpus: runtime on the fast cores %s of %s (max %lu vs %lu on the slow ones; RECOMP_BIG_CORES=0 to undo)\n",
           b, a, fast, slow);
    fflush(stdout);
    recomp_host_cpus_track(big);
    return big;
#else
    return 0;
#endif
}

/* ── The runtime's threads (XBA-50) ─────────────────────────────────────── */

#if defined(__linux__)
#define TRACK_MAX 256
static struct { pid_t tid; uint64_t mask; } s_track[TRACK_MAX];
static int s_track_n;
static pthread_mutex_t s_track_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t mask_of(pid_t tid)
{
    cpu_set_t set;
    uint64_t m = 0;
    if (sched_getaffinity(tid, sizeof set, &set) != 0)
        return 0;
    for (int cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &set)) m |= (uint64_t)1 << cpu;
    return m;
}

static int set_mask(pid_t tid, uint64_t m)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; cpu++)
        if (m >> cpu & 1) CPU_SET(cpu, &set);
    return sched_setaffinity(tid, sizeof set, &set);
}
#endif

void recomp_host_cpus_track(uint64_t mask)
{
#if defined(__linux__)
    pid_t tid = (pid_t)syscall(SYS_gettid);
    int i;
    if (!mask)
        mask = mask_of(tid);
    if (!mask)
        return;
    pthread_mutex_lock(&s_track_lock);
    for (i = 0; i < s_track_n; i++)
        if (s_track[i].tid == tid)
            break;
    if (i == s_track_n) {
        /* Full: reuse the slot of a thread that has gone. */
        if (s_track_n == TRACK_MAX) {
            for (i = 0; i < TRACK_MAX; i++)
                if (sched_getaffinity(s_track[i].tid, sizeof(cpu_set_t), &(cpu_set_t){0}) != 0 && errno == ESRCH)
                    break;
            if (i == TRACK_MAX) {
                pthread_mutex_unlock(&s_track_lock);
                return;
            }
        } else {
            s_track_n++;
        }
    }
    s_track[i].tid = tid;
    s_track[i].mask = mask;
    pthread_mutex_unlock(&s_track_lock);
#else
    (void)mask;
#endif
}

int recomp_host_cpus_restore(void)
{
#if defined(__linux__)
    int changed = 0, gone = 0, failed = 0;
    pthread_mutex_lock(&s_track_lock);
    for (int i = 0; i < s_track_n; i++) {
        uint64_t now = mask_of(s_track[i].tid);
        if (!now) {
            gone++;             /* exited (ESRCH): its slot is reused later */
            continue;
        }
        if (now == s_track[i].mask)
            continue;
        if (set_mask(s_track[i].tid, s_track[i].mask) == 0)
            changed++;
        else
            failed++;
    }
    pthread_mutex_unlock(&s_track_lock);
    if (changed || failed) {
        printf("host cpus: %d runtime thread(s) back on their cores after resume%s\n", changed,
               failed ? " (some could not be moved)" : "");
        fflush(stdout);
    }
    (void)gone;
    return changed;
#else
    return 0;
#endif
}
