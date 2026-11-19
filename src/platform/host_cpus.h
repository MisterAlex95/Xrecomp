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

/* The fastest core of `allowed`, for the guest threads' one-core pin
 * (RECOMP_GUEST_CPUS=one, kernel_thread.c): the one with the highest cpufreq
 * maximum, or, when no allowed core has one, the highest cpu_capacity; the
 * lowest index on a tie. On the AYN Thor (cpus 3-7 allowed: four A7xx at
 * 2.8 GHz, one X3 at 3.2 GHz) that is cpu 7. `root` as above; NULL is the
 * real tree, or the one recomp_host_cpus_set_sysfs_root gave. *value gets
 * the winning maximum in kHz (or its capacity, with *by_capacity = 1); 0
 * when nothing could be read, and then the lowest allowed core is returned.
 * -1 when `allowed` is empty. */
int recomp_fastest_core(const char *root, uint64_t allowed,
                        unsigned long *value, int *by_capacity);

/* Tests: the sysfs cpu tree NULL roots stand for (NULL = the real one,
 * "/sys/devices/system/cpu"). */
void recomp_host_cpus_set_sysfs_root(const char *root);

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

/* The calling thread is ending: forget it, so its tid, once the kernel
 * hands it to another thread (ART's, a binder's), is never moved. */
void recomp_host_cpus_untrack(void);

/* Set every tracked thread that still exists back to its mask. Returns how
 * many had drifted (and prints one line when any had). */
int recomp_host_cpus_restore(void);

/* The calling thread off the cores of `avoid` (XBA-305: the Xbox's floating threads off the guest's core while the
 * GL back end draws): its mask less those cores, set and tracked as above. Nothing when that leaves no core or
 * changes nothing. Returns the new mask, 0 when unchanged. The mask it had is kept for
 * recomp_host_cpus_avoid_undo. Linux and Android; elsewhere it does nothing. */
uint64_t recomp_host_cpus_avoid(uint64_t avoid);

/* The calling thread back on the mask recomp_host_cpus_avoid took it from (the Xbox executor before it starts the
 * raster pool, whose threads inherit its mask). Returns 1 when it was moved back. */
int recomp_host_cpus_avoid_undo(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_HOST_CPUS_H */
