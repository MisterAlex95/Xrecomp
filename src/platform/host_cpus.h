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

/* The runtime's threads and the cores each should run on (XBA-50).
 *
 * Android moves an app between cpusets as it goes to the background and
 * back; on kernels before 6.2 (the AYN Thor runs 5.15) the move overwrites
 * the mask a thread set for itself and does not put it back, so after a
 * notification the game ran on the little cores. Each runtime thread notes
 * its intended mask here (the shim's CreateThread, the raster pool, the
 * guest threads' one-core pin, the main thread after recomp_host_cpus_init);
 * recomp_lifecycle_resume calls recomp_host_cpus_restore, which sets them
 * again. Threads that are not ours (ART, binder, SDL's) are never touched.
 * Linux and Android; elsewhere both do nothing. */

/* The calling thread's intended mask; 0 = its affinity right now. A second
 * call from the same thread replaces the first. */
void recomp_host_cpus_track(uint64_t mask);

/* Set every tracked thread that still exists back to its mask. Returns how
 * many had drifted (and prints one line when any had). */
int recomp_host_cpus_restore(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_HOST_CPUS_H */
