/* recomp_big_core_mask (XBA-25): the fast cores of a big.LITTLE host, read
 * from a fake /sys/devices/system/cpu tree. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "host_cpus.h"

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

    /* nothing readable: unchanged */
    clean(); mkdir(root, 0755);
    m = recomp_big_core_mask(root, 0x0F, &lo, &hi);
    CHECK(m == 0x0F, "empty tree: want 0xF, got %#llx", (unsigned long long)m);

    clean();
    /* recomp_host_cpus_init on this host: off unless RECOMP_BIG_CORES=1 outside Android */
#if !defined(__ANDROID__)
    unsetenv("RECOMP_BIG_CORES");
    CHECK(recomp_host_cpus_init() == 0, "off by default outside Android");
#endif
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    puts("PASS: host cpus");
    return 0;
}
