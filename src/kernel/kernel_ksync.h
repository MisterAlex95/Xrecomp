/*
 * kernel_ksync.h - SLists, kernel queues, device queues, read-write locks and
 * the APC queue, kept in guest memory with the console's layout (XBA-113).
 *
 * Every structure here lives where the title put it, laid out as in nxdk's
 * xboxkrnl.h, so lifted XAPI or title code that reads a field directly sees
 * the value the console would show. All arguments are guest addresses.
 *
 * SLists use a real 64-bit compare-and-swap on the header (Next, Depth,
 * Sequence), as the console's cmpxchg8b does, so they stay lock-free and
 * agree with a title's own inlined `lock cmpxchg8b` on the same header. The
 * other structures are guarded by one host lock; a blocking call waits on a
 * host condition variable with that lock released, and is made with the
 * guest CPU let go (kernel_thunk_dispatch).
 */
#ifndef KERNEL_KSYNC_H
#define KERNEL_KSYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* InterlockedPushEntrySList / PopEntrySList / FlushSList (56-58). Push and
 * Pop return the previous first entry and the removed one (0 when empty);
 * Flush returns the old chain. */
uint32_t ks_slist_push(uint32_t head, uint32_t entry);
uint32_t ks_slist_pop(uint32_t head);
uint32_t ks_slist_flush(uint32_t head);

/* KQUEUE (111, 116, 117, 136, 141). Insert returns the queue's previous
 * SignalState. Remove returns the entry, or 0x102 (STATUS_TIMEOUT) when the
 * timeout (a guest PLARGE_INTEGER, 0 = infinite) ran out. */
void     ks_queue_init(uint32_t q, uint32_t count);
int32_t  ks_queue_insert(uint32_t q, uint32_t entry, int at_head);
uint32_t ks_queue_remove(uint32_t q, uint32_t timeout_va);
uint32_t ks_queue_rundown(uint32_t q);

/* I/O completion objects (NtCreateIoCompletion 191, NtSetIoCompletion 227,
 * NtRemoveIoCompletion 223, NtQueryIoCompletion 212; XBA-114), host-side.
 * `ident` is the host HANDLE behind the title's token, a manual-reset event
 * signalled while packets are queued. Create returns 0 when the table is
 * full. Post returns 0, or -1 for a handle that is no port. Remove returns
 * 0 with out[] = key, ApcContext, status, information, or STATUS_TIMEOUT,
 * or STATUS_INVALID_HANDLE. Depth is -1 for no port. A file joins a port
 * with associate (FileCompletionInformation); post_for_file posts a request
 * of that file's (1) or does nothing (0). forget drops a closed handle,
 * port or file. */
int      ks_port_create(void *ident, uint32_t count);
int      ks_port_is(void *ident);
int      ks_port_post(void *ident, uint32_t key, uint32_t ctx, uint32_t status,
                      uint32_t info);
uint32_t ks_port_remove(void *ident, uint32_t timeout_va, uint32_t out[4]);
int32_t  ks_port_depth(void *ident);
int      ks_port_associate(void *file, void *port, uint32_t key);
int      ks_port_post_for_file(void *file, uint32_t ctx, uint32_t status,
                               uint32_t info);
void     ks_port_forget(void *handle);

/* KDEVICE_QUEUE (106, 114, 115, 133, 134, 135). */
void     ks_devq_init(uint32_t dq);
int      ks_devq_insert(uint32_t dq, uint32_t entry);
int      ks_devq_insert_by_key(uint32_t dq, uint32_t entry, uint32_t key);
uint32_t ks_devq_remove(uint32_t dq);
uint32_t ks_devq_remove_by_key(uint32_t dq, uint32_t key);
int      ks_devq_remove_entry(uint32_t dq, uint32_t entry);

/* ERWLOCK (12, 13, 18, 28). */
void ks_rwlock_init(uint32_t lock);
void ks_rwlock_acquire_exclusive(uint32_t lock);
void ks_rwlock_acquire_shared(uint32_t lock);
void ks_rwlock_release(uint32_t lock);

/* KAPC (105, 118). Insert queues the APC on its thread's ApcState list for
 * its mode and returns 1, or 0 when it was already queued or names no
 * thread. Take removes the first APC of that mode queued on `kthread`, or
 * returns 0. ks_apc_pending is the number of APCs queued anywhere, read
 * without the lock as a cheap gate. */
void     ks_apc_init(uint32_t apc, uint32_t thread, uint32_t kernel_routine,
                     uint32_t rundown_routine, uint32_t normal_routine,
                     uint32_t mode, uint32_t normal_context);
int      ks_apc_insert(uint32_t apc, uint32_t arg1, uint32_t arg2);
uint32_t ks_apc_take(uint32_t kthread, int mode);
int      ks_apc_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_KSYNC_H */
