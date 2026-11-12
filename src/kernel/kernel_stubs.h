/*
 * kernel_stubs.h - kernel routes that are still stubs, and their report (XBA-115).
 *
 * A stub route returns a constant and does nothing else, so a title that
 * depends on it goes quiet or hangs with nothing in the log to say why. The
 * table of them (kernel_stub_table.c) is generated from kernel_bridge.c by
 * tools/kernel_audit/stubs.py and keyed by ordinal, so no wrapper is touched.
 *
 * kernel_thunk_dispatch calls xbox_kernel_stub_note for every call of an
 * ordinal in the table, and for an ordinal with no bridge at all. The first
 * call of each, then the 10th, 100th, 1000th ..., prints
 *     [KERNEL] STUB ordinal 136 (KeRemoveQueue) called from 0x00012345 (call #1)
 * (the K-10 rate limit), and the first is written to the discovery file as
 * {"t":"stub","ord":136,"name":"KeRemoveQueue","ret":"0x00012345"}, which
 * tools.compat names as a blocker.
 */
#ifndef KERNEL_STUBS_H
#define KERNEL_STUBS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t ordinal;
    const char *name;
} KernelStubEntry;

extern const KernelStubEntry g_kernel_stub_table[];
extern const unsigned g_kernel_stub_count;

/* 1 when `ordinal` is in the table (a flag lookup after the first call). */
int xbox_kernel_is_stub(uint32_t ordinal);

/* The table's name for `ordinal`, or NULL when it is not a stub. */
const char *xbox_kernel_stub_name(uint32_t ordinal);

/* A call of a stub route (or of an ordinal with no bridge: has_bridge 0);
 * ret_va is the guest call site. Cheap after the first call. */
void xbox_kernel_stub_note(uint32_t ordinal, int has_bridge, uint32_t ret_va);

/* Calls seen so far of `ordinal` through xbox_kernel_stub_note (tests). */
uint32_t xbox_kernel_stub_calls(uint32_t ordinal);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_STUBS_H */
