#pragma once
/* Page tracker for guest address space above the RAM mirrors.
 *
 * The heap (xbox_HeapAlloc, xbox_ReserveAlloc) hands out addresses inside the
 * mapped range and never honours a caller-supplied base: it gives whatever its
 * cursor is at. That is fine for a title that passes base = NULL, but one that
 * reserves a specific high address and checks it got that address back needs a
 * real answer -- the address, or a failure status its own retry logic can act
 * on -- not a different address substituted in.
 *
 * This tracks state per 4 KiB page from the top of the RAM mirrors to the top
 * of user address space, backed by host memory reserved when the layout comes
 * up and committed only where a title commits.
 *
 * Opt-in, RECOMP_EXT_VMA=1. Without it nothing here is initialised and every
 * function answers "not mine" (returns 0), so the bridge behaves as before.
 *
 * Everything below g_memory_size (RAM, or the mapped size when a title asked
 * for one) is not this file's business. What lies between there and the top of
 * the mirrors aliases low memory, and is refused or reported reserved.
 */
#include <stddef.h>
#include <stdint.h>

/* End of user address space, rounded down to a 64 KiB allocation granule. Real
 * Xbox user mode ends at 0x7FFEFFFF; the kernel and system reserve the rest. */
#define GUEST_VMEM_TOP 0x7FFE0000u

/* Called by xbox_MemoryLayoutInit once the mirrors are mapped. `offset` is
 * host address minus guest address; the mirrors occupy [mirror_lo, mirror_top).
 * Does nothing unless RECOMP_EXT_VMA is set, or when the mirrors already reach
 * the top of user space (a 128 MB map), where there is nothing above them to
 * track. Returns 1 when the tracker is live. */
int guest_vmem_init(ptrdiff_t offset, uint64_t mirror_lo, uint64_t mirror_top);
/* POSIX: the same over a range the caller already holds reserved (`held`
 * bytes at mirror_top), tracked in place and never released here. */
int guest_vmem_adopt(ptrdiff_t offset, uint64_t mirror_lo, uint64_t mirror_top, size_t held);
void guest_vmem_shutdown(void);

/* True once guest_vmem_init has succeeded. */
int guest_vmem_active(void);

/* Each returns 0 when the request is not the tracker's (it is not live, or the
 * address is below the mirrors' start), in which case the caller carries on
 * with its own path. Otherwise they return 1 with *status set. */
int guest_vmem_allocate(uint32_t *base, uint32_t *size, uint32_t alloc_type,
                        uint32_t protect, uint32_t *status);
int guest_vmem_free(uint32_t *base, uint32_t *size, uint32_t free_type,
                    uint32_t *status);
/* info[7] = { BaseAddress, AllocationBase, AllocationProtect, RegionSize,
 *             State, Protect, Type } -- the 28-byte guest
 *             MEMORY_BASIC_INFORMATION layout. */
int guest_vmem_query(uint32_t address, uint32_t info[7]);

/* Save states (XBA-245, xbox_state.h).
 *
 * state_save: the tracker's page table as runs of identical entries (u32
 * active; when active: u32 mirror_lo, lo, top, run count, then per run u32
 * pages, owner, u16 state, u16 protect, u32 reserve_protect), written to out
 * when out is not NULL and cap is large enough; returns its size either way.
 * state_check: 0 when a payload of that shape covers [lo, top) exactly, else
 * -1; *runs the run count (may be NULL).
 * committed_runs: fn for each run of committed pages (the only tracked pages
 * the host backs and that can be read), in address order; returns the runs. */
size_t guest_vmem_state_save(uint8_t *out, size_t cap);
int guest_vmem_state_check(const uint8_t *in, size_t n, uint32_t *runs);
int guest_vmem_committed_runs(void (*fn)(uint32_t va, uint32_t size, uint8_t *host, void *user), void *user);
