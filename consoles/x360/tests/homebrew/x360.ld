/* x360.ld -- links a test title for consoles/x360/tests/homebrew/elf2xex.py (XBA-270): one blob from the start of
 * the synthetic XEX's .text (tools/xex/synth.py: base 0x82000000 + 0x1000), code then read-only data then data and
 * bss (zeros in the file), and the import thunks right after it, 16 bytes each, in the order of elf2xex.py's
 * KERNEL list (synth.py appends them there). */
ENTRY(_start)
SECTIONS
{
    . = 0x82001000;
    .text : { KEEP(*(.text.start)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) *(.sdata2 .sdata2.*) *(.sbss2 .sbss2.*) }
    .data : { *(.data .data.*) *(.sdata .sdata.*) *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) . = ALIGN(4); }
    __thunks = .;
    DbgPrint = __thunks + 0 * 16;
    NtAllocateVirtualMemory = __thunks + 1 * 16;
    NtFreeVirtualMemory = __thunks + 2 * 16;
    RtlInitializeCriticalSection = __thunks + 3 * 16;
    RtlEnterCriticalSection = __thunks + 4 * 16;
    RtlLeaveCriticalSection = __thunks + 5 * 16;
    NtCreateEvent = __thunks + 6 * 16;
    NtSetEvent = __thunks + 7 * 16;
    NtWaitForSingleObjectEx = __thunks + 8 * 16;
    NtWaitForMultipleObjectsEx = __thunks + 9 * 16;
    ExCreateThread = __thunks + 10 * 16;
    NtClose = __thunks + 11 * 16;
    KeDelayExecutionThread = __thunks + 12 * 16;
    NtCreateSemaphore = __thunks + 13 * 16;
    NtReleaseSemaphore = __thunks + 14 * 16;
    MmAllocatePhysicalMemoryEx = __thunks + 15 * 16;
    KeTlsAlloc = __thunks + 16 * 16;
    KeTlsSetValue = __thunks + 17 * 16;
    KeTlsGetValue = __thunks + 18 * 16;
    sprintf = __thunks + 19 * 16;
    InterlockedPushEntrySList = __thunks + 20 * 16;
    InterlockedPopEntrySList = __thunks + 21 * 16;
    ExInitializeReadWriteLock = __thunks + 22 * 16;
    ExAcquireReadWriteLockExclusive = __thunks + 23 * 16;
    ExAcquireReadWriteLockShared = __thunks + 24 * 16;
    ExReleaseReadWriteLock = __thunks + 25 * 16;
    KeInitializeQueue = __thunks + 26 * 16;
    KeInsertQueue = __thunks + 27 * 16;
    KeRemoveQueue = __thunks + 28 * 16;
    NtCreateIoCompletion = __thunks + 29 * 16;
    NtSetIoCompletion = __thunks + 30 * 16;
    NtRemoveIoCompletion = __thunks + 31 * 16;
    KeInitializeDpc = __thunks + 32 * 16;
    KeInsertQueueDpc = __thunks + 33 * 16;
    KeInitializeEvent = __thunks + 34 * 16;
    KeSetEvent = __thunks + 35 * 16;
    KeWaitForSingleObject = __thunks + 36 * 16;
    NtQueueApcThread = __thunks + 37 * 16;
    NtCreateTimer = __thunks + 38 * 16;
    NtSetTimerEx = __thunks + 39 * 16;
    NtCancelTimer = __thunks + 40 * 16;
    /DISCARD/ : { *(.comment) *(.eh_frame) *(.gnu.attributes) }
}
