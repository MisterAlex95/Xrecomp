/**
 * Xbox Memory Layout Compatibility
 *
 * The Xbox has 64MB of unified memory shared between CPU and GPU.
 * Memory is identity-mapped (physical == virtual for most of it).
 * Game code and data are linked to specific address ranges which vary
 * per game. Section addresses are parsed dynamically from the XBE header
 * at runtime, so this module works with ANY Xbox game.
 *
 * On Windows, we:
 * 1. Create a 64MB file mapping (CreateFileMapping)
 * 2. Map the base view + 28 mirror views at 64MB intervals
 * 3. Parse the XBE section table and copy sections to their Xbox VAs
 * 4. Set up simulated stack, heap, TIB, and kernel data area
 *
 * The mirror views ensure Xbox RAM wrapping works correctly: the Xbox
 * memory controller uses a 26-bit address bus, so ALL addresses wrap
 * modulo 64MB. File mapping views backed by the same section give us
 * true aliases where writes at one address are visible at all mirrors.
 */

#ifndef XBOX_MEMORY_LAYOUT_H
#define XBOX_MEMORY_LAYOUT_H

#include "platform/xbox_winnt.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox memory map constants
 * ================================================================ */

/* Base address of all XBE files in Xbox memory */
#define XBOX_BASE_ADDRESS       0x00010000

/* Start of mapped region - includes low memory (KPCR at 0x0) because
 * game code reads from addresses like 0x20 and 0x28 (Xbox kernel structures). */
#define XBOX_MAP_START          0x00000000

/* Xbox physical memory. 64 MB is the retail default; debug/beta builds ship for
 * 128 MB devkits and allocate accordingly (Halo's cachebeta pre-allocates ~57 MB
 * plus its debug arrays, which only fits on a devkit). Runtime-overridable via
 * xbox_SetTotalRam() before xbox_MemoryLayoutInit(); see g_xbox_total_ram. */
#define XBOX_TOTAL_RAM          (64 * 1024 * 1024)  /* 64 MB (default) */
#define XBOX_DEVKIT_RAM         (128 * 1024 * 1024) /* 128 MB (debug kit) */
#define XBOX_GPU_RESERVED       (4 * 1024 * 1024)   /* ~4 MB for GPU */

/* Actual mapped RAM for this run. Defaults to XBOX_TOTAL_RAM; a title with a
 * devkit build calls xbox_SetTotalRam(XBOX_DEVKIT_RAM) before init. Heap top and
 * mirror stride derive from this, not from the compile-time constant. */
extern size_t g_xbox_total_ram;

/* How much guest address space to map, when that must exceed RAM.
 *
 * These are not the same quantity and conflating them is a bug. RAM is what
 * the console has and what the heap is carved out of; the mapped range is how
 * much guest address space is backed by distinct host pages. The runtime
 * mirrors RAM at intervals of the mapped size, because a real Xbox wraps
 * addresses on a 26-bit bus -- so anything a title allocates above the mapped
 * range silently shares storage with low memory.
 *
 * Half-Life 2 needed this (before the lazy reservations, below): its
 * allocator sub-allocates past the top of RAM, and at 64 MB its first commit
 * past the boundary (0x04F80000) aliases the base of the live heap
 * (0x00F80000). A CUtlRBTree element array landed at 0x0CB80000,
 * aliasing 0x00B80000, and its links were overwritten between one insert and
 * the next search.
 *
 * Raising g_xbox_total_ram instead does not work: the heap top and anything
 * the guest is told about memory derive from that, so the title sizes itself
 * differently and faults during CRT init. Growing only the mapping leaves both
 * alone.
 *
 * Zero means "same as RAM", which is the existing behaviour for every title
 * that does not ask. Set before xbox_MemoryLayoutInit().
 *
 * No title needs it any more on a POSIX host (XBA-103): a reservation the
 * heap cannot back is served lazily from the held guest window
 * (xbox_ReserveAlloc), with no size picked in advance. It stays for Windows,
 * which keeps the old path, and as the A/B against the lazy reservations. */
extern size_t g_xbox_map_size;
void xbox_SetMapSize(size_t bytes);

/* Carve a pure address-space reservation. With a mapping larger than RAM
 * (xbox_SetMapSize) it comes from the mapped range above RAM first. Otherwise,
 * or once that is exhausted, POSIX hosts take it lazily from the top of guest
 * user space down: guest [0x7FFE0000 - n, 0x7FFE0000), 64 KB granules, above
 * the RAM view (or the xbox_SetMapSize mapping), replacing the RAM mirrors
 * there with zero-filled read-write pages the host backs only once touched
 * (RECOMP_LAZY_RESERVE=0 turns that off). Returns 0 when neither path has
 * room. See the implementation for why reservations must not come from the
 * heap. */
uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align);

/* Whether [va, va + bytes) lies inside the lazy reservations handed out since
 * the layout came up: mapped guest memory, though above the RAM view. */
int xbox_ReserveHolds(uint32_t va, uint32_t bytes);

/* NtFreeVirtualMemory for a lazy reservation: release (the whole reservation,
 * va its base) or decommit [va, va + size) (size 0: to its end). The pages go
 * back to the host and read as zero afterwards; the address is not handed out
 * again. Returns 1 with the page-rounded range in *out_base / *out_size, 0 when
 * va is not a live lazy reservation, -1 for a release not at its base. */
int xbox_ReserveFree(uint32_t va, uint32_t size, int release,
                     uint32_t *out_base, uint32_t *out_size);

/* NtAllocateVirtualMemory MEM_RESERVE at an address the title chose, above the
 * RAM view: the range is mapped with fresh zero pages and listed with the lazy
 * reservations (released by xbox_ReserveFree), so the title gets the address
 * it asked for, as on the console. Intrinsic Alchemy's sbrk reserves the run
 * VirtualQuery called free and gives up on any other answer. Returns -1 when
 * the address is not ours (below the floor, the lazy reservations off, or
 * RECOMP_EXT_VMA's tracker on), else an NTSTATUS: 0 with the 64 KB-rounded base
 * and the page-rounded size written back, STATUS_CONFLICTING_ADDRESSES when a
 * live reservation is in the way. */
int xbox_ReserveAt(uint32_t *base, uint32_t *size);

/* NtQueryVirtualMemory above the RAM view, when the lazy reservations are on:
 * a live reservation reads as committed, anything else as free up to the next
 * reservation (or the top of user space), in one region. Fills the 7 guest
 * MEMORY_BASIC_INFORMATION fields and returns 1, or returns 0 when the address
 * is not ours. */
int xbox_ReserveQuery(uint32_t va, uint32_t info[7]);

/* Bounds of the guest's executable sections, derived from the XBE at load.
 *
 * recomp_types.h declares these too, for RECOMP_ICALL_IS_CODE. They are
 * repeated here so hand-written host code -- a fault handler wanting to tell a
 * guest return address on the stack from ordinary data, say -- can use them
 * without including the generated-code header, which redefines `eax` and
 * friends as macros.
 */
/* Full extent of the loaded XBE image -- every section, not just the
 * executable ones. Anything writing guest memory on the title's behalf must
 * stay out of this range. */
extern uint32_t g_xbox_image_lo;
extern uint32_t g_xbox_image_hi;

extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;
void xbox_SetTotalRam(size_t bytes);

/* NOTE: Section addresses (.text, .rdata, .data, etc.) are NOT hardcoded.
 * They are parsed from the XBE header at runtime in xbox_MemoryLayoutInit().
 * This allows the toolkit to work with ANY Xbox game without modification. */

/* ================================================================
 * Memory initialization
 * ================================================================ */

/**
 * Initialize the Xbox memory layout.
 *
 * Reserves the virtual address range 0x00010000 through 0x0076F000
 * and maps the XBE sections to their expected addresses:
 * - .rdata: copied from XBE, read-only
 * - .data: initialized portion copied from XBE, BSS zeroed
 *
 * Note: .text is NOT mapped here - the recompiled code is native
 * Windows code and doesn't need to be at the original address.
 * The data sections DO need to be at their original addresses
 * because the recompiled code references globals by absolute address.
 *
 * @param xbe_data  Pointer to the loaded XBE file contents.
 * @param xbe_size  Size of the XBE file.
 * @return TRUE on success, FALSE on failure.
 */
BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size);

/**
 * Release the reserved Xbox memory layout.
 */
/**
 * Mirror a GPU completion fence the title spins on.
 *
 * The NV2A tables in xbox_memory_layout.c acknowledge handshakes that live at
 * fixed aperture offsets. Some titles instead wait on a semaphore the GPU
 * writes into contiguous memory: D3D seeds it, submits work, then spins until
 * it reaches the submitted count. Wreckless does this at guest 0x000FE920,
 * waiting on the 96-byte MmAllocateContiguousMemoryEx block its device struct
 * points at.
 *
 * Nothing executes the push buffer -- the D3D11 layer draws -- so everything
 * submitted is complete, and advancing the fence is the same honest
 * acknowledgement the register tables make.
 *
 * The fence has no fixed address; it is reached through the title's device
 * struct, so it is registered as the chain of indirections to follow:
 *
 *     device = MEM32(device_ptr_va)
 *     fence  = MEM32(device + get_ptr_off)
 *     MEM32(fence) = MEM32(device + put_off)
 *
 * Every step is bounds-checked each poll, so registering a chain that is not
 * yet initialised (or never becomes valid) is harmless.
 *
 * Returns 0 on success, -1 if the table is full.
 */
/* Advance a frame/swap counter inside the D3D device at ~60 Hz.
 *
 * A title that waits a frame reads the device's swap count and spins until it
 * moves. Nothing here presents, so without this the count never changes and
 * the wait never ends. Followed through the device pointer, like the fence,
 * because the device is allocated at runtime.
 *
 * Returns 0 on success, -1 if the table is full. */
/* Mirror one field of the device struct onto another, for the counter pair a
 * swap throttle waits on: the title bumps "frames submitted" itself and waits
 * for "frames completed", which only the GPU moves on hardware. Unlike
 * xbox_Nv2aFrameCounter this cannot run ahead of the title, so the unsigned
 * submitted - completed it gets compared against cannot underflow. */
int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off);

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off);
/* Advance those counters now, because a swap really completed. Called by the
 * pushbuffer executor on FLIP_STALL; while these arrive the 60 Hz fallback
 * stands down, so the count follows what was actually drawn. A counter the
 * title is seen to move itself is no longer bumped by either. */
void xbox_Nv2aFrameCounterFlip(void);
/* A word of the NV2A register file (offset from the register base); NULL until
 * the aperture is mapped. */
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
/* PVIDEO's STOP handshake on a host where the register page is plain memory
 * (POSIX): with STOP's bit 0 set in `pvideo` (the 4 KB PVIDEO block, NV2A
 * offset 0x8000) BUFFER and STOP are cleared and the stop count goes up;
 * returns whether it did. The NV2A ack pass calls it; not static so tests can
 * drive it against plain words. */
int xbox_Nv2aPvideoStop(volatile uint32_t *pvideo);
/* STOP requests answered so far: the overlay scan-out's sign that the overlay
 * went down (STOP itself reads 0 again by then). */
uint32_t xbox_Nv2aPvideoStops(void);
/* A kick that waits for the GPU's ack, as events rather than a poll: set
 * `mask` in the register (an NV2A word, e.g. the PFB flush kick 0x100410)
 * and return once the NV2A ack thread has cleared it. While the title kicks
 * through this, the ack thread sleeps until a kick instead of polling (see
 * xbox_memory_layout.c). For title overrides of D3D's KickOff wait. */
void xbox_Nv2aKickWait(volatile uint32_t *reg, uint32_t mask);
/* RECOMP_POLL_WAIT's runtime side (templates/runtime/recomp_types.h): lifted
 * code polls `word` for `mask` in a loop with nothing else in it. Blocks on
 * the ack thread's event when the word is the PFB flush kick; returns at
 * once for any other word. 1 when it waited, 0 when it returned at once. */
int xbox_GuestPollWait(volatile uint32_t *word, uint32_t mask, int while_set,
                       uint32_t va);
/* Registered counters the title turned out to move itself (its vblank DPC),
 * which the toolkit has stopped bumping; for the [GPU] report. */
uint32_t xbox_Nv2aFrameCountersOwned(void);

/* Tell the runtime where the display framebuffer is (from AvSetDisplayMode). */
void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch);
/* Read it back: 0 until the title sets a mode. Pitch is optional. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch);

/* Allocate from the contiguous (physical-mirror) arena. Returns a guest VA
 * below 256 MB, or 0 when the arena is exhausted. */
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
/* The same, constrained to physical addresses [phys_lo, phys_hi] (inclusive),
 * as MmAllocateContiguousMemoryEx asks. Alignment is rounded up to a page and
 * to a power of two. */
uint32_t xbox_ContiguousAllocEx(uint32_t size, uint32_t alignment,
                                uint32_t phys_lo, uint32_t phys_hi);
/* Record a block the caller placed itself, so it is not handed out again. */
void xbox_ContiguousClaim(uint32_t va, uint32_t size);
/* Release a block (unless RECOMP_CONTIG_RECLAIM=0); 0 if va is not in the
 * window. */
int xbox_ContiguousFree(uint32_t va);
/* Bytes from va to the end of its block, 0 if va is not allocated. */
uint32_t xbox_ContiguousBlockSize(uint32_t va);
/* High-water mark (never decreases) and current usage, in bytes. */
uint32_t xbox_ContiguousAllocatedBytes(void);
/* One byte per 4 KB page of the window, nonzero while the page is allocated
 * (or claimed). Read without the arena lock; a device model's view. */
const volatile uint8_t *xbox_ContiguousPageMap(void);
/* MmGetPhysicalAddress named this VA; recorded if it is a heap page below
 * 64 MB (the image and stacks are not recorded). */
void xbox_PhysNoteRamName(uint32_t va);
/* One byte per 4 KB physical page below 64 MB, nonzero while the title holds
 * a RAM name for a heap page there (see xbox_PhysNoteRamName); cleared when
 * no live heap block covers the page any more. Read without a lock; a device
 * model's view. */
const volatile uint8_t *xbox_PhysRamNameMap(void);
/* Whether physical byte `pa` lies in a live heap block the title named:
 * the page map refined to the block (see xbox_PhysRamNamedAt). */
int xbox_PhysRamNamedAt(uint32_t pa);
uint32_t xbox_ContiguousInUseBytes(void);

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off);

/* Where the mirrored fence word is (0 if none is registered or it does not
 * resolve into the contiguous window). For the pushbuffer executor. */
uint32_t xbox_Nv2aFenceWordVa(void);

void xbox_MemoryLayoutShutdown(void);

/**
 * Check if an address falls within the Xbox memory map.
 */
BOOL xbox_IsXboxAddress(uintptr_t address);

/**
 * Get the base pointer for direct memory access.
 * Returns NULL if memory layout is not initialized.
 */
void *xbox_GetMemoryBase(void);

/**
 * Host address of the MCPX device aperture (Xbox VA 0xFE800000, 8 MB), or
 * NULL when it is not mapped. Its first 0x30000 bytes (APU registers and VP)
 * may be trapped; the GP/EP DSP memories above them are plain memory.
 */
void *xbox_GetMcpxAperture(void);

/**
 * Get the offset from Xbox VA to actual mapped address.
 * actual_address = xbox_va + offset
 * Returns 0 if memory is mapped at original Xbox addresses (ideal case).
 */
ptrdiff_t xbox_GetMemoryOffset(void);
/* Bytes of guest address space mapped, for bounds-checking a guest pointer
 * that came out of guest memory rather than from this side. */
size_t xbox_GetMappedSize(void);
void xbox_ProtectMirrorsForDebug(void);

/* Trapped hardware registers (APU, AC'97 bus master, USB OHCI, NIC). A title's
 * fault handler calls one of these first and resumes the thread when it
 * returns 1; only 0 is a crash. void * keeps windows.h and ucontext.h out of
 * here.
 *   xbox_VehMmioFault   Win32 VEH: the PEXCEPTION_POINTERS.
 *   xbox_PosixMmioFault POSIX SIGBUS/SIGSEGV handler: its ucontext_t * and
 *                       si_addr. Always 0 on a host with no decoder. */
#if defined(_WIN32)
int xbox_VehMmioFault(void *exception_pointers);
#else
int xbox_PosixMmioFault(void *ucv, uintptr_t fault);
/* Installs a SIGSEGV / SIGBUS handler that services trapped MMIO through
 * xbox_PosixMmioFault and hands every other fault to the handler that was
 * there before (debuggerd and ART on Android), so real crashes keep their
 * tombstone. Call early, before the title runs; call again after anything
 * that may install its own handler (SDL_Init): it checks it is still in
 * front and re-installs itself, chaining to the newcomer. 0 on success. */
int xbox_PosixInstallMmioFaultHandler(void);

/* A trapped window of the caller's own, for the self-test (XBA-51,
 * src/platform/recomp_selftest.c): xbox_PosixMmioFault services a fault at
 * guest [va, va + size) against (dev, rd, wr) with this host's decoder, after
 * the APU, AC'97 and OHCI. The caller makes those pages fault (PAGE_NOACCESS)
 * and puts them back. One window; size 0 removes it. 0 on success, -1 on a
 * host with no decoder (anything but POSIX arm64 and Linux x86-64). */
typedef uint64_t (*xbox_mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*xbox_mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);
int xbox_PosixMmioSetTestWindow(uint32_t va, uint32_t size, void *dev,
                                xbox_mmio_read_fn rd, xbox_mmio_write_fn wr);
#endif

/* 1 while the whole 4 GB guest window is held as one reservation
 * (RECOMP_GUEST_WINDOW, on by default on Android; the Windows placeholder),
 * 0 when only RAM and its mirrors are reserved and the other windows were
 * mapped at fixed addresses that happened to be free. */
int xbox_GuestWindowHeld(void);

/* Dump the guest call stack and abort if the title has not exited within
 * RECOMP_WATCHDOG_SECS seconds. Call from the thread that runs guest code;
 * does nothing unless that variable is set. */
void xbox_WatchdogStart(void);

/* Print the globals named by RECOMP_PEEK, tagged with `label`. No-op when
 * RECOMP_PEEK is unset. Called at a hang and at an early exit. */
void xbox_PeekSample(const char *label);
void xbox_WatchInit(void);

/* ================================================================
 * Xbox stack for recompiled code
 * ================================================================ */

/* ================================================================
 * Kernel data export area
 * ================================================================ */

/** Base VA for kernel data exports (XboxHardwareInfo, XboxKrnlVersion, etc.)
 *  These are kernel exports that are DATA, not functions. The game reads
 *  their thunk entries and dereferences them to access the data. */
/* Runtime, not a constant: the toolkit's own guest-side structures (this one,
 * the fake TIB/TLS data and the main stack) live in a region that starts above
 * the title image -- see g_xbox_region_base below. 0x00740000 for any image
 * that ends below 0x00700000. */
#define XBOX_KERNEL_DATA_BASE   (g_xbox_region_base + XBOX_REGION_KDATA_OFF)
#define XBOX_KERNEL_DATA_SIZE   4096   /* 4 KB - plenty for all data exports */

/* Offsets within the kernel data area */
#define KDATA_HARDWARE_INFO     0x000  /* XBOX_HARDWARE_INFO (8 bytes) */
#define KDATA_KRNL_VERSION      0x010  /* XBOX_KRNL_VERSION (8 bytes) */
#define KDATA_TICK_COUNT        0x020  /* KeTickCount (4 bytes) */
#define KDATA_LAUNCH_DATA_PAGE  0x030  /* LaunchDataPage (4 bytes, pointer) */
#define KDATA_THREAD_OBJ_TYPE   0x040  /* PsThreadObjectType (4 bytes) */
#define KDATA_EVENT_OBJ_TYPE    0x050  /* ExEventObjectType (4 bytes) */
#define KDATA_XE_IMAGE_FILENAME 0x060  /* XeImageFileName (ANSI_STRING) */
#define KDATA_IO_COMPLETION_TYPE 0x070 /* IoCompletionObjectType (4 bytes) */
#define KDATA_IO_DEVICE_TYPE    0x080  /* IoDeviceObjectType (4 bytes) */
/* Object-type exports a title may compare against each other, so each needs a
 * distinct non-zero value rather than a shared placeholder. */
#define KDATA_MUTANT_OBJ_TYPE   0x090  /* ExMutantObjectType (4 bytes) */
#define KDATA_SEMAPHORE_OBJ_TYPE 0x0A0 /* ExSemaphoreObjectType (4 bytes) */
#define KDATA_TIMER_OBJ_TYPE    0x0B0  /* ExTimerObjectType (4 bytes) */
#define KDATA_FILE_OBJ_TYPE     0x0C0  /* IoFileObjectType (4 bytes) */
#define KDATA_TIME_INCREMENT    0x0D0  /* KeTimeIncrement (4 bytes) */
#define KDATA_BOOT_SMC_VIDEO    0x0E0  /* HalBootSMCVideoMode (4 bytes) */
#define KDATA_IDEX_CHANNEL      0x500  /* IDE_CHANNEL_OBJECT (512-byte reserved region) */
#define KDATA_HD_KEY            0x100  /* XboxHDKey (16 bytes) */
#define KDATA_SIGNATURE_KEY     0x110  /* XboxSignatureKey (16 bytes) */
#define KDATA_LAN_KEY           0x120  /* XboxLANKey (16 bytes) */
#define KDATA_ALT_SIGNATURE_KEYS 0x130 /* XboxAlternateSignatureKeys (256 bytes) */
#define KDATA_XE_PUBLIC_KEY     0x300  /* XePublicKeyData (284 bytes) */
/* HAL disk identity strings (ordinals 41/42). Each is an XBOX_ANSI_STRING
 * (Length, MaximumLength, Buffer VA) followed by the string bytes it points to,
 * because HalRandGather dereferences Buffer to read the text as entropy. */
#define KDATA_DISK_MODEL_STR    0x420  /* XBOX_ANSI_STRING (8 bytes) */
#define KDATA_DISK_MODEL_BUF    0x430  /* model text (up to 48 bytes) */
#define KDATA_DISK_SERIAL_STR   0x460  /* XBOX_ANSI_STRING (8 bytes) */
#define KDATA_DISK_SERIAL_BUF   0x470  /* serial text (up to 32 bytes) */
#define KDATA_DISK_CACHE_PARTS  0x4A0  /* HalDiskCachePartitionCount (4 bytes) */
/* XeImageFileName's text. The exported symbol at KDATA_XE_IMAGE_FILENAME is
 * an XBOX_ANSI_STRING, and a title dereferences its Buffer -- the CRT reads
 * it to work out the running image's path. The struct was declared without
 * anything to point at, so Buffer held whatever was in that page. */
#define KDATA_XE_IMAGE_BUF      0x4B0  /* image path text (up to 64 bytes) */

/** Size of the simulated Xbox stack (8 MB).
 *  Increased from 1 MB because failed RECOMP_ICALL indirect calls
 *  can leak stdcall args onto the stack each frame. An 8 MB stack
 *  provides enough headroom for extended gameplay sessions. */

/* Thread-local storage class for the recompiled register set. Must match
 * templates/runtime/recomp_types.h -- a mismatch is a link-time surprise. */
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif

/* SSE register storage, shared with the generated code. Defined in both this
 * header and templates/runtime/recomp_types.h -- a translation unit can end up
 * including both, so the guard keeps that from being a redefinition. Keep the
 * two identical: the generated code and the runtime have to agree on the
 * layout, and nothing else checks. */
#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif

/* The guest CPU: every per-thread register in one thread-local struct.
 *
 * Each register used to be its own thread-local variable (g_eax, g_esp,
 * g_xmm0, ...). On Mach-O every thread-local is reached through its own
 * descriptor and a call to _tlv_get_addr, so a lifted function paid one call
 * per register it touched: 27-33 % of the guest thread's own CPU in Fable.
 * In one struct a function resolves the thread's block once (the address is
 * constant per thread, and clang hoists it across calls), and on ELF hosts a
 * dlopen'd library (Android's libmain.so) pays one TLSDESC call instead of
 * one per register.
 *
 * The old names stay: g_eax is a macro for g_cpu.r_eax, so code that reads or
 * writes registers compiles unchanged. What no longer compiles is a
 * declaration of one of them (`extern RECOMP_TLS uint32_t g_eax;`): delete
 * it, this header declares them all. Taking an address (&g_esp) still gives
 * the calling thread's register.
 *
 * Defined identically in templates/runtime/recomp_types.h and
 * src/kernel/xbox_memory_layout.h (a translation unit can include both, hence
 * the guard). The members are r_eax, r_esp, ... rather than eax, esp: in
 * generated code `ebx` is itself a macro (for g_ebx), so a member called ebx
 * would be rewritten when g_ebx expands. Designated initializers name them
 * (`RecompCpu g_cpu = { .r_fs_base = ... }`). The generated code and the
 * runtime must agree on the layout; tools/recomp/test_cpu_struct.py checks
 * that the two copies match. */
#ifndef RECOMP_CPU_DEFINED
#define RECOMP_CPU_DEFINED
/* The guest frame shadow stack (XBA-247, docs/technical/xbox-save-states.md).
 *
 * Lifted code keeps its return path in host frames and ebp in a C local, so
 * the host frames of a parked thread cannot be read. Every lifted function
 * that may reach a park point (a kernel call or a host wait; the translator's
 * _park_plan) records itself on entry: one RecompFrame, the guest esp it was
 * entered with (it points at its return address) and g_ebp, the ebp its
 * caller published before the call. It drops the frame at every return. So at
 * any park point the thread's whole guest call chain is in memory: a save
 * state reads it (the chain signature) and a load compares it. Functions that
 * can never park, and calls into them, pay nothing.
 *
 * The frames are a heap array per host thread (1024 of them and two spare),
 * allocated the first time the thread enters such a function
 * (recomp_frames_slow); g_cpu keeps only the pointers, so the thread-local
 * block stays the register file and fits any static TLS reserve. Past 1024
 * frames (deep recursion) the frames go to a spare slot, shared and never
 * read: the code stays correct, the chain is unreadable (OVERFLOW). The
 * registers come first and are a RecompCpuRegs (the same members,
 * RECOMP_CPU_REGS): what the park records copy and the state stores. */
#define RECOMP_FRAME_SLOTS 1024
typedef struct RecompFrame {
    uint32_t f_ebp;             /* g_ebp at the function's entry: its caller's published ebp */
    uint32_t f_esp;             /* guest esp at its entry (at its return address); not `esp`:
                                 * generated code has a macro of that name */
} RecompFrame;

#define RECOMP_CPU_REGS \
    uint32_t r_eax, r_ecx, r_edx, r_esp, r_ebx, r_esi, r_edi; \
    uint32_t r_ebp;             /* last frame published by `mov ebp, esp` */ \
    uint32_t r_seh_ebp;         /* SEH prolog/epilog frame bridge */ \
    uint32_t r_fs_base;         /* linear base of fs: (this thread's TIB) */ \
    uint32_t r_eflags;          /* EFLAGS image: pushfd, popfd, calls */ \
    uint32_t r_mxcsr;           /* SSE control/status */ \
    int r_df;                   /* EFLAGS.DF */ \
    int r_fp_top;               /* x87 stack top */ \
    int r_fp_cmp; \
    uint16_t r_fp_control_word; \
    uint16_t r_fp_cc;           /* x87 status word condition bits */ \
    double r_fp_stack[8]; \
    RecompXmm r_xmm0, r_xmm1, r_xmm2, r_xmm3, r_xmm4, r_xmm5, r_xmm6, r_xmm7; \
    RecompMmx r_mm0, r_mm1, r_mm2, r_mm3, r_mm4, r_mm5, r_mm6, r_mm7;

typedef struct RecompCpuRegs {
    RECOMP_CPU_REGS
} RecompCpuRegs;

typedef struct RecompCpu {
    RECOMP_CPU_REGS
    RecompFrame *r_ftop;        /* the next frame (XBA-247); NULL until the first */
    RecompFrame *r_fbase;       /* the thread's frames, RECOMP_FRAME_SLOTS + 2 */
    RecompFrame *r_flimit;      /* r_fbase + RECOMP_FRAME_SLOTS; r_fbase while resuming (XBA-250) */
    void *r_resume;             /* the thread's resume plan while it re-enters its chain (XBA-250), else NULL */
} RecompCpu;

/* What the lifter emits in a function that may park (XBA-247):
 *
 *   void sub_X(void)
 *   {
 *       RECOMP_FRAME_ENTRY();
 *       ...
 *       RECOMP_FRAME_LEAVE(); return;     (at every return, and at the end)
 *   }
 *
 * ENTRY: one compare on the way in (the array exists and has room), the
 * frame's two words and the top bumped; LEAVE: the top put back. Nothing per
 * call. A longjmp or an SEH resume over frames puts the top back too
 * (xbox_SehRuntimeSnapshot). -DRECOMP_FRAME_SHADOW=0 compiles them out: a
 * measurement switch for tools/bench, not a build option (no chains then). */
#if defined(__GNUC__) || defined(__clang__)
#define RECOMP_FRAME_COLD inline __attribute__((cold))
#define RECOMP_FRAME_RARE(x) __builtin_expect(!!(x), 0)
#else
#define RECOMP_FRAME_COLD __inline
#define RECOMP_FRAME_RARE(x) (x)
#endif
void *calloc(size_t, size_t);
extern RECOMP_TLS RecompCpu g_cpu;
/* The thread's first frame (the array allocated), or past the last slot the
 * spare. Without memory, a scratch pair: frames go nowhere, the chain is
 * unreadable, the code runs. */
static RECOMP_FRAME_COLD RecompFrame *recomp_frames_slow(void)
{
    static RecompFrame s_nowhere[2];
    if (!g_cpu.r_fbase) {
        RecompFrame *f = (RecompFrame *)calloc(RECOMP_FRAME_SLOTS + 2, sizeof *f);
        if (!f)
            return s_nowhere;
        g_cpu.r_fbase = f;
        g_cpu.r_flimit = g_cpu.r_resume ? f : f + RECOMP_FRAME_SLOTS;
        g_cpu.r_ftop = f;
        return g_cpu.r_ftop;
    }
    /* Resuming (XBA-250): the limit was lowered to the base so every entry comes here; the frame is the next one.
     * A plan is never deeper than the slots (an OVERFLOW chain is not resumable). */
    if (RECOMP_FRAME_RARE(g_cpu.r_resume != 0))
        return g_cpu.r_ftop && g_cpu.r_ftop < g_cpu.r_fbase + RECOMP_FRAME_SLOTS ? g_cpu.r_ftop : g_cpu.r_fbase;
    /* Past the slots: the two spares, the second shared by everything deeper (never read). The first keeps the
     * top exact for a function 1024 deep; deeper, the chain says OVERFLOW whatever the top. */
    return g_cpu.r_ftop == g_cpu.r_flimit ? g_cpu.r_flimit : g_cpu.r_flimit + 1;
}
#if !defined(RECOMP_FRAME_SHADOW) || RECOMP_FRAME_SHADOW
#define RECOMP_FRAME_ENTRY() \
    RecompFrame *_gff = g_cpu.r_ftop; \
    if (RECOMP_FRAME_RARE((uintptr_t)_gff >= (uintptr_t)g_cpu.r_flimit)) _gff = recomp_frames_slow(); \
    _gff->f_ebp = g_cpu.r_ebp; _gff->f_esp = g_cpu.r_esp; g_cpu.r_ftop = _gff + 1
#define RECOMP_FRAME_LEAVE() (g_cpu.r_ftop = _gff)
/* The same with a resume entry (XBA-250): while the thread re-enters its saved chain (r_resume set, the limit
 * lowered so the compare above already fails), the frame is recorded and the function jumps to its resume
 * block (_recomp_resume, emitted by the lifter at its end), which goes to the call it was in. The check is
 * inside the slow path: nothing more on the way in. */
#define RECOMP_FRAME_ENTRY_R() \
    RecompFrame *_gff = g_cpu.r_ftop; \
    if (RECOMP_FRAME_RARE((uintptr_t)_gff >= (uintptr_t)g_cpu.r_flimit)) { \
        _gff = recomp_frames_slow(); \
        if (RECOMP_FRAME_RARE(g_cpu.r_resume != 0)) { \
            _gff->f_ebp = g_cpu.r_ebp; _gff->f_esp = g_cpu.r_esp; g_cpu.r_ftop = _gff + 1; \
            goto _recomp_resume; \
        } \
    } \
    _gff->f_ebp = g_cpu.r_ebp; _gff->f_esp = g_cpu.r_esp; g_cpu.r_ftop = _gff + 1
#else
#define RECOMP_FRAME_ENTRY() (void)0
#define RECOMP_FRAME_LEAVE() (void)0
#define RECOMP_FRAME_ENTRY_R() if (0) goto _recomp_resume
#endif
/* The runtime's side of a resume entry (src/kernel/xbox_resume.c): the site the function re-enters (a call's
 * return VA, or 0x80000000 | the VA of a host wait), the ebp it held there, the target of an indirect call re-made,
 * and a site the function has no entry for (fatal). The ebp comes from its own call, not an out-parameter: an
 * address-taken local would give every function that may park a stack slot and a longer prologue. */
uint32_t recomp_resume_site(uint32_t func);
uint32_t recomp_resume_ebp(void);
uint32_t recomp_resume_target(void);
void recomp_resume_lost(uint32_t func, uint32_t site);
/* A constant the compiler must not fold into its neighbours: the resume block's function VA, which it would
 * otherwise share with the return addresses the normal path pushes (an add more on that path, measured on arm64). */
#if defined(__GNUC__) || defined(__clang__)
#define RECOMP_OPAQUE32(x) __extension__ ({ uint32_t _rso = (x); __asm__("" : "+r"(_rso)); _rso; })
#else
#define RECOMP_OPAQUE32(x) (x)
#endif

extern RECOMP_TLS RecompCpu g_cpu;

#define g_eax             g_cpu.r_eax
#define g_ecx             g_cpu.r_ecx
#define g_edx             g_cpu.r_edx
#define g_esp             g_cpu.r_esp
#define g_ebx             g_cpu.r_ebx
#define g_esi             g_cpu.r_esi
#define g_edi             g_cpu.r_edi
#define g_ebp             g_cpu.r_ebp
#define g_seh_ebp         g_cpu.r_seh_ebp
#define g_fs_base         g_cpu.r_fs_base
#define g_eflags          g_cpu.r_eflags
#define g_mxcsr           g_cpu.r_mxcsr
#define g_df              g_cpu.r_df
#define g_fp_top          g_cpu.r_fp_top
#define g_fp_cmp          g_cpu.r_fp_cmp
#define g_fp_control_word g_cpu.r_fp_control_word
#define g_fp_cc           g_cpu.r_fp_cc
#define g_fp_stack        g_cpu.r_fp_stack
#define g_xmm0            g_cpu.r_xmm0
#define g_xmm1            g_cpu.r_xmm1
#define g_xmm2            g_cpu.r_xmm2
#define g_xmm3            g_cpu.r_xmm3
#define g_xmm4            g_cpu.r_xmm4
#define g_xmm5            g_cpu.r_xmm5
#define g_xmm6            g_cpu.r_xmm6
#define g_xmm7            g_cpu.r_xmm7
#define g_mm0             g_cpu.r_mm0
#define g_mm1             g_cpu.r_mm1
#define g_mm2             g_cpu.r_mm2
#define g_mm3             g_cpu.r_mm3
#define g_mm4             g_cpu.r_mm4
#define g_mm5             g_cpu.r_mm5
#define g_mm6             g_cpu.r_mm6
#define g_mm7             g_cpu.r_mm7
#endif /* RECOMP_CPU_DEFINED */

/* The runtime's view of a thread's frame shadow stack (XBA-247): how many frames are pushed (0 before its
 * first), and setting that count back (an SEH resume, a guest longjmp, a stack switch). */
static inline uint32_t recomp_frames_depth(const RecompCpu *c)
{
    return c->r_fbase && c->r_ftop >= c->r_fbase ? (uint32_t)(c->r_ftop - c->r_fbase) : 0u;
}
static inline void recomp_frames_set_depth(RecompCpu *c, uint32_t d)
{
    if (c->r_fbase)
        c->r_ftop = c->r_fbase + (d <= RECOMP_FRAME_SLOTS + 2u ? d : RECOMP_FRAME_SLOTS + 2u);
}

/* The toolkit region: scratch structures, then the main stack, then the heap.
 *
 * These used to be fixed -- scratch at 0x00700000, stack at 0x00780000, heap
 * at 0x00F80000 -- which is right only while the title's image ends below
 * 0x00700000. A title whose 70-odd sections run past 0x03000000 had the
 * stack, the kernel data exports and the heap inside its .data/BSS, and the
 * first zeroed heap allocation wiped its globals.
 *
 * xbox_MemoryLayoutInit now places the region at the first 64 KB boundary
 * above the highest loaded section, never below 0x00700000, so a title that
 * fit before sees exactly the old addresses. Offsets inside the region are
 * the old ones relative to 0x00700000. Valid once xbox_MemoryLayoutInit has
 * loaded the sections; nothing reads them earlier. */
#define XBOX_REGION_MIN         0x00700000u
#define XBOX_REGION_KDATA_OFF   0x00040000u  /* kernel data exports */
#define XBOX_REGION_TLS_OFF     0x00060000u  /* fake TLS structure (fs:[0x28]) */
#define XBOX_REGION_KPROC_OFF   0x00060800u  /* the title's KPROCESS (XBA-113) */
#define XBOX_REGION_PRCB_OFF    0x00061000u  /* zeroed KPCR Prcb stand-in */
#define XBOX_REGION_TLSBLK_OFF  0x00070000u  /* image TLS block */
#define XBOX_REGION_STACK_OFF   0x00080000u  /* main stack */
extern uint32_t g_xbox_region_base;
extern uint32_t g_xbox_stack_base;
extern uint32_t g_xbox_stack_size;

/* 8 MB, unless that would leave less than 16 MB of heap -- then it shrinks,
 * to no less than 1 MB (hardware gives the main thread the XBE's stack
 * commit, typically 64 KB). */
#define XBOX_STACK_SIZE     g_xbox_stack_size

/** Base VA of the stack area (above last XBE section). */
/* Where the fake TIB lives -- the linear address fs: is based at.
 *
 * Deliberately not 0. The TIB used to sit on page zero, because the lifter
 * dropped the fs prefix and fs:[N] became linear [N]. That made a null
 * dereference read or write the TIB instead of faulting: a null check of the
 * form `cmp byte [ecx], 0` saw the exception-chain head's 0xFF and passed, and
 * a store through a null pointer quietly overwrote that head. Both then
 * surfaced somewhere else entirely. With the TIB up here, page zero is left
 * unmapped and either mistake faults where it happens.
 *
 * Sits below every XBE's image base (0x00010000), so it displaces nothing.
 *
 * 0x4000 rather than 0x1000 because protection is applied at *host* page
 * granularity. Apple Silicon pages are 16 KB, so RECOMP_TRAP_NULL asking to
 * protect guest page zero actually covers guest 0..0x3FFF -- which reached a
 * TIB at 0x1000 and killed the run, so the guard disabled itself on every
 * such host and the diagnostic quietly did nothing. At 0x4000 the largest
 * page any supported host uses fits below the TIB and the guard installs.
 * Nothing else lives in the low 64 KB, and no guest code names the address:
 * fs: resolves through g_fs_base.
 *
 * Per-thread, because a TIB is. It used to be one constant address for the
 * whole process, which meant every guest thread shared one SEH chain head
 * and -- through fs:[4] -- one CRT per-thread data block. Half-Life 2
 * deadlocked on that: two threads in _lock() each holding the CRT lock the
 * other wanted, because the bookkeeping that decides who owns what was
 * shared between them.
 *
 * XBOX_TIB_MAIN is where the first thread's TIB is built; every spawned
 * thread gets its own from xbox_AllocThreadTib() and points g_fs_base at
 * it. */
#define XBOX_TIB_MAIN       0x00004000
#define XBOX_FS_BASE        g_fs_base

#define XBOX_STACK_BASE     g_xbox_stack_base

/** Initial ESP value (top of stack, 16-byte aligned). */
#define XBOX_STACK_TOP      (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)

/** A spawned guest thread's stack (xbox_AllocThreadStack), from the heap. */
#define XBOX_THREAD_STACK_SIZE  (512 * 1024)

/* ================================================================
 * Worker stack slices (host-tick-driven titles)
 * ================================================================
 *
 * A second way to drive a recompiled title, ported from the Burnout 3 fork as
 * the runtimes reunite (see docs/technical/burnout3-reunification.md).
 *
 * The default model (Halo, Crimson Skies) runs the game's entry routine inline
 * and it drives its own main loop. Some titles instead return from their entry
 * after spawning an init thread, and expect the *host* to drive the per-frame
 * tick -- Burnout 3 is tick-driven, not main-loop-driven. To call recompiled
 * code from the host's own message-loop thread, that thread needs a guest stack
 * (its g_esp starts at 0), which is what a worker slice provides.
 *
 * These slices carve the low end of the same 8 MB stack region xbox_AllocThreadStack
 * uses, and a title uses one model or the other -- never both -- so they do not
 * coexist at runtime. For a title that never calls xbox_worker_stack_alloc
 * (every default-model title), this is unused address space and dead code, so
 * adding it changes nothing for them.
 */
#define XBOX_WORKER_STACK_SIZE   (256 * 1024)
#define XBOX_WORKER_STACK_BASE   XBOX_STACK_BASE             /* 0x00780000 for small images */
#define XBOX_WORKER_STACK_COUNT  16                          /* 4 MB total */
#define XBOX_WORKER_STACK_END    (XBOX_WORKER_STACK_BASE + \
                                  XBOX_WORKER_STACK_SIZE * XBOX_WORKER_STACK_COUNT)

/** Top (initial esp) of worker stack slice n, 16-byte aligned, growing down. */
#define XBOX_WORKER_STACK_TOP(n) (XBOX_WORKER_STACK_BASE + \
                                  XBOX_WORKER_STACK_SIZE * ((n) + 1) - 16)

/* ================================================================
 * Xbox dynamic heap (for MmAllocateContiguousMemory, etc.)
 * ================================================================ */

/** Base VA of the dynamic heap area (above stack). */
#define XBOX_HEAP_BASE      (XBOX_STACK_BASE + XBOX_STACK_SIZE)  /* 0x00F80000 for small images */

/** Exclusive top of the dynamic heap: the end of RAM for this run. Runtime,
 *  not a macro, because RAM size is now configurable (retail 64 MB vs devkit
 *  128 MB). The total mapped region (data + stack + heap) equals RAM so the
 *  engine's memory probing stops at the correct boundary. */
/* The heap runs to the end of the *mapped* range, not the end of RAM.
 *
 * When a title maps more address space than it has RAM, a large
 * MEM_RESERVE has to come from somewhere. Carving it out of a separate
 * arena above the heap looked tidy and was wrong: the guest CRT's own
 * bookkeeping never learns about that region, so realloc's block lookup
 * fails for a pointer in it and the copy is skipped -- a grown buffer
 * comes back empty with the old one still intact. Half-Life 2 loses a
 * 129-node CUtlRBTree that way, and the tree then self-cycles.
 *
 * Letting the ordinary heap serve the whole mapped range keeps every
 * allocation inside one allocator the guest already understands.
 */
#define XBOX_HEAP_TOP       ((uint32_t)(g_xbox_map_size ? g_xbox_map_size \
                                                        : g_xbox_total_ram))

/** No static mirror/guard region. RAM mirror is handled via file mapping
 *  views that alias the same physical pages as the base 64 MB region. */
#define XBOX_MIRROR_SIZE    0
#define XBOX_GUARD_SIZE     0

/** Number of 64 MB mirror views to pre-map (covers 1.75 GB of address space). */
#define XBOX_NUM_MIRRORS    28

/* Tiled / write-combined aperture. The NV2A shows physical RAM again here, and
 * titles render through it: physical page P is at XBOX_TILED_BASE + P. Aliases
 * the RAM mapping rather than getting its own storage, because a title writes a
 * surface through the tiled address and reads it back through the normal one. */
#define XBOX_TILED_BASE     0xF0000000u

/**
 * Allocate from the Xbox heap. Returns an Xbox VA, or 0 on failure.
 * Alignment must be a power of 2 (minimum 4).
 * Thread-safe: yes; a lock guards the block table.
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/**
 * Free a block from the Xbox heap. Currently a no-op (bump allocator).
 */
void xbox_HeapFree(uint32_t xbox_va);

/* ---- Save states (XBA-245, xbox_state.h) -----------------------------------
 *
 * The guest memory a state carries, as ranges of separate storage: the RAM
 * view [0, mapped size), the contiguous window up to its high-water mark (its
 * own backing, not RAM: kernel.h XBOX_CONTIG_BASE), the lazy reservations
 * (POSIX, [cursor, top) above the RAM view) and the pages RECOMP_EXT_VMA has
 * committed (guest_vmem). Mirrors and the tiled alias are views of these and
 * are left out. `host` is where the range's first byte is on the host. */
enum {
    XBOX_RANGE_RAM = 0,
    XBOX_RANGE_CONTIG = 1,
    XBOX_RANGE_LAZY = 2,
    XBOX_RANGE_VMEM = 3,
};
typedef struct XboxGuestRange {
    uint32_t kind;          /* XBOX_RANGE_* */
    uint32_t va;            /* guest address, 4 KB aligned */
    uint32_t size;          /* bytes, a multiple of 4 KB */
    uint8_t *host;
} XboxGuestRange;
/* Fills out[0..max) and returns how many ranges there are (may exceed max). */
int xbox_StateGuestRanges(XboxGuestRange *out, int max);

/* The memory manager's host-side state (section MEM): the heap's cursor and
 * block table, the worker stack count, the contiguous window's page map,
 * block lengths, counters and the RAM-name map, the lazy reservations, and
 * guest_vmem's page table. Writes it to out when out is not NULL and cap is
 * large enough; returns its size either way. Takes the allocators' locks. */
#define XBOX_MEM_STATE_VERSION 1
size_t xbox_MemStateSave(uint8_t *out, size_t cap);
typedef struct XboxMemStateInfo {
    uint32_t heap_next, heap_blocks, heap_live;
    uint32_t contig_pages, contig_in_use, contig_high;
    uint32_t lazy_count, lazy_next, lazy_top;
    uint32_t vmem_active, vmem_runs;
} XboxMemStateInfo;
/* Parses a MEM payload (version XBOX_MEM_STATE_VERSION) without restoring
 * it: 0 when it is well formed and matches this build's window sizes, -1
 * otherwise. info may be NULL. */
int xbox_MemStateCheck(const uint8_t *in, size_t n, XboxMemStateInfo *info);
/* Puts a MEM payload back (same-run restore, XBA-249): the heap's block table and cursor, the thread stack
 * counts, the contiguous window's page map and counters, the lazy reservations. guest_vmem's page table must be
 * what it is now (RECOMP_EXT_VMA commits are not redone): otherwise -1 and nothing changes. 0, or -1 with a
 * reason in why. Every guest thread frozen. */
int xbox_MemStateLoad(const uint8_t *in, size_t n, char *why, size_t why_n);
/* What xbox_MemStateLoad would refuse, changing nothing: 0 when it would load. */
int xbox_MemStateLoadCheck(const uint8_t *in, size_t n, char *why, size_t why_n);

/* SHA-1 of the loaded XBE's headers (base to SizeOfHeaders: the image and
 * section headers with each section's own SHA-1 digest, the certificate), so
 * it names the whole image without hashing every section again. All zero
 * before an XBE is loaded. */
const uint8_t *xbox_XbeHeaderSha1(void);

/**
 * Bytes remaining in the heap block containing this guest address, or 0 if the
 * heap never handed it out. Backs MmQueryAllocationSize and
 * ExQueryPoolBlockSize -- the host cannot answer either, since VirtualQuery on
 * the translated address describes the whole guest mapping.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va);

/**
 * True when RECOMP_HEAP_RECLAIM is set: freed heap blocks are split rather than
 * handed over whole, and NtFreeVirtualMemory returns heap memory to the heap.
 * Off by default; it changes which address later allocations get.
 */
int xbox_HeapReclaimEnabled(void);

/**
 * Get the file mapping handle for the Xbox memory region.
 * Used by the VEH handler to map additional mirror views on demand.
 * Returns NULL if file mapping is not available.
 */
HANDLE xbox_GetMappingHandle(void);

#ifdef __cplusplus
}
#endif


/* Carve a simulated stack (512 KB of guest heap) for a spawned thread.
 * Returns the Xbox VA of the stack top, or 0 when the guest heap is exhausted
 * or the optional RECOMP_THREAD_STACKS cap is reached. */
uint32_t xbox_AllocThreadStack(void);

/* The most worker stacks ever alive at once (logged at 8 and each doubling). */
int xbox_ThreadStacksHighWater(void);

/* A TIB and TLS block for a newly spawned guest thread, copied from the
 * template the loader built. Returns the new TIB's Xbox VA, or 0. The TIB
 * names a KTHREAD of its own at fs:[0x28] (XBA-113). */
uint32_t xbox_AllocThreadTib(void);

/* The console's KTHREAD/ETHREAD fields that XAPI and lifted code read, written
 * into the object at `kt` (XBA-113): header and empty lists, StackBase/Limit,
 * TlsData (+0x28), State, Priority/BasePriority 8, ApcState with its Process
 * (the title's one KPROCESS, xbox_GuestKprocess), ETHREAD.ExitStatus
 * STILL_ACTIVE and a fresh UniqueThread (+0x12C). Fields past 0x140 are left
 * alone. */
#define XBOX_KTHREAD_SIZE 0x140u
void xbox_InitGuestKthread(uint32_t kt, uint32_t tls_data,
                           uint32_t stack_base, uint32_t stack_limit);

/* A fresh ETHREAD.UniqueThread. PsCreateSystemThread(Ex) takes one for the
 * thread it creates, stores it in the new KTHREAD and returns it through
 * ThreadId, so the creator's copy (CreateThread's lpThreadId) equals what the
 * thread reads from fs:[0x28]+0x12C (GetCurrentThreadId).
 * xbox_InitGuestKthread keeps a UniqueThread that is already set. */
uint32_t xbox_NewGuestThreadId(void);
void xbox_GuestThreadIdAtLeast(uint32_t id);   /* XBA-250: a resumed state's ids stay unique */

/* The guest KPROCESS every KTHREAD's ApcState.Process names; 0 before the
 * loader built it. */
uint32_t xbox_GuestKprocess(void);

/**
 * Return a worker's stack when the worker ends. Takes the value
 * xbox_AllocThreadStack returned. Without this the pool counts threads ever
 * created rather than threads alive, and a title that cycles workers exhausts
 * the guest heap.
 */
void xbox_FreeThreadStack(uint32_t stack_top);

/* Worker stack slices for host-tick-driven titles (see XBOX_WORKER_STACK_* and
 * docs/technical/burnout3-reunification.md). Additive; unused by default-model
 * titles. */
int  xbox_worker_stack_alloc(void);   /* slice index, or -1 if none free */
void xbox_worker_stack_free(int slot);
void   xbox_set_game_thread(void *h);  /* HANDLE, recorded for the host watchdog */
void  *xbox_thread_debug_handle(void); /* the game thread, or NULL under inline model */

/* PsCreateSystemThreadEx behaviour. Default INLINE runs the first call as the
 * game (Halo, Crimson Skies). SPAWN makes every call a real thread so a
 * host-tick-driven title's entry can return and let the host drive -- call
 * xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN) before the game starts. */
#define XBOX_THREAD_MODE_INLINE 0
#define XBOX_THREAD_MODE_SPAWN  1
void xbox_SetThreadMode(int mode);

#endif /* XBOX_MEMORY_LAYOUT_H */
