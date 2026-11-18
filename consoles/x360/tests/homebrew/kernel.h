/* kernel.h -- the xboxkrnl.exe exports our test titles call (XBA-270), declared by hand from their documented
 * signatures (Windows NT / Xbox kernel APIs). x360.ld places each name on its import thunk; elf2xex.py gives the
 * thunks their ordinals. 32-bit arguments only: devkitPPC's 32-bit EABI with -mpowerpc64 passes 64-bit values in
 * register pairs, the console's convention in one 64-bit register, so 64-bit values travel through pointers. */
#ifndef X360_TEST_KERNEL_H
#define X360_TEST_KERNEL_H

typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef u32 HANDLE;
typedef s32 NTSTATUS;

typedef struct {
    u8 header[16];
    s32 lock_count;
    u32 recursion;
    u32 owner;
} CRITICAL_SECTION;

typedef struct {
    s32 hi;
    u32 lo;
} LARGE_TIME; /* a 64-bit time in 100 ns units, big-endian halves (negative: relative) */

int DbgPrint(const char *fmt, ...);
NTSTATUS NtAllocateVirtualMemory(u32 *base, u32 *size, u32 type, u32 protect, u32 unused);
NTSTATUS NtFreeVirtualMemory(u32 *base, u32 *size, u32 type, u32 unused);
void RtlInitializeCriticalSection(CRITICAL_SECTION *cs);
void RtlEnterCriticalSection(CRITICAL_SECTION *cs);
void RtlLeaveCriticalSection(CRITICAL_SECTION *cs);
NTSTATUS NtCreateEvent(HANDLE *h, void *attributes, u32 type, u32 initial);
NTSTATUS NtSetEvent(HANDLE h, s32 *previous);
NTSTATUS NtWaitForSingleObjectEx(HANDLE h, u32 mode, u32 alertable, LARGE_TIME *timeout);
NTSTATUS NtWaitForMultipleObjectsEx(u32 count, HANDLE *handles, u32 wait_type, u32 mode, u32 alertable,
                                    LARGE_TIME *timeout);
NTSTATUS ExCreateThread(HANDLE *h, u32 stack, u32 *id, void *startup, u32 (*start)(void *), void *ctx, u32 flags);
NTSTATUS NtClose(HANDLE h);
NTSTATUS KeDelayExecutionThread(u32 mode, u32 alertable, LARGE_TIME *interval);
NTSTATUS NtCreateSemaphore(HANDLE *h, void *attributes, s32 initial, s32 maximum);
NTSTATUS NtReleaseSemaphore(HANDLE h, s32 count, s32 *previous);
void *MmAllocatePhysicalMemoryEx(u32 flags, u32 size, u32 protect, u32 lowest, u32 highest, u32 alignment);
u32 KeTlsAlloc(void);
u32 KeTlsSetValue(u32 slot, u32 value);
u32 KeTlsGetValue(u32 slot);
int sprintf(char *out, const char *fmt, ...);
/* XBA-296 (homebrew/kobjects) */
void *InterlockedPushEntrySList(void *header, void *entry);
void *InterlockedPopEntrySList(void *header);
void ExInitializeReadWriteLock(void *lock);
void ExAcquireReadWriteLockExclusive(void *lock);
void ExAcquireReadWriteLockShared(void *lock);
void ExReleaseReadWriteLock(void *lock);
void KeInitializeQueue(void *queue, u32 count);
s32 KeInsertQueue(void *queue, void *entry);
void *KeRemoveQueue(void *queue, u32 mode, LARGE_TIME *timeout);
NTSTATUS NtCreateIoCompletion(HANDLE *h, u32 access, void *attributes, u32 threads);
NTSTATUS NtSetIoCompletion(HANDLE h, u32 key, u32 context, u32 status, u32 information);
NTSTATUS NtRemoveIoCompletion(HANDLE h, u32 *key, u32 *context, u32 *iosb, LARGE_TIME *timeout);
void KeInitializeDpc(void *dpc, void (*routine)(void *, void *, void *, void *), void *context);
u32 KeInsertQueueDpc(void *dpc, void *arg1, void *arg2);
void KeInitializeEvent(void *event, u32 type, u32 state);
s32 KeSetEvent(void *event, u32 increment, u32 wait);
NTSTATUS KeWaitForSingleObject(void *object, u32 reason, u32 mode, u32 alertable, LARGE_TIME *timeout);
NTSTATUS NtQueueApcThread(HANDLE thread, void (*routine)(void *, void *, void *), void *context, void *arg1, void *arg2);
NTSTATUS NtCreateTimer(HANDLE *h, void *attributes, u32 type);
NTSTATUS NtSetTimerEx(HANDLE h, LARGE_TIME *due, void *apc, u32 mode, void *context, u32 resume, u32 period_ms, u32 unused);
NTSTATUS NtCancelTimer(HANDLE h, u32 *state);

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define MEM_LARGE_PAGES 0x20000000
#define PAGE_READWRITE 4

#endif
