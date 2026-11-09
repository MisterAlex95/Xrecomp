/* Which host cores the runtime runs on (XBA-25).
 *
 * Phones mix fast and slow cores (big.LITTLE). Left alone, the scheduler may
 * put the guest's main thread or the rasteriser on a little core, which runs
 * lifted code several times slower. recomp_host_cpus_init(), called first
 * thing in main, restricts the calling thread to the fast cores; every thread
 * created afterwards (guest threads, the rasteriser pool, SDL's) inherits
 * that mask, and Win32 CreateThread's "process mask" is taken from it.
 *
 * The fast cores are those whose cpufreq maximum is above the slowest
 * cluster's. RECOMP_BIG_CORES=1 turns this on, =0 off; unset, it is on for
 * Android and off elsewhere (desktop cores are alike, or the OS knows best). */
#ifndef XBOXRECOMP_HOST_CPUS_H
#define XBOXRECOMP_HOST_CPUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Applies the policy above and prints one line saying what it did. Returns
 * the mask of the cores the thread now runs on (bit n = cpu n), or 0 when it
 * left the affinity alone. */
uint64_t recomp_host_cpus_init(void);

/* The fast-core mask computed from a sysfs tree rooted at `root`
 * ("/sys/devices/system/cpu" on a real host; a test passes a fake one), among
 * the cores in `allowed`. Returns `allowed` itself when every core has the
 * same maximum or nothing can be read. *slow_khz / *fast_khz receive the
 * slowest and fastest maxima seen (0 if unknown). */
uint64_t recomp_big_core_mask(const char *root, uint64_t allowed,
                              unsigned long *slow_khz, unsigned long *fast_khz);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_HOST_CPUS_H */
