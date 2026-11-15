/* The runtime's risky pieces, run where they will really run (XBA-51).
 *
 * The device test bundle (XBA-19) runs from adb shell: SELinux domain shell,
 * no ART, no libsigchain, an almost empty address space. An app runs as
 * untrusted_app under ART, with libsigchain in front of sigaction and
 * hundreds of libraries mapped, so a pass in the bundle does not prove the
 * app. recomp_selftest_run() runs the same checks in whatever process calls
 * it: the app shell calls it when recomp.cfg says RECOMP_SHELL_SELFTEST=1
 * (android/app/src/main/cpp/shell_main.c), tests/recomp_selftest calls it on
 * Linux (x86-64, and arm64 under qemu-user) so that a device failure is an
 * Android difference.
 *
 * One line per check on stdout (logcat in the app):
 *     selftest NAME: PASS [(note)]
 *     selftest NAME: FAIL (reason)
 *     selftest NAME: SKIP (RECOMP_SELFTEST_SKIP)
 * then
 *     selftest: N passed, M failed[, K skipped]
 *
 * The checks, in order:
 *   layout       xbox_MemoryLayoutInit on the synthetic XBE (tools/conformance,
 *                ours, no game content); the base is host-page aligned
 *   window       when the 4 GB guest window is held (RECOMP_GUEST_WINDOW, the
 *                default on 64-bit hosts), /proc/self/maps covers all of it;
 *                on a 64-bit host, not holding it (the 4 GB reservation
 *                refused) fails
 *   mirrors      a write and a read through each of the 28 RAM mirrors
 *                (memfd views), seen at the base, and the other way round
 *   tiled_alias  the same through the tiled aperture and the contiguous
 *                window, two views of the other memfd
 *   mmio_handler xbox_PosixInstallMmioFaultHandler still in front, and a
 *                trapped page of guest RAM (xbox_PosixMmioSetTestWindow)
 *   mmio_*       one trapped access of each decoder class through that page,
 *                by the real chain (libsigchain, the toolkit's handler, the
 *                A64 or x86-64 decoder, the register write-back into the
 *                signal frame); mmio_simd is the SIMD load / store (ldr q /
 *                movups): on arm64 the V register in the fpsimd_context
 *                and, when live, Zn in the sve_context (XBA-45, XBA-189)
 *   exec_alloc   xbox_NtAllocateVirtualMemory with PAGE_EXECUTE_READWRITE
 *                succeeds, read / write, and the mapping is not executable
 *   exec_guest   xbox_MmSetAddressProtect with PAGE_EXECUTE_READ and
 *                PAGE_EXECUTE_READWRITE on guest RAM (a memfd view) takes
 *                effect without PROT_EXEC
 *   reserve      a 608 MB reservation (xbox_ReserveAlloc) served lazily from
 *                the held window: writable at both ends, not an alias of low
 *                RAM, released pages read as zero (XBA-103)
 *   save_dir     a file created, written, synced, read back and removed in
 *                the save directory
 *   reinit       shutdown, a second init and shutdown again: the title maps
 *                its own layout after this in the same process
 *
 * A probe never faults: whether an address is mapped is asked of the kernel
 * (a pipe write / read returns EFAULT), so an unmapped mirror is a FAIL line,
 * not a crash. A trapped access the decoder cannot service is a crash, as it
 * would be in a title: the PASS lines before it (the fault handler drains
 * logcat first) say which check it was.
 *
 * RECOMP_SELFTEST_SKIP: names to skip, separated by commas or spaces.
 *
 * POSIX hosts. Leaves the memory layout shut down. */
#ifndef XBOXRECOMP_RECOMP_SELFTEST_H
#define XBOXRECOMP_RECOMP_SELFTEST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* xbe / xbe_len: the synthetic XBE's bytes (NULL fails every check that needs
 * the layout). save_dir: an existing, writable directory (NULL fails
 * save_dir). The memory layout must not be initialised yet. Returns the
 * number of failed checks. */
int recomp_selftest_run(const void *xbe, size_t xbe_len, const char *save_dir);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_SELFTEST_H */
