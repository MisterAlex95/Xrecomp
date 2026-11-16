/* recomp_big_core_mask (XBA-25): the fast cores of a big.LITTLE host, and
 * recomp_fastest_core (WP-E): the guest's one core, read from a fake
 * /sys/devices/system/cpu tree. */
#if !defined(_WIN32)
#define _GNU_SOURCE   /* sched_getaffinity, CPU_* (XBA-50) */
#endif
#include "host_cpus.h"
#include <sched.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static char root[256];

static void put(int cpu, const char *file, unsigned long v)
{
    char path[512], dir[512];
    FILE *f;
    snprintf(dir, sizeof dir, "%s/cpu%d", root, cpu);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/cpu%d/cpufreq", root, cpu);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/cpu%d/%s", root, cpu, file);
    f = fopen(path, "w");
    if (f) { fprintf(f, "%lu\n", v); fclose(f); }
}

static void clean(void)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) { /* best effort */ }
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    unsigned long lo, hi;
    uint64_t m;
    int cpu;

    snprintf(root, sizeof root, "%s/host_cpus_test_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    clean();
    mkdir(root, 0755);

    /* Snapdragon 8 Gen 2 (AYN Thor): 3 x A510 @ 2.0 GHz, 2 x A715 + 2 x A710 @ 2.8, 1 x X3 @ 3.2 */
    for (cpu = 0; cpu < 3; cpu++) put(cpu, "cpufreq/cpuinfo_max_freq", 2016000);
    for (cpu = 3; cpu < 7; cpu++) put(cpu, "cpufreq/cpuinfo_max_freq", 2803200);
    put(7, "cpufreq/cpuinfo_max_freq", 3187200);
    m = recomp_big_core_mask(root, 0xFF, &lo, &hi);
    CHECK(m == 0xF8, "8 Gen 2: want cpus 3-7 (0xF8), got %#llx", (unsigned long long)m);
    CHECK(lo == 2016000 && hi == 3187200, "8 Gen 2 maxima: %lu %lu", lo, hi);

    /* the guest's one core (RECOMP_GUEST_CPUS=one, WP-E): the prime core, not
     * the lowest fast one; with capacities alongside, the maxima decide */
    put(0, "cpu_capacity", 280); put(1, "cpu_capacity", 280); put(2, "cpu_capacity", 280);
    for (cpu = 3; cpu < 7; cpu++) put(cpu, "cpu_capacity", 855);
    put(7, "cpu_capacity", 1024);
    {
        unsigned long v;
        int cap = -1, c;
        c = recomp_fastest_core(root, 0xF8, &v, &cap);        /* the Thor's runtime: cpus 3-7 */
        CHECK(c == 7 && v == 3187200 && cap == 0, "fastest of 3-7: want cpu 7 at 3187200, got %d at %lu (cap %d)", c, v, cap);
        c = recomp_fastest_core(root, 0xFF, &v, &cap);
        CHECK(c == 7, "fastest of 0-7: want cpu 7, got %d", c);
        c = recomp_fastest_core(root, 0x78, &v, &cap);        /* cpus 3-6 alike: the lowest */
        CHECK(c == 3 && v == 2803200, "tie among 3-6: want cpu 3, got %d at %lu", c, v);
        c = recomp_fastest_core(root, 0x50, &v, &cap);        /* cpus 4 and 6 */
        CHECK(c == 4, "tie of 4 and 6: want cpu 4, got %d", c);
        c = recomp_fastest_core(root, 0x07, &v, &cap);        /* the slow cluster only */
        CHECK(c == 0 && v == 2016000, "tie among 0-2: want cpu 0, got %d", c);
        c = recomp_fastest_core(root, 0, &v, &cap);
        CHECK(c == -1, "nothing allowed: want -1, got %d", c);
        /* NULL root: the one set for tests */
        recomp_host_cpus_set_sysfs_root(root);
        c = recomp_fastest_core(NULL, 0xF8, &v, &cap);
        CHECK(c == 7, "set root: want cpu 7, got %d", c);
        recomp_host_cpus_set_sysfs_root(NULL);
    }

    /* only some cores allowed: the slow cluster is the slowest *allowed* one */
    m = recomp_big_core_mask(root, 0x78, &lo, &hi);         /* cpus 3-6: all alike */
    CHECK(m == 0x78, "cpus 3-6 alike: want them all, got %#llx", (unsigned long long)m);
    m = recomp_big_core_mask(root, 0x88, &lo, &hi);         /* cpu 3 and cpu 7 */
    CHECK(m == 0x80, "cpus 3 and 7: want cpu 7, got %#llx", (unsigned long long)m);

    /* every core alike: the mask stays as it was */
    clean(); mkdir(root, 0755);
    for (cpu = 0; cpu < 4; cpu++) put(cpu, "cpufreq/cpuinfo_max_freq", 3000000);
    m = recomp_big_core_mask(root, 0x0F, &lo, &hi);
    CHECK(m == 0x0F && lo == 0 && hi == 0, "alike: want 0xF and no maxima, got %#llx", (unsigned long long)m);

    /* no cpufreq (some VMs): cpu_capacity instead */
    clean(); mkdir(root, 0755);
    put(0, "cpu_capacity", 400); put(1, "cpu_capacity", 400); put(2, "cpu_capacity", 1024); put(3, "cpu_capacity", 1024);
    m = recomp_big_core_mask(root, 0x0F, &lo, &hi);
    CHECK(m == 0x0C, "cpu_capacity: want cpus 2-3, got %#llx", (unsigned long long)m);
    {
        /* no cpufreq: the guest's core by capacity, the lowest of the biggest */
        unsigned long v;
        int cap = -1, c = recomp_fastest_core(root, 0x0F, &v, &cap);
        CHECK(c == 2 && v == 1024 && cap == 1, "capacity: want cpu 2 at 1024, got %d at %lu (cap %d)", c, v, cap);
    }

    /* nothing readable: unchanged */
    clean(); mkdir(root, 0755);
    m = recomp_big_core_mask(root, 0x0F, &lo, &hi);
    CHECK(m == 0x0F, "empty tree: want 0xF, got %#llx", (unsigned long long)m);
    {
        /* nothing readable: the lowest allowed core, speed unknown */
        unsigned long v = 1;
        int cap = -1, c = recomp_fastest_core(root, 0x0C, &v, &cap);
        CHECK(c == 2 && v == 0 && cap == 0, "empty tree: want cpu 2 at 0, got %d at %lu", c, v);
    }

    clean();
    /* recomp_host_cpus_init on this host: off unless RECOMP_BIG_CORES=1 outside Android */
#if !defined(__ANDROID__)
    unsetenv("RECOMP_BIG_CORES");
    CHECK(recomp_host_cpus_init() == 0, "off by default outside Android");
#endif
#if defined(__linux__)
    {
        /* XBA-50: a tracked thread whose mask Android widened (a cpuset move
         * on going to the background) gets its own back on resume. */
        cpu_set_t set, all;
        int first = -1, n = 0;
        sched_getaffinity(0, sizeof all, &all);
        for (cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; cpu++)
            if (CPU_ISSET(cpu, &all)) { if (first < 0) first = cpu; n++; }
        if (n >= 2) {
            CPU_ZERO(&set);
            CPU_SET(first, &set);
            sched_setaffinity(0, sizeof set, &set);
            recomp_host_cpus_track(0);                  /* intended: that one core */
            CHECK(recomp_host_cpus_restore() == 0, "nothing drifted yet");
            sched_setaffinity(0, sizeof all, &all);     /* the "cpuset move" */
            CHECK(recomp_host_cpus_restore() == 1, "one thread put back");
            sched_getaffinity(0, sizeof set, &set);
            CHECK(CPU_COUNT(&set) == 1 && CPU_ISSET(first, &set), "back on cpu %d", first);
            recomp_host_cpus_untrack();                 /* an ending thread: no longer moved */
            sched_setaffinity(0, sizeof all, &all);
            CHECK(recomp_host_cpus_restore() == 0, "untracked: left alone");
        } else {
            puts("one core only: the restore check is skipped");
        }
    }
#endif
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    puts("PASS: host cpus");
    return 0;
}
