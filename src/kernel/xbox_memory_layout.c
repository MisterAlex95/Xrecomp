/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* glibc declares dladdr/Dl_info (RECOMP_WATCH) only under it */
#endif
#include "recomp_status.h"
#include "recomp_stack_switch.h"
#include "xbox_memory_layout.h"
#include "xbox_seh.h"
#include "kernel.h"
#include "guest_vmem.h"
#include "kernel_missing.h"
#include "nv2a_flip_hold.h"
#include "platform/host_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#if !defined(_WIN32)
#include <unistd.h>   /* _exit */
#include <time.h>     /* nanosleep: the ack thread's idle wait */
#include <sys/mman.h> /* lazy reservations: anonymous memory over the mirrors */
#endif

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120
#define XBE_TLS_ADDR_OFFSET     0x012C

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */

/* Actual mapped RAM for this run; see the header. Default retail 64 MB. */
size_t g_xbox_total_ram = XBOX_TOTAL_RAM;
size_t g_xbox_map_size = 0;   /* 0 = same as RAM */

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}


void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};

/* The base view and its 28 mirrors occupy one contiguous span. Reserving that
 * span up front is what makes the mirrors placeable at all: each one sits at
 * base + N * 64 MB, and on a host that chose the base for us, those addresses
 * run through whatever the loader already owns. Placing them one at a time
 * means ~3 of 28 collide, and *which* three changes with ASLR. Claiming the
 * whole range first, then carving views out of ground we hold, removes the
 * question. */
static void *g_span_base = NULL;
static size_t g_span_size = 0;
/* POSIX, RECOMP_EXT_VMA only: the span holds the whole 4 GB guest window, and
 * every fixed-address window is carved out of it (layout_map_view,
 * layout_commit_at). See the span reservation in xbox_MemoryLayoutInit. */
static int g_span_whole_window = 0;
/* POSIX, RECOMP_EXT_VMA: the part of the span handed to guest_vmem, which
 * reserves (and releases) it itself. Not ours any more: see span_holds. */
static uintptr_t g_span_gap_lo = 0, g_span_gap_hi = 0;

/* POSIX: is [addr, addr + size) part of the span reservation we still hold?
 *
 * Every window inside the span replaces its slice of the reservation in one
 * mmap (w32_map_view_replace, w32_commit_replace), and shutdown forgets those
 * windows and unmaps the span in one call, so the span never has a hole in it
 * while we hold it (XBA-55). It used to: init unmapped a slice and mapped the
 * window again with MAP_FIXED_NOREPLACE, and shutdown unmapped each window
 * and then the whole span. Anything another thread mapped into one of those
 * holes in between -- a malloc arena's 64 MB heap, a thread stack -- was then
 * unmapped with the span, under the thread using it. Under qemu-user that
 * was qemu's own arena for the NV2A ack thread, created while the thread
 * exited (the wait only sees its start routine return), and the next access
 * crashed about one run in ten after the shutdown; in an app process, any of
 * its threads can map at that moment. */
static int span_holds(const void *addr, size_t size)
{
#if !defined(_WIN32)
    uintptr_t a = (uintptr_t)addr, lo = (uintptr_t)g_span_base;
    if (!g_span_base || a < lo || a + size > lo + g_span_size)
        return 0;
    return a + size <= g_span_gap_lo || a >= g_span_gap_hi;
#else
    (void)addr; (void)size;
    return 0;
#endif
}
static void *g_tiled_view = NULL;

/* Lazy reservations (XBA-103): see lazy_reserve, by xbox_ReserveAlloc. */
#define LAZY_RESERVE_MAX 256
static struct { uint32_t base, size; int released; } g_lazy[LAZY_RESERVE_MAX];
static int g_lazy_count;
static uint32_t g_lazy_floor, g_lazy_top;
static volatile uint32_t g_lazy_next;     /* in use: [g_lazy_next, g_lazy_top) */
static SRWLOCK g_lazy_lock = SRWLOCK_INIT;
static void lazy_reset(void);

/* Contiguous / physical memory window (see MemoryLayoutInit).
 * XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE come from kernel.h - the bridges need
 * the same numbers for MmClaimGpuInstanceMemory. */
static void *g_contig_memory = NULL;

/* NV2A GPU register aperture (see MemoryLayoutInit). Backed as plain RAM so
 * that D3D8 code linked into the title can poke it without faulting. */
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

/* MCPX southbridge register span: APU 0xFE800000 through NIC 0xFEF00000. */
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
static void *g_mcpx_memory = NULL;

/* Flash ROM. The console's 256 KB flash is mirrored through the top of the
 * address space, and the MCPX span above stops one page short of it -- so a
 * title that touches it faulted on an address that is perfectly ordinary on
 * hardware.
 *
 * The Xbox Dashboard does, from two directions at once: its XIP workers hash
 * 64 KB from 0xFF000000 (it verifies archives against digests), and its render
 * path writes to 0xFF000040. Both are hard faults today, and they kill the
 * process a few dozen lines after its first frame clears.
 *
 * Plain memory, like the other two apertures, and mapped for the same stated
 * reason: a read of zero is survivable, a fault is not. Zeros are not the
 * console's BIOS, so a digest taken over this will not match one taken over
 * real flash -- that is a separate question from whether the access should
 * fault, and this is the half that has an obviously right answer. */
#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)
static void *g_flash_memory = NULL;
/* The contiguous window's backing section. It is a file mapping rather than
 * plain committed memory for one reason: the tiled aperture has to be a
 * second view of the very same bytes, and only a mapping can be mapped
 * twice. See the tiled aperture below for why that matters.
 */
static HANDLE g_contig_mapping = NULL;

/* Windows: one reservation for the whole guest address space.
 *
 * Every guest window lives at host g_memory_offset + guest VA: RAM at 0, its
 * mirrors, the contiguous window at 0x80000000, the tiled aperture at
 * 0xF0000000, and the NV2A, MCPX and flash apertures near the top. Reserving
 * only RAM and its mirrors, and letting the OS pick the base, leaves the rest
 * to luck. Under Wine the base landed at host 0x7FFF0000, so the contiguous
 * window fell on host 0xFFFF0000, which was taken (error 487). The kernel page
 * inside it went with it, and the title faulted reading guest 0x8000FFF0.
 *
 * So reserve 4 GB (or base + mirrors, if that is more) as one placeholder
 * (VirtualAlloc2 + MEM_RESERVE_PLACEHOLDER). Each window is then carved out of
 * it: split the placeholder (VirtualFree MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)
 * and replace that piece with a view (MapViewOfFile3) or with private memory
 * (VirtualAlloc2), both with MEM_REPLACE_PLACEHOLDER. Nothing else in the
 * process can be placed inside the window in between. One exception, opt-in:
 * under RECOMP_EXT_VMA the range above the mirrors is released outright for
 * guest_vmem, which reserves it with a plain VirtualAlloc. Another
 * thread could take it in that gap; guest_vmem then reports the reservation
 * failed and stays off, which is detectable, not corrupting.
 *
 * Both functions are resolved at run time from kernelbase.dll: they need
 * Windows 10 1803 or later, and Wine has them too. Without them, the fallback
 * looks for a free 4 GB hole and maps into it without holding it (see
 * win_reserve_guest_window).
 *
 * POSIX hosts do not use any of this. They keep the base + mirrors span in
 * xbox_MemoryLayoutInit, which works there because munmap can release part of
 * a mapping. */
#if defined(_WIN32)
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER  0x00004000
#endif
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER  0x00040000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#define WIN_GUEST_WINDOW 0x100000000ULL   /* the guest's 32-bit address space */

/* Our own prototypes, so this does not depend on how a given SDK gates the
 * declarations (_WIN32_WINNT, API families). The extended-parameter argument is
 * always NULL here. */
typedef PVOID (WINAPI *pfn_VirtualAlloc2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                          void *, ULONG);
typedef PVOID (WINAPI *pfn_MapViewOfFile3)(HANDLE, HANDLE, PVOID, ULONG64,
                                           SIZE_T, ULONG, ULONG, void *, ULONG);
static pfn_VirtualAlloc2  s_VirtualAlloc2;
static pfn_MapViewOfFile3 s_MapViewOfFile3;

/* Non-zero while the window is held as placeholders. */
static uintptr_t g_win_window_base;
static size_t    g_win_window_size;
static const char *g_win_reserve_method = "none (fixed base list, nothing held)";

static int win_in_window(uintptr_t addr, size_t size)
{
    return g_win_window_size && addr >= g_win_window_base
        && addr + size <= g_win_window_base + g_win_window_size;
}

/* Make [addr, addr + size) a placeholder of its own. This fails when the range
 * already is exactly one placeholder, which is fine. The replace call that
 * follows reports any real problem. */
static void win_split_placeholder(uintptr_t addr, size_t size)
{
    VirtualFree((LPVOID)addr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
}

/* Release every region still inside the window: placeholders, and the
 * reservations they were turned into. Views and committed apertures must be
 * released before this is called. */
static void win_release_window(void)
{
    uintptr_t a = g_win_window_base;
    uintptr_t end = g_win_window_base + g_win_window_size;

    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) || !mbi.RegionSize)
            break;
        if (mbi.State == MEM_RESERVE && (uintptr_t)mbi.AllocationBase == a)
            VirtualFree((LPVOID)a, 0, MEM_RELEASE);
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    g_win_window_base = 0;
    g_win_window_size = 0;
}

/* Turn the placeholders left over after init into ordinary reservations.
 *
 * Unmapped guest ranges still fault, as before. But code that later commits
 * memory at a fixed guest address with plain VirtualAlloc(MEM_COMMIT) works
 * again; the NV2A VRAM fault hook does this. A placeholder accepts only
 * MEM_REPLACE_PLACEHOLDER calls. Best effort: a failure leaves a placeholder,
 * which is still ours and still faults. */
static void win_settle_placeholders(void)
{
    uintptr_t a = g_win_window_base;
    uintptr_t end = g_win_window_base + g_win_window_size;

    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) || !mbi.RegionSize)
            break;
        if (mbi.State == MEM_RESERVE && (uintptr_t)mbi.AllocationBase == a)
            s_VirtualAlloc2(GetCurrentProcess(), (PVOID)a, mbi.RegionSize,
                            MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                            PAGE_NOACCESS, NULL, 0);
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
}
#endif /* _WIN32 */

/* RECOMP_EXT_VMA (guest_vmem): the guest range from the top of the
 * RAM mirrors to GUEST_VMEM_TOP, or 0 when the switch is off or the mirrors
 * already reach past it. Only when this is non-zero does the layout below
 * differ from the stock one. */
static uint64_t ext_vma_span(uint64_t mirror_top)
{
    if (!getenv("RECOMP_EXT_VMA") || mirror_top >= GUEST_VMEM_TOP)
        return 0;
    return GUEST_VMEM_TOP - mirror_top;
}

/* RECOMP_GUEST_WINDOW: hold the whole 4 GB guest window from the start and
 * carve every view out of it (POSIX), instead of reserving only base+mirrors
 * and mapping the contiguous window, the apertures and the tiled alias at
 * fixed addresses that must still be free. Linux places mappings top-down, so
 * the host range just above a fresh span is often taken already: under
 * qemu-user, and in a process full of libraries like an Android app, the
 * contiguous window at base + 2 GB then fails (ENOMEM) and the first pinned
 * allocation faults. =1 on, =0 off; unset, on for every 64-bit POSIX host.
 * RECOMP_EXT_VMA implies it.
 *
 * It used to be on for Android only. The desktop layout it replaced caps its
 * span at 2 GB once the map passes 64 MB, so the contiguous window lands on
 * the host range right above the span; on the Mac the malloc zone was there
 * and Fable's first contiguous allocation faulted at guest 0x80005000 (XBA-103,
 * staged in tests/memory_layout_posix). One layout everywhere: the 4 GB
 * reservation is PROT_NONE address space, which costs a 64-bit host nothing.
 * A 32-bit host cannot reserve it at all, so it keeps the old layout. */
static int hold_guest_window(void)
{
    const char *e = getenv("RECOMP_GUEST_WINDOW");
    if (sizeof(void *) < 8)
        return 0;
    if (e && *e)
        return *e != '0';
    return 1;
}

/* Map a view of `mapping` at a fixed host address. On Windows, inside the
 * reserved window, the view replaces the placeholder there. Elsewhere this is
 * the MapViewOfFileEx call it always was. */
static void *layout_map_view(HANDLE mapping, size_t size, void *addr)
{
#if defined(_WIN32)
    if (win_in_window((uintptr_t)addr, size)) {
        win_split_placeholder((uintptr_t)addr, size);
        return s_MapViewOfFile3(mapping, GetCurrentProcess(), addr, 0, size,
                                MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                NULL, 0);
    }
#else
    if (span_holds(addr, size))
        return w32_map_view_replace(mapping, size, addr);
#endif
    return MapViewOfFileEx(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size, addr);
}

/* Commit zeroed read-write memory at a fixed host address. Same split as
 * layout_map_view. Replace the placeholder with a reservation, then commit it.
 * Two steps, because MEM_COMMIT together with MEM_REPLACE_PLACEHOLDER is not
 * documented to work. */
static void *layout_commit_at(void *addr, size_t size)
{
#if defined(_WIN32)
    if (win_in_window((uintptr_t)addr, size)) {
        win_split_placeholder((uintptr_t)addr, size);
        if (!s_VirtualAlloc2(GetCurrentProcess(), addr, size,
                             MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                             PAGE_NOACCESS, NULL, 0))
            return NULL;
        return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE);
    }
#else
    if (span_holds(addr, size))
        return w32_commit_replace(addr, size);
#endif
    return VirtualAlloc(addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

/* Shutdown's counterpart: unmap a window, or forget one inside the span,
 * which the span release unmaps with the rest of it (see span_holds). */
static void layout_release(void *addr, int is_view)
{
#if !defined(_WIN32)
    if (span_holds(addr, 1)) {
        w32_view_forget(addr);
        return;
    }
#endif
    if (is_view)
        UnmapViewOfFile(addr);
    else
        VirtualFree(addr, 0, MEM_RELEASE);
}

#if defined(_WIN32)
/* Reserve the guest window and map RAM at its base. Returns the RAM view, or
 * NULL. On NULL the caller falls back to the fixed try_bases list.
 *
 * 1. Placeholders (see above): the whole window is ours before anything is
 *    mapped into it.
 * 2. No placeholder API, or it failed: reserve a window to find a free hole,
 *    release it, and map RAM at its base. The later windows are mapped at
 *    fixed addresses without a reservation. That is racy, but at init nothing
 *    else is likely to allocate 4 GB of address space in between. */
static void *win_reserve_guest_window(HANDLE mapping, size_t ram_size)
{
    size_t span = ram_size * (size_t)(1 + XBOX_NUM_MIRRORS);
    size_t window = span > WIN_GUEST_WINDOW ? span : (size_t)WIN_GUEST_WINDOW;
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    void *ram;

    window = (window + 0xFFFF) & ~(size_t)0xFFFF;
    if (kb) {
        s_VirtualAlloc2 = (pfn_VirtualAlloc2)(void (*)(void))
            GetProcAddress(kb, "VirtualAlloc2");
        s_MapViewOfFile3 = (pfn_MapViewOfFile3)(void (*)(void))
            GetProcAddress(kb, "MapViewOfFile3");
    }

    if (s_VirtualAlloc2 && s_MapViewOfFile3) {
        void *base = s_VirtualAlloc2(GetCurrentProcess(), NULL, window,
                                     MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                     PAGE_NOACCESS, NULL, 0);
        if (base) {
            g_win_window_base = (uintptr_t)base;
            g_win_window_size = window;
            ram = layout_map_view(mapping, ram_size, base);
            if (ram == base) {
                g_win_reserve_method = "placeholder (VirtualAlloc2 + MapViewOfFile3)";
                return ram;
            }
            fprintf(stderr, "xbox_MemoryLayoutInit: placeholder reservation at %p"
                    " could not take the RAM view (error %lu); falling back\n",
                    base, GetLastError());
            if (ram)
                UnmapViewOfFile(ram);
            win_release_window();
        } else {
            fprintf(stderr, "xbox_MemoryLayoutInit: VirtualAlloc2 placeholder"
                    " reservation of %zu MB failed (error %lu); falling back\n",
                    window >> 20, GetLastError());
        }
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: VirtualAlloc2/MapViewOfFile3 not"
                " found in kernelbase.dll; falling back to an unreserved"
                " 4 GB window\n");
    }

    {
        void *probe = VirtualAlloc(NULL, window, MEM_RESERVE, PAGE_NOACCESS);
        if (probe) {
            VirtualFree(probe, 0, MEM_RELEASE);
            ram = MapViewOfFileEx(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                  ram_size, probe);
            if (ram) {
                g_win_reserve_method = "probed free window (not held; racy)";
                return ram;
            }
        }
    }
    return NULL;
}
#endif /* _WIN32 */
/* How much of the tiled aperture can exist.
 *
 * Two ceilings, both below the mapped RAM size once that is large:
 *
 *   - it starts at 0xF0000000 in a 32-bit guest address space, so it can
 *     never reach past 0x100000000; and
 *   - the NV2A register aperture sits at 0xFD000000, which is where the
 *     window really ends on hardware.
 *
 * Asking for the full RAM size overlapped both and MapViewOfFileEx failed
 * with ERROR_INVALID_ADDRESS -- a warning at startup and then a fault on the
 * title's first surface write, with nothing connecting the two. */
/* How much guest address space is mapped.
 *
 * For anything that dereferences an address it read out of guest memory --
 * a descriptor pointer a device model follows, say. Those are attacker-ish
 * input in the only sense that matters here: the title can leave one
 * uninitialised, and 0xCCCCCCCC dereferenced is a crash in the runtime
 * rather than a fault the title would have taken. */
size_t xbox_GetMappedSize(void)
{
    return g_memory_size;
}

static size_t xbox_TiledApertureSize(void)
{
    uint64_t end = XBOX_NV2A_BASE < 0x100000000ULL
                 ? XBOX_NV2A_BASE : 0x100000000ULL;
    size_t max = (size_t)(end - XBOX_TILED_BASE);
    return g_memory_size < max ? g_memory_size : max;
}

static HANDLE g_nv2a_ack_thread = NULL;
static volatile LONG g_nv2a_ack_stop = 0;

/*
 * NV2A busy-bit acknowledgement.
 *
 * D3D8 talks to the GPU through set-a-bit / wait-for-hardware-to-clear-it
 * handshakes. Against plain RAM the bit is set and nothing ever clears it, so
 * the title spins forever. Halo hangs in the push-buffer kick at 0x001EF930:
 *
 *     mov  [eax+0x100410], edx     ; set 0x10000
 *   L: test [eax+0x100410], 0x10000
 *     jne  L                       ; wait for the GPU
 *
 * Clearing those bits from a thread is not a hack around the handshake, it is
 * the handshake: on hardware the GPU clears them asynchronously, which is
 * exactly what this does. Work that would have been submitted is being done by
 * the D3D11 layer instead, so acknowledging immediately is honest.
 *
 * Only registers listed here are touched. Blanket-zeroing the aperture would
 * also wipe registers holding real state.
 *
 * ponytail: table-driven, extend as more handshakes turn up. A spin on a bit
 * that is not listed still hangs -- run the title and the watchdog sample will
 * name the register.
 */
static const struct { uint32_t offset; uint32_t busy_mask; } NV2A_ACK[] = {
    { 0x100410, 0x00010000u },  /* PFB flush kick, Halo 0x001EF930 */

    /* Interrupt status registers. These are write-1-to-clear on hardware, so
     * an ISR "clearing" one writes the pending bit back -- against plain RAM
     * that sets it instead, the interrupt stays pending forever, and the
     * service routine re-enters until the stack is gone. Halo dies exactly
     * that way: CMiniport::ServiceGrInterrupt writes 0x1000 to PGRAPH_INTR to
     * acknowledge, reads it back still pending, and recurses into a native
     * stack overflow.
     *
     * Holding them at zero is correct rather than convenient: nothing here
     * ever raises a GPU interrupt, so "none pending" is the truth. */
    { 0x000100, 0xFFFFFFFFu },  /* PMC_INTR_0    */
    { 0x001100, 0xFFFFFFFFu },  /* PBUS_INTR_0   */
    { 0x002100, 0xFFFFFFFFu },  /* PFIFO_INTR_0  */
    { 0x400100, 0xFFFFFFFFu },  /* PGRAPH_INTR   */
    { 0x600100, 0xFFFFFFFFu },  /* PCRTC_INTR_0  */
};

/*
 * Bits that must always read as SET. The mirror image of the table above:
 * where an interrupt-pending bit is false because nothing raises interrupts,
 * a queue-empty bit is true because nothing is queued.
 *
 * Halo's CMiniport::TilingUpdateIdle spins until the PFIFO caches report
 * empty (0x001F5CD1). Zeroed RAM says "not empty" forever, so tile setup
 * during CDevice::InitializeFrameBuffers never completes.
 *
 * Note 0x003220 is deliberately absent -- that one exits on the bit being
 * CLEAR, which zeroed memory already gives.
 */
static const struct { uint32_t offset; uint32_t idle_mask; } NV2A_IDLE[] = {
    { 0x002400, 0x00000010u },  /* PFIFO_RUNOUT_STATUS  LOW_MARK (empty) */
    { 0x003214, 0x00000010u },  /* PFIFO_CACHE1_STATUS  LOW_MARK (empty) */
};

/*
 * PFIFO channel DMA pointers. Software writes DMA_PUT and spins until the GPU
 * advances DMA_GET to match -- "you have consumed everything I submitted".
 * Halo's wait is at 0x001F3948:
 *
 *   L: call BusyLoop
 *      ecx = [[dev+0x2304] + 0x44]   ; DMA_GET
 *      edx = [dev]                   ; DMA_PUT
 *      test (edx ^ ecx), 0xfffffff
 *      jne L
 *
 * [dev+0x2304] is 0xFD800000, so the channel's USER area sits at aperture
 * offset 0x800000 and the two pointers are at +0x40 / +0x44. Copying PUT to
 * GET is the acknowledgement; the commands are not executed from the push
 * buffer here -- the D3D11 layer draws -- so reporting them consumed is the
 * truthful answer.
 *
 * This was written once, removed, and restored. It was removed because
 * [dev+0x2304] read as 0x0080F7FF, i.e. no register to acknowledge -- but that
 * garbage was a downstream symptom of ordinal 47 having no stdcall arg size,
 * which walked esp 8 bytes off and made D3D initialise the DMA channel with
 * `this` = 1. With that fixed the pointer is correct and so is this.
 */
#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u

/*
 * The same channel's pointers on the PFIFO side of the aperture.
 *
 * The USER area above is the window software writes through; PFIFO holds the
 * engine's own copy, and D3D reads it back on the path where the USER pointer
 * is not usable. The title's channel context switch saves and restores all
 * four of these as one block (DDS9 0x002FE2xx), which is what identifies them:
 *
 *   0x3240 CACHE1_DMA_PUT          0x3248 CACHE1_REF
 *   0x3244 CACHE1_DMA_GET          0x324C CACHE1_DMA_SUBROUTINE
 *
 * DMA_SUBROUTINE matters because it is not a flag: bits 31:1 are the offset
 * the engine returns to when a pushbuffer subroutine ends, and bit 0 says
 * whether one is running. DDS9's free-space calculation (sub_002F6CC0) reads
 * the USER GET first and falls back to this register's return offset when
 * that lands outside the ring -- i.e. "the GPU is off in a subroutine, so ask
 * where it will come back to". Zeroed RAM answers 0 to both, which is below
 * the ring base, and the free-space subtraction then goes negative and is
 * clamped to zero. The reserve wants 0x2000 bytes, gets 0, and spins.
 *
 * Not acknowledged here, only reported. DDS9 reads the USER pair and never
 * reaches the fallback, so every value in this block is zero for the one
 * title that was traced -- mirroring PUT to GET would be a guess dressed as
 * a handshake. The watchdog prints them so the next title to spin here is
 * diagnosed from data instead.
 */
#define NV2A_PFIFO_DMA_PUT        0x003240u
#define NV2A_PFIFO_DMA_GET        0x003244u
#define NV2A_PFIFO_REF            0x003248u
#define NV2A_PFIFO_DMA_SUBROUTINE 0x00324Cu

/*
 * Free-running counters in the MCPX aperture.
 *
 * Some hardware registers are clocks, not flags: software reads them and waits
 * until the value passes a target. Against zeroed RAM the value never moves and
 * the wait is forever. DirectSound's CMcpxCore::SetupVoiceProcessor spins on
 * the APU sample counter at 0xFE820010 exactly this way, which is where Halo
 * stopped once input initialisation started working.
 *
 * Ticking it is the honest model: on hardware this counter advances on its own
 * whether or not anything is listening.
 *
 * ponytail: the rate is "as fast as this thread loops", not 48 kHz. Nothing
 * paces audio off it yet. Derive it from a real clock if timing starts to
 * matter.
 */
/*
 * AC'97 bus-master reset, modelled by trapping the write rather than by
 * clearing the bit afterwards.
 *
 * Each of the three DMA channels -- PCM In, PCM Out, Mic In -- has a one-byte
 * control register at NABM + 0x0B, and bit 1 is RR, "Reset Registers".
 * Software sets it and waits for the controller to clear it. DDS9 does that
 * inside DirectSoundCreate, and the wait is worth quoting because it is not a
 * poll:
 *
 *     mov  cl, [eax+0xFEC0010B]
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L
 *
 * MSVC hoisted the load out of the loop -- the pointer was not volatile -- so
 * the title reads the register exactly ONCE, a few instructions after writing
 * it, and spins forever on whatever that single read returned. On hardware
 * the reset has long completed by then.
 *
 * That rules out the NV2A_ACK approach. A thread that clears the bit
 * afterwards is racing a window a few instructions wide and gets only one
 * attempt; measured, it loses, and the title sits on a stale cl = 2 while the
 * register itself reads 0. The bit has to be clear at the moment of the read,
 * which means the write must never deposit it.
 *
 * So the page is PAGE_READONLY: reads run at full speed and see plain memory,
 * writes fault. The fault handler makes the page writable, single-steps the
 * faulting instruction, then masks RR out of the three control bytes and
 * re-protects. No instruction decoding, which matters because the write forms
 * a compiler emits here are not worth enumerating -- and being wrong about
 * one would corrupt a register rather than fail visibly.
 *
 * Only RR. Bit 0 is RPBM, run/pause bus master, which software owns.
 *
 * That open window is process-wide, though: while one thread's write is being
 * stepped, another thread's plain store to the page lands unmasked. On POSIX
 * the page is therefore a shared mapping instead, read-only at the guest
 * address with a second writable view: the handler decodes the store, masks
 * RR out of it and writes it through the view, and the guest page is never
 * opened (ac97_map_view). The mprotect window stays as the fallback. Windows
 * keeps the single-step for now and with it the residual; Fable's DirectSound
 * resets channels from two threads at once during movies, which is what
 * exposed it on POSIX.
 */
#define AC97_NABM_OFFSET  0x400000u   /* 0xFEC00000 within the MCPX aperture */
#define AC97_TRAP_BYTES   0x1000u
#define AC97_RR           0x02u

static void *g_ac97_page = NULL;      /* host address of the trapped page */
static void *g_ac97_veh  = NULL;
static RECOMP_TLS int s_ac97_stepping = 0;

static void ac97_clear_reset_bits(void)
{
    /* Every bus-master channel, not the three a PC AC'97 has.
     *
     * The generic controller has PCM In, PCM Out and Mic In at NABM +0x00,
     * +0x10 and +0x20; the MCPX has more, and DDS9 walks a table of channel
     * offsets rather than naming them. It reset the channel at +0x00 first
     * and then one at +0x60 -- which a three-entry list did not cover, so it
     * spun on the second exactly as it had on the first. Sweeping the whole
     * NABM block is both simpler and right: +0x0B is the control byte of
     * whatever channel lives there, and RR is the same bit in all of them. */
    uint32_t off;

    for (off = 0x10B; off < 0x180; off += 0x10) {
        volatile uint8_t *r = (volatile uint8_t *)((char *)g_ac97_page + off);
        if (*r & AC97_RR)
            *r = (uint8_t)(*r & ~AC97_RR);
    }
}

static LONG CALLBACK ac97_write_veh(PEXCEPTION_POINTERS ep)
{
#if defined(_WIN32)
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    if (!g_ac97_page)
        return EXCEPTION_CONTINUE_SEARCH;

    /* Second half: the faulting write has now executed. Apply what the
     * controller would have done and close the page again. Thread-local,
     * because another thread must not mistake its own single-step for this
     * one -- and re-protecting from the wrong thread would strand this one
     * mid-step. */
    if (code == EXCEPTION_SINGLE_STEP && s_ac97_stepping) {
        s_ac97_stepping = 0;
        ac97_clear_reset_bits();
        VirtualProtect(g_ac97_page, AC97_TRAP_BYTES, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;   /* clear TF */
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];

        if (fault >= (uintptr_t)g_ac97_page
                && fault < (uintptr_t)g_ac97_page + AC97_TRAP_BYTES) {
            if (!VirtualProtect(g_ac97_page, AC97_TRAP_BYTES,
                                PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            s_ac97_stepping = 1;
            ep->ContextRecord->EFlags |= 0x100u;   /* TF: step the write */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
#else
    /* The non-Windows VEH shim never fires and EXCEPTION_POINTERS is
     * opaque there, so there is nothing to decode. */
    (void)ep;
    (void)s_ac97_stepping;
    (void)ac97_clear_reset_bits;
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

#if !defined(_WIN32) && (defined(__aarch64__) \
        || (defined(__x86_64__) && defined(__linux__)))
#if defined(__aarch64__)
#include "../platform/mmio_decode_a64.h"
#else
#include "../platform/mmio_decode_x86_64.h"
#endif
#include <sys/mman.h>
static size_t g_ac97_hsize;    /* host page: 16 KB on arm64 Darwin */
/* A second, writable mapping of the trapped page; NULL on the mprotect
 * fallback. See ac97_arm_write_trap. */
static void *g_ac97_view;

static uint64_t ac97_posix_rd(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    memcpy(&v, (char *)dev + off, size > 8 ? 8 : size);
    return v;
}

/* The controller's half of the store: RR never reaches the register. The
 * mask is applied to the bytes being written, so the bit is never visible
 * to any reader, on any thread, at any moment -- which a sweep after the
 * store cannot promise. The control bytes are the ones ac97_clear_reset_bits
 * sweeps: +0x0B of every channel in 0x100..0x17F. */
static void ac97_posix_wr(void *dev, uint32_t off, uint64_t v, int size)
{
    uint8_t b[8];
    int i, n = size > 8 ? 8 : size;

    memcpy(b, &v, (size_t)n);
    for (i = 0; i < n; i++) {
        uint32_t o = off + (uint32_t)i;
        if (o >= 0x10B && o < 0x180 && (o & 0xFu) == 0xBu)
            b[i] &= (uint8_t)~AC97_RR;
    }
    memcpy((char *)(g_ac97_view ? g_ac97_view : dev) + off, b, (size_t)n);
}

/* POSIX half of the AC'97 write trap: the page is read-only at the guest
 * address, so a write arrives as SIGBUS/SIGSEGV; the title's fault handler
 * reaches this through xbox_PosixMmioFault before reporting. Returns 1 when
 * the fault was this trap and the access was serviced.
 *
 * Normally the store is emulated through g_ac97_view, the writable mapping
 * of the same memory: the guest page itself never becomes writable, so
 * every other thread's store keeps faulting and goes through the same mask.
 * No lock either: two emulated stores are two stores to the same memory,
 * as on hardware.
 *
 * Fallback (no view: the shared mapping could not be made): open the page,
 * emulate, sweep, close. The page is then writable process-wide for that
 * window, so a plain store from another thread lands unmasked; the sweep
 * catches one that lands before it, one between the sweep and the close
 * keeps its RR bit. Fable hit exactly that (two threads resetting channels
 * in DirectSound's sub_00832BFC during movies, the main thread then spinning
 * for ever), which is why the view exists. */
static volatile int g_ac97_posix_lock;

static int ac97_emulate(void *ucv, uintptr_t fault)
{
#if defined(__aarch64__)
    return mmio_emulate_a64((ucontext_t *)ucv,
                            (uint32_t)(fault - (uintptr_t)g_ac97_page),
                            g_ac97_page, ac97_posix_rd, ac97_posix_wr);
#else
    return mmio_emulate_x86_64((ucontext_t *)ucv,
                               (uint32_t)(fault - (uintptr_t)g_ac97_page),
                               g_ac97_page, ac97_posix_rd, ac97_posix_wr);
#endif
}

int xbox_Ac97PosixFault(void *ucv, uintptr_t fault)
{
    int ok;

    if (!g_ac97_page || fault < (uintptr_t)g_ac97_page
            || fault >= (uintptr_t)g_ac97_page + g_ac97_hsize)
        return 0;
    if (g_ac97_view)
        return ac97_emulate(ucv, fault);
    /* Fallback. One trap at a time: a second thread trapping in the open
     * window would have the page closed under it by the first thread's
     * mprotect and fault inside the emulation. */
    while (__atomic_exchange_n(&g_ac97_posix_lock, 1, __ATOMIC_ACQUIRE))
        ;
    /* Not opened: the emulated store would fault again on this thread,
     * which holds the lock it would then spin on. Let it be reported. */
    if (mprotect(g_ac97_page, g_ac97_hsize, PROT_READ | PROT_WRITE) != 0) {
        __atomic_store_n(&g_ac97_posix_lock, 0, __ATOMIC_RELEASE);
        return 0;
    }
    ok = ac97_emulate(ucv, fault);
    if (ok)
        ac97_clear_reset_bits();
    mprotect(g_ac97_page, g_ac97_hsize, PROT_READ);
    __atomic_store_n(&g_ac97_posix_lock, 0, __ATOMIC_RELEASE);
    return ok;
}

/* Put a shared mapping under the trapped page: read-only at the guest
 * address, writable at g_ac97_view. The page's current contents (the codec
 * ready bit) are copied over first. RECOMP_AC97_ALIAS=0 skips it and keeps
 * the mprotect fallback, for A/B. Returns 1 when the view is in place; on 0
 * the page is untouched. */
static int ac97_map_view(void)
{
    const char *e = getenv("RECOMP_AC97_ALIAS");
    void *view;
    int fd;

    if (e && e[0] == '0')
        return 0;
    fd = w32_anon_map_fd("xba_ac97");
    if (fd < 0)
        return 0;
    if (ftruncate(fd, (off_t)g_ac97_hsize) != 0) {
        close(fd);
        return 0;
    }
    view = mmap(NULL, g_ac97_hsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) {
        close(fd);
        return 0;
    }
    memcpy(view, g_ac97_page, g_ac97_hsize);
    /* Replaces the page in one call: no moment where it is unmapped. */
    if (mmap(g_ac97_page, g_ac97_hsize, PROT_READ, MAP_SHARED | MAP_FIXED,
             fd, 0) == MAP_FAILED) {
        munmap(view, g_ac97_hsize);
        close(fd);
        return 0;
    }
    close(fd);                  /* both mappings keep the memory alive */
    g_ac97_view = view;
    return 1;
}

static void ac97_disarm_write_trap(void)
{
    if (g_ac97_view) {
        munmap(g_ac97_view, g_ac97_hsize);
        g_ac97_view = NULL;
    }
    g_ac97_page = NULL;
}
#else
static void ac97_disarm_write_trap(void)
{
#if defined(_WIN32)
    if (g_ac97_veh) {
        RemoveVectoredExceptionHandler(g_ac97_veh);
        g_ac97_veh = NULL;
    }
#endif
    g_ac97_page = NULL;
}
#endif

/* Arm the trap. Called once the MCPX aperture exists, and only alongside the
 * rest of RECOMP_AC97_READY: a title that never gets as far as resetting a
 * channel has nothing to gain from it, and the page fault costs something. */
static void ac97_arm_write_trap(void)
{
    DWORD old;

    if (!g_mcpx_memory || g_ac97_page)
        return;
#if !defined(_WIN32) && !defined(__aarch64__) \
        && !(defined(__x86_64__) && defined(__linux__))
    /* No POSIX decoder for this host's stores: a trapped write would be a
     * fault nothing services. Leave the page writable. */
    (void)old;
    fprintf(stderr, "  AC97: bus-master write trap not available on this"
                    " host; a channel reset will spin\n");
    return;
#elif !defined(_WIN32)
    (void)old;
    g_ac97_page = (char *)g_mcpx_memory + AC97_NABM_OFFSET;
    g_ac97_hsize = (size_t)sysconf(_SC_PAGESIZE);
    if (g_ac97_hsize < AC97_TRAP_BYTES)
        g_ac97_hsize = AC97_TRAP_BYTES;
    if (ac97_map_view()) {
        fprintf(stderr, "  AC97: bus-master writes trapped at 0x%08X"
                        " (POSIX, writable view; channel reset completes"
                        " on write)\n", XBOX_MCPX_BASE + AC97_NABM_OFFSET);
        return;
    }
    if (mprotect(g_ac97_page, g_ac97_hsize, PROT_READ) != 0) {
        g_ac97_page = NULL;
        fprintf(stderr, "  AC97: could not arm the bus-master write trap;"
                        " a channel reset will spin\n");
        return;
    }
    fprintf(stderr, "  AC97: bus-master writes trapped at 0x%08X"
                    " (POSIX, mprotect fallback; channel reset completes"
                    " on write)\n", XBOX_MCPX_BASE + AC97_NABM_OFFSET);
#else
    g_ac97_page = (char *)g_mcpx_memory + AC97_NABM_OFFSET;
    /* First, so it runs before the game target's own crash reporter, which
     * would otherwise print the write as an access violation. */
    g_ac97_veh = AddVectoredExceptionHandler(1, ac97_write_veh);
    if (!g_ac97_veh
            || !VirtualProtect(g_ac97_page, AC97_TRAP_BYTES,
                               PAGE_READONLY, &old)) {
        if (g_ac97_veh) {
            RemoveVectoredExceptionHandler(g_ac97_veh);
            g_ac97_veh = NULL;
        }
        g_ac97_page = NULL;
        fprintf(stderr, "  AC97: could not arm the bus-master write trap;"
                        " a channel reset will spin\n");
        return;
    }
    fprintf(stderr, "  AC97: bus-master writes trapped at 0x%08X"
                    " (channel reset completes on write)\n",
            XBOX_MCPX_BASE + AC97_NABM_OFFSET);
#endif
}

/*
 * Trapped-MMIO dispatch: one call for a title's fault handler.
 *
 * Three models answer hardware registers by faulting on them -- the APU, the
 * AC'97 bus-master page and the USB OHCI controllers -- and each has its own
 * handler. A title's fault handler had to call them one by one, and one whose
 * handler missed a model died at that device's first register access. These
 * route the fault to whichever model owns the address and return 1 when it
 * was serviced, so the caller resumes the thread instead of reporting a
 * crash. They only re-route: each model's own handler does the work.
 */
#include "apu/apu.h"
#include "apu/apu_wav.h"
#include "usb/ohci.h"

#define MMIO_APU_BASE 0xFE800000u   /* what MemoryLayoutInit unmaps for the APU */
#define MMIO_APU_END  0xFE880000u

extern MCPXAPUState *g_apu_state;   /* apu_mmio_hook.c; NULL until audio is up */

/* The guest VA a host fault address names. Only an address inside the 4 GB
 * guest window names one: truncating the offset to 32 bits would let a host
 * address far above it alias into a device range. */
static int mmio_fault_va(uintptr_t fault, uint32_t *va)
{
    uintptr_t base = (uintptr_t)xbox_GetMemoryOffset();
    uint64_t rel = (uint64_t)fault - (uint64_t)base;

    if (fault < base || rel >= 0x100000000ull)
        return 0;
    *va = (uint32_t)rel;
    return 1;
}

#if defined(_WIN32)
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* Win32: call from the title's VEH for an access violation; on 1, return
 * EXCEPTION_CONTINUE_EXECUTION. APU, then OHCI. The AC'97 page needs no call
 * here: its trap is a VEH of its own, installed first, because it single-steps
 * the write and so has to see the EXCEPTION_SINGLE_STEP that follows. */
int xbox_VehMmioFault(void *exception_pointers)
{
    PEXCEPTION_POINTERS ep = (PEXCEPTION_POINTERS)exception_pointers;
    uintptr_t fault;
    uint32_t va;

    if (!ep || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return 0;
    fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    if (!mmio_fault_va(fault, &va))
        return 0;
    if (va >= MMIO_APU_BASE && va < MMIO_APU_END)
        return g_apu_state
            && apu_hook_handle_mmio(ep->ContextRecord, fault, va,
                   ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0);
    if (xbox_OhciOwnsAddress(va))
        return xbox_OhciHandleMmio(ep->ContextRecord, va);
    return 0;
}
#else
#if defined(__aarch64__) || (defined(__x86_64__) && defined(__linux__))
extern bool apu_hook_handle_mmio_posix(void *ucv, uintptr_t fault_addr,
                                       uint32_t fault_xbox_va, int is_write);
#endif

/* The caller's own trapped window (xbox_PosixMmioSetTestWindow). */
static struct {
    uint32_t va, size;              /* size 0: none; written last, read first */
    void *dev;
    xbox_mmio_read_fn rd;
    xbox_mmio_write_fn wr;
} g_mmio_test;

/* POSIX: call from the title's SIGBUS/SIGSEGV handler (SA_SIGINFO) with its
 * ucontext_t * and si_addr; on 1, return from the handler and the thread
 * resumes after the emulated instruction. APU, then AC'97, then OHCI, then
 * the caller's test window (the self-test, XBA-51). Linux
 * arm64 and Linux x86-64 have decoders; elsewhere nothing is trapped and this
 * returns 0. */
int xbox_PosixMmioFault(void *ucv, uintptr_t fault)
{
#if defined(__aarch64__) || (defined(__x86_64__) && defined(__linux__))
    uint32_t va;

    if (!mmio_fault_va(fault, &va))
        return 0;
    /* The APU decodes the direction from the opcode; is_write is advisory. */
    if (va >= MMIO_APU_BASE && va < MMIO_APU_END)
        return g_apu_state
            && apu_hook_handle_mmio_posix(ucv, fault, va, 0);
    if (xbox_Ac97PosixFault(ucv, fault))
        return 1;
    if (xbox_OhciOwnsAddress(va))
        return xbox_OhciHandleMmio(ucv, va);
    if (__atomic_load_n(&g_mmio_test.size, __ATOMIC_ACQUIRE)
            && va - g_mmio_test.va < g_mmio_test.size)
#if defined(__aarch64__)
        return mmio_emulate_a64((ucontext_t *)ucv, va - g_mmio_test.va,
                                g_mmio_test.dev, g_mmio_test.rd, g_mmio_test.wr);
#else
        return mmio_emulate_x86_64((ucontext_t *)ucv, va - g_mmio_test.va,
                                   g_mmio_test.dev, g_mmio_test.rd, g_mmio_test.wr);
#endif
#else
    (void)ucv; (void)fault;
#endif
    return 0;
}

int xbox_PosixMmioSetTestWindow(uint32_t va, uint32_t size, void *dev,
                                xbox_mmio_read_fn rd, xbox_mmio_write_fn wr)
{
#if defined(__aarch64__) || (defined(__x86_64__) && defined(__linux__))
    __atomic_store_n(&g_mmio_test.size, 0, __ATOMIC_RELEASE);
    if (!size)
        return 0;
    if (!rd || !wr)
        return -1;
    g_mmio_test.va = va;
    g_mmio_test.dev = dev;
    g_mmio_test.rd = rd;
    g_mmio_test.wr = wr;
    __atomic_store_n(&g_mmio_test.size, size, __ATOMIC_RELEASE);
    return 0;
#else
    (void)va; (void)dev; (void)rd; (void)wr; (void)&g_mmio_test;
    return size ? -1 : 0;
#endif
}

/* The fault handler a POSIX title would otherwise write itself (see
 * docs/runtime/README.md), with the chaining Android needs (XBA-22).
 *
 * A guest MMIO access is serviced and the thread resumes. Anything else is a
 * real crash and must reach whoever was there before us -- debuggerd on
 * Android, which writes the tombstone, or ART's fault manager in front of it
 * (libsigchain sits in front of sigaction in an app process): their handler
 * is called with the original context, or, when the previous action is the
 * default one, it is put back and the faulting instruction runs again, so
 * the crash and its backtrace are the original ones. SA_ONSTACK: bionic gives
 * every thread an alternate signal stack, which a stack overflow needs. */
#include <signal.h>
#include "platform/android_log.h"
#define XBOX_FAULT_LOG_DRAIN_MS 200
static struct sigaction g_mmio_prev_segv, g_mmio_prev_bus;
static volatile sig_atomic_t g_mmio_handler_installed;

static void mmio_fault_handler(int sig, siginfo_t *si, void *ucv)
{
    struct sigaction *prev;

    if (xbox_PosixMmioFault(ucv, (uintptr_t)si->si_addr))
        return;
    /* A real crash: let the log pump hand the last lines to logcat before
     * debuggerd writes the tombstone and the process goes (XBA-52). Bounded,
     * async-signal-safe, and immediate when the pipes are empty or no pump
     * runs (desktop) -- see xbox_log_drain in platform/android_log.h. */
    xbox_log_drain(XBOX_FAULT_LOG_DRAIN_MS);
    prev = sig == SIGBUS ? &g_mmio_prev_bus : &g_mmio_prev_segv;
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) {
            prev->sa_sigaction(sig, si, ucv);
            return;
        }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
        return;
    }
    /* Default action: restore it and return; the instruction faults again
     * and the process dies of the original signal at the original place. */
    sigaction(sig, prev, NULL);
}

int xbox_PosixInstallMmioFaultHandler(void)
{
    struct sigaction sa;

    if (g_mmio_handler_installed) {
        /* Called again (after SDL_Init, say): still first in line? If someone
         * replaced us, go back in front and chain to them instead. */
        struct sigaction cur_segv, cur_bus;
        int segv_ok, bus_ok;
        sigaction(SIGSEGV, NULL, &cur_segv);
        sigaction(SIGBUS, NULL, &cur_bus);
        segv_ok = (cur_segv.sa_flags & SA_SIGINFO) && cur_segv.sa_sigaction == mmio_fault_handler;
        bus_ok  = (cur_bus.sa_flags & SA_SIGINFO) && cur_bus.sa_sigaction == mmio_fault_handler;
        if (segv_ok && bus_ok)
            return 0;
        fprintf(stderr, "  [MMIO] fault handler replaced (SIGSEGV %s, SIGBUS %s): re-installed in front\n",
                segv_ok ? "ours" : "theirs", bus_ok ? "ours" : "theirs");
        g_mmio_handler_installed = 0;
        if (!segv_ok) g_mmio_prev_segv = cur_segv;
        if (!bus_ok)  g_mmio_prev_bus = cur_bus;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = mmio_fault_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if ((!segv_ok && sigaction(SIGSEGV, &sa, NULL) != 0) ||
            (!bus_ok && sigaction(SIGBUS, &sa, NULL) != 0))
            return -1;
        g_mmio_handler_installed = 1;
        return 0;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = mmio_fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_mmio_prev_segv) != 0)
        return -1;
    if (sigaction(SIGBUS, &sa, &g_mmio_prev_bus) != 0) {
        sigaction(SIGSEGV, &g_mmio_prev_segv, NULL);
        return -1;
    }
    g_mmio_handler_installed = 1;
    return 0;
}
#endif

/*
 * Command words the DSP stub completes instantly. A bring-up probe, not a
 * model.
 *
 * The GP and EP DSPs in src/apu are stubs -- effects bypass, encode
 * passthrough -- and a stub that never completes is worse for a title than
 * one that completes at once, because the title cannot get past it at all.
 * DDS9 posts a command and waits for the DSP to clear it:
 *
 *     mov  [ebx], 3            ; ebx = scratch + 0x810
 *   L: cmp  dword [ebx], 0
 *     jne  L
 *
 * Unlike the AC'97 reset bit, this one re-reads every iteration, so clearing
 * it from here is a race this side wins rather than loses.
 *
 * Why an environment variable rather than a registration API: the word's
 * address is reached as *(*(*(this+8)+0x10)) + 0x810 from an object with no
 * global anchor, and the APU never sees that address directly -- it reaches
 * the block through the scatter-gather descriptors the title programmed. So
 * the honest fix is for the GP stub to follow those descriptors, which is DSP
 * work. This exists to answer, in one run and without that work, whether
 * completing the command is in fact all the title is waiting for.
 *
 * RECOMP_DSP_ACK=0x804A8810[,...] -- up to 8 words, zeroed whenever non-zero.
 */
#define XBOX_MAX_DSP_ACK 8
static uint32_t g_dsp_ack[XBOX_MAX_DSP_ACK];
static int g_dsp_ack_count = 0;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_DSP_ACK");
    char buf[128], *q, *end;

    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (q = buf; *q && g_dsp_ack_count < XBOX_MAX_DSP_ACK; ) {
        unsigned long va = strtoul(q, &end, 0);
        if (end == q)
            break;
        g_dsp_ack[g_dsp_ack_count++] = (uint32_t)va;
        q = (*end == ',') ? end + 1 : end;
    }
    if (g_dsp_ack_count)
        fprintf(stderr, "  DSP ack: %d command word(s) will be completed"
                        " immediately\n", g_dsp_ack_count);
}

static int fence_readable(uint32_t va, uint32_t bytes);  /* defined below */

/* Hold a guest global at a value. A bring-up probe, like the DSP ack.
 *
 * There is exactly one reason this exists: to answer "is the title waiting on
 * this?" in one run, before spending a day making the thing that would set it
 * honestly. It is not a fix and must not be mistaken for one -- whatever it
 * holds, nothing in the guest is producing, so the state it fakes is
 * inconsistent with everything downstream of it by construction.
 *
 * RECOMP_POKE=0x30F234:1,0x30F238:1
 */
#define XBOX_MAX_POKE 8
static struct { uint32_t va, value; } g_poke[XBOX_MAX_POKE];
static int g_poke_count;

static void poke_init(void)
{
    const char *spec = getenv("RECOMP_POKE");
    char buf[192], *q, *end;

    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (q = buf; *q && g_poke_count < XBOX_MAX_POKE; ) {
        unsigned long va = strtoul(q, &end, 0);
        unsigned long val = 0;
        if (end == q)
            break;
        if (*end == ':')
            val = strtoul(end + 1, &end, 0);
        g_poke[g_poke_count].va    = (uint32_t)va;
        g_poke[g_poke_count].value = (uint32_t)val;
        g_poke_count++;
        q = (*end == ',') ? end + 1 : end;
    }
    if (g_poke_count)
        fprintf(stderr, "  POKE: holding %d guest global(s) -- bring-up probe,"
                        " not a fix\n", g_poke_count);
}

static void poke_tick(void)
{
    int i;

    for (i = 0; i < g_poke_count; i++) {
        if (!fence_readable(g_poke[i].va, 4))
            continue;
        {
            volatile uint32_t *w = (volatile uint32_t *)
                ((uintptr_t)g_poke[i].va + g_memory_offset);
            if (*w != g_poke[i].value)
                *w = g_poke[i].value;
        }
    }
}

static void dsp_ack_tick(void)
{
    int i;

    for (i = 0; i < g_dsp_ack_count; i++) {
        /* fence_readable rather than a bare bounds test: these land in the
         * contiguous window, which a plain size check against the main map
         * rejects. */
        if (!fence_readable(g_dsp_ack[i], 4))
            continue;
        {
            volatile uint32_t *w = (volatile uint32_t *)
                ((uintptr_t)g_dsp_ack[i] + g_memory_offset);
            if (*w)
                *w = 0;
        }
    }
}

static const uint32_t MCPX_COUNTERS[] = {
    0x020010,   /* APU GP sample counter, DirectSound SetupVoiceProcessor */
};

static void *g_mcpx_regs = NULL;
/* Set when the APU's registers are unmapped so they can be routed to the
 * emulated APU. Once that happens they are no longer plain memory, and the
 * counter ticking below must leave them alone -- writing through the pointer
 * faults, and the emulated APU owns those registers anyway. */
static int g_apu_mmio_trapped = 0;

/*
 * GPU completion fences the title waits on in guest memory rather than in the
 * aperture. See xbox_Nv2aMirrorFence in the header for why this is the same
 * acknowledgement the NV2A_ACK table makes, and why the address has to be
 * followed through the device struct instead of being a constant.
 */
#define XBOX_MAX_FENCE_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t put_off;
    uint32_t get_ptr_off;
} g_fence_mirrors[XBOX_MAX_FENCE_MIRRORS];
static int g_fence_mirror_count = 0;

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off)
{
    if (g_fence_mirror_count >= XBOX_MAX_FENCE_MIRRORS)
        return -1;
    g_fence_mirrors[g_fence_mirror_count].device_ptr_va = device_ptr_va;
    g_fence_mirrors[g_fence_mirror_count].put_off = put_off;
    g_fence_mirrors[g_fence_mirror_count].get_ptr_off = get_ptr_off;
    g_fence_mirror_count++;
    fprintf(stderr, "  NV2A fence mirror: device at 0x%08X,"
            " PUT +0x%X -> *(GET +0x%X)\n",
            device_ptr_va, put_off, get_ptr_off);
    return 0;
}
/* A guest address is usable only once the window is mapped and it lands
 * inside it; the chain is followed fresh every poll because the title may not
 * have built it yet. */
static int fence_readable(uint32_t va, uint32_t bytes)
{
    /* Page zero is unmapped, so the bound is the first mapped page rather than
     * just "not null": the device pointer is zero until the title creates the
     * device, and this thread polls from before that. Rejecting only 0 let
     * dev + get_ptr_off through as 0x34 and faulted on the very first tick. */
    if (g_memory_base == NULL || va < XBOX_FS_BASE)
        return 0;
    /* The contiguous window is mapped separately and sits far above the main
     * range, so a size check against g_memory_size rejects it. The fence a
     * title waits on is exactly the kind of block that lives there --
     * MmAllocateContiguousMemory is where a GPU-written semaphore comes
     * from -- so a chain ending in that window has to be followed, not
     * discarded. */
    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return g_contig_memory != NULL;
    return (size_t)va + bytes <= g_memory_size;
}

/*
 * Two counters inside the device, one of which the GPU owns.
 *
 * D3D's swap throttle is a pair: the title bumps "frames submitted" itself and
 * waits for "frames completed", which on hardware only the GPU moves. The Xbox
 * dashboard's is exactly that, at guest 0x000AF121 --
 *
 *     eax = [esi+0x2518]        ; completed
 *     ecx = [esi+0x2B60]        ; submitted
 *     ecx = ecx - eax
 *     if (ecx < 2) proceed      ; else spin on a 400-iteration delay loop
 *
 * -- and with nothing moving completed it spins there forever once two frames
 * are outstanding. That delay loop was 99.8 million of the dashboard's calls,
 * against 35 thousand for the next function down.
 *
 * This differs from xbox_Nv2aFrameCounter, which advances a counter on a 60 Hz
 * clock, in the way that matters for this pair: a free-running counter can
 * pass submitted, and then submitted - completed underflows to about four
 * billion, which is >= 2, and the spin never ends again. Mirroring cannot do
 * that, because completed is only ever whatever submitted already is.
 *
 * It is also simply true here. The pushbuffer is executed at submit, so by the
 * time the title asks whether the frame is finished, it is.
 */
#define XBOX_MAX_COUNTER_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t src_off, dst_off;
} g_counter_mirrors[XBOX_MAX_COUNTER_MIRRORS];
static int g_counter_mirror_count = 0;

int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off)
{
    if (g_counter_mirror_count >= XBOX_MAX_COUNTER_MIRRORS)
        return -1;
    g_counter_mirrors[g_counter_mirror_count].device_ptr_va = device_ptr_va;
    g_counter_mirrors[g_counter_mirror_count].src_off = src_off;
    g_counter_mirrors[g_counter_mirror_count].dst_off = dst_off;
    g_counter_mirror_count++;
    fprintf(stderr, "  NV2A counter mirror: device at 0x%08X,"
            " +0x%X -> +0x%X\n", device_ptr_va, src_off, dst_off);
    return 0;
}

static void counter_mirrors_tick(void)
{
    for (int i = 0; i < g_counter_mirror_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_counter_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_counter_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_counter_mirrors[i].src_off, 4)
                || !fence_readable(dev + g_counter_mirrors[i].dst_off, 4))
            continue;
        {
            volatile uint32_t *dst =
                (volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].dst_off)
                                      + g_memory_offset);
            uint32_t src =
                *(volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].src_off)
                                       + g_memory_offset);
            if (*dst != src)
                *dst = src;
        }
    }
}

/*
 * Frame counters the title polls to pace itself.
 *
 * D3D keeps a swap count inside the device and bumps it once per presented
 * frame; a title that wants to wait a frame reads it and spins until it moves.
 * Wreckless does exactly that at guest 0x000DC5E0 -- "loop while the counter
 * has advanced by less than 2" -- so a counter that never moves is not a
 * dropped frame, it is a hang with a full asset load behind it.
 *
 * Nothing here presents, so nothing would ever move it. Advancing it on a
 * clock is what makes the wait terminate, and 60 Hz is the rate the title
 * expects the display to run at. Followed through the device pointer for the
 * same reason the fence is: the device is allocated at runtime.
 */
#define XBOX_MAX_FRAME_COUNTERS 4
#define XBOX_FRAME_PERIOD_MS    16      /* ~60 Hz */

/* A counter can belong to the title after all. A title may register the
 * miniport's own vblank count, which D3D's vblank DPC bumps itself once
 * vblanks reach the title (RECOMP_VBLANK); bumped here per flip as well, it
 * ran at 60 Hz plus the flip rate, and video and loads timed on it played
 * several times too fast. So under the flip hold each bump first checks
 * whether something else moved the counter since the last one; one that
 * keeps moving on its own is the title's and is left alone for as long as
 * it keeps moving (nv2a_flip_hold.h). The first bump only seeds: a field
 * D3D initialised is not the title counting. */
static struct {
    uint32_t device_ptr_va;
    uint32_t counter_off;
    XboxFrameCounterBump bump;
} g_frame_counters[XBOX_MAX_FRAME_COUNTERS];
static int   g_frame_counter_count = 0;
static DWORD g_frame_counter_last_ms = 0;
static uint32_t g_frame_counters_owned;

extern int nv2a_pb_exec_flip_pacing(void);

/* Registered counters the title is moving itself right now. */
uint32_t xbox_Nv2aFrameCountersOwned(void) { return g_frame_counters_owned; }

static void frame_counter_bump(int i)
{
    uint32_t dev;
    volatile uint32_t *p;
    int r;

    if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
        return;
    dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                 + g_memory_offset);
    if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
        return;
    p = (volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                              + g_memory_offset);
    r = xbox_FrameCounterBump(&g_frame_counters[i].bump, p,
                              nv2a_pb_exec_flip_pacing(), GetTickCount());
    if (r == XBOX_FRAME_COUNTER_HANDED_OVER) {
        g_frame_counters_owned++;
        fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X keeps"
                " moving on its own (0x%08X): the title counts it, the"
                " toolkit stops\n", g_frame_counters[i].device_ptr_va,
                g_frame_counters[i].counter_off, *p);
    } else if (r == XBOX_FRAME_COUNTER_HANDED_BACK) {
        g_frame_counters_owned--;
        fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X has"
                " stopped moving on its own: the toolkit bumps it again\n",
                g_frame_counters[i].device_ptr_va,
                g_frame_counters[i].counter_off);
    }
}

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off)
{
    if (g_frame_counter_count >= XBOX_MAX_FRAME_COUNTERS)
        return -1;
    g_frame_counters[g_frame_counter_count].device_ptr_va = device_ptr_va;
    g_frame_counters[g_frame_counter_count].counter_off   = counter_off;
    g_frame_counter_count++;
    fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X @ %d Hz\n",
            device_ptr_va, counter_off, 1000 / XBOX_FRAME_PERIOD_MS);
    return 0;
}

/* A real swap happened: advance every registered counter, and remember when.
 *
 * The timer below exists for a title nothing presents for. Once the
 * pushbuffer executor is actually running flips, the timer is the wrong
 * clock and an actively harmful one: Half-Life 2's loader paces its intro on
 * this count, so a 62 Hz timer against an executor managing a fraction of a
 * frame per second ran the video forward in virtual time far faster than it
 * could be drawn. Only every few hundredth frame was ever presented, each one
 * sampled part way through its own decode -- which looks exactly like a
 * stalling, blocky video rather than a clock running away.
 */
static DWORD g_frame_counter_flip_ms;

void xbox_Nv2aFrameCounterFlip(void)
{
    int i;

    g_frame_counter_flip_ms = GetTickCount();
    if (!g_frame_counter_flip_ms)
        g_frame_counter_flip_ms = 1;          /* 0 means "never" */
    for (i = 0; i < g_frame_counter_count; i++)
        frame_counter_bump(i);
}

static void frame_counters_tick(void)
{
    DWORD now = GetTickCount();
    int i;

    if (!g_frame_counter_count)
        return;
    if (g_frame_counter_last_ms
            && (now - g_frame_counter_last_ms) < XBOX_FRAME_PERIOD_MS)
        return;
    /* Something is presenting: let it drive the count instead. Two seconds,
     * because the executor's flips are not evenly spaced and a title that
     * genuinely stops presenting still has to be got moving again. */
    if (g_frame_counter_flip_ms && (now - g_frame_counter_flip_ms) < 2000) {
        g_frame_counter_last_ms = now;
        return;
    }
    g_frame_counter_last_ms = now;

    for (i = 0; i < g_frame_counter_count; i++)
        frame_counter_bump(i);
}

/* Guest VA of the title's GPU fence word, found through the first registered
 * mirror, or 0 when none is registered or the chain does not resolve into the
 * contiguous window (where a GPU-visible word has to be). Read-only: the
 * pushbuffer executor uses it to keep the NV2A's fence latch consistent with
 * what the mirror writes. */
uint32_t xbox_Nv2aFenceWordVa(void)
{
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (get_ptr >= 0x80000000u && get_ptr - 0x80000000u <= 0x04000000u - 4u
                && !(get_ptr & 3u) && fence_readable(get_ptr, 4))
            return get_ptr;
    }
    return 0;
}

static void fence_mirrors_tick(void)
{
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4)
                || !fence_readable(dev + g_fence_mirrors[i].put_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (!fence_readable(get_ptr, 4))
            continue;
        {
            volatile uint32_t *fence =
                (volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset);
            uint32_t put =
                *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].put_off)
                                       + g_memory_offset);
            if (*fence != put)
                *fence = put;
        }
    }
}

static int s_nv2a_trace = 0;
/* The per-kick DMA_PUT line, only on an explicit RECOMP_NV2A_TRACE. The
 * executor sets s_nv2a_trace too, and a line plus an fflush per kick is
 * hundreds of stderr writes a second on the thread the title waits on. */
static int s_nv2a_trace_kicks = 0;

/* The display framebuffer, as reported by AvSetDisplayMode. Checksummed once a
 * second so a run can answer the only question that matters before building a
 * presenter: is the guest putting pixels anywhere at all, and do they change
 * from frame to frame. */
static uint32_t s_fb_va, s_fb_pitch, s_fb_height = 480;

void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    s_fb_pitch = pitch;
}

/* Where the title last said its framebuffer is, for anything that wants to read
 * guest pixels directly. There was only a setter, so every such reader carried
 * its own hardcoded address instead -- and a title that moves its framebuffer
 * (which is most of them, once it owns one) left that constant pointing at
 * uninitialised memory. A dump taken there is not empty, it is noise, which
 * reads as "the title drew garbage" rather than "you read the wrong page".
 *
 * This is the resolved address, not the physical one AvSetDisplayMode states:
 * the caller resolves before storing, because a physical framebuffer address
 * read directly lands in the loaded image. Getting that wrong is the same bug
 * twice over -- once in the probe below, once in a caller of this. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch)
{
    if (pitch)
        *pitch = s_fb_pitch;
    return s_fb_va;
}

static void framebuffer_probe_tick(void)
{
    static DWORD last_ms;
    static uint32_t last_sum;
    DWORD now = GetTickCount();
    uint32_t sum = 0, nonzero = 0, i, n;
    const uint32_t *p;

    if (!s_nv2a_trace || !s_fb_va || !s_fb_pitch)
        return;
    if (last_ms && (now - last_ms) < 1000)
        return;
    last_ms = now;
    if ((size_t)s_fb_va + s_fb_pitch * s_fb_height > g_memory_size)
        return;
    p = (const uint32_t *)((uintptr_t)s_fb_va + g_memory_offset);
    n = (s_fb_pitch * s_fb_height) / 4;
    for (i = 0; i < n; i++) {
        sum = sum * 33u + p[i];
        if (p[i]) nonzero++;
    }
    fprintf(stderr, "  [FB] 0x%08X sum=%08X nonzero=%u/%u %s\n",
            s_fb_va, sum, nonzero, n,
            sum != last_sum ? "CHANGED" : "same");
    last_sum = sum;
    fflush(stderr);
}

/* The PFB flush kick (NV2A_ACK above), which D3D's KickOff waits on. */
#define NV2A_ACK_PFB_FLUSH          0x100410u
/* The vblank's two status bits, which the vblank-ack thread owns. */
#define NV2A_ACK_PMC_INTR_0         0x000100u
#define NV2A_ACK_PMC_INTR_PCRTC     (1u << 24)
#define NV2A_ACK_PCRTC_INTR_0       0x600100u
#define NV2A_ACK_PCRTC_INTR_VBLANK  (1u << 0)

/* ---- The PFB flush kick as events (xbox_Nv2aKickWait) -----------------
 *
 * D3D's KickOff sets the PFB flush bit and spins until the GPU clears it,
 * then writes DMA_PUT. Nothing can see that store from translated code, so
 * the guest polled (yield, then 50 us sleeps that wake late on a loaded
 * host) and the ack thread polled back (Sleep(0)) while it waited for the
 * next kick: in Fable's Oakvale 85 % of the main thread and 7-18 % of the
 * executor went into the two polls. A title override that calls
 * xbox_Nv2aKickWait turns them into two conditions:
 *   - "kicked": the guest set the bit. The ack thread, with nothing to walk,
 *     waits on it instead of polling (nv2a_ack_wait);
 *   - "acked": the ack thread cleared the bit (nv2a_ack_busy_bits), so the
 *     waiting guest goes on at once.
 * Every wait is timed (1 ms for the guest, the ack thread's 250 us cap), so
 * a lost wake-up costs that much, never a hang. Lost wake-ups are avoided
 * all the same: the guest counts itself a waiter before it looks at the bit,
 * and the ack thread looks at the waiters after it cleared the bit, both
 * sequentially consistent, so either the guest sees the bit clear or the ack
 * thread sees the waiter and broadcasts under the mutex.
 *
 * Titles that never call it keep the polls exactly as before; so does
 * Windows. */
#if !defined(_WIN32)
#include <pthread.h>
#include <sched.h>
static pthread_mutex_t s_kick_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_kick_kicked = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  s_kick_acked = PTHREAD_COND_INITIALIZER;
static int      s_kick_events;            /* a title waits through xbox_Nv2aKickWait */
static int      s_kick_waiters;           /* guests waiting on "acked" */
static int      s_kick_idle;              /* the ack thread waits on "kicked" */
static uint32_t s_kick_seq;               /* kicks signalled */
static uint32_t s_kick_seen;              /* the ack thread's view of s_kick_seq */
static uint64_t s_kick_released_ns;       /* the last ack that released a waiter */
/* After an ack released a waiter, how long the ack thread polls for the
 * DMA_PUT that follows (a woken thread's latency on a loaded host). */
#define NV2A_KICK_PUT_NS  (200u * 1000u)

static void kick_deadline(struct timespec *ts, long ns)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += ns;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}
#endif

/* The guest side: set `mask` in the register and wait until the ack thread
 * clears it. Same register traffic as D3D's own loop. */
void xbox_Nv2aKickWait(volatile uint32_t *reg, uint32_t mask)
{
#if defined(_WIN32)
    InterlockedOr((volatile LONG *)reg, (LONG)mask);
    while (*reg & mask)
        Sleep(0);
#else
    __atomic_fetch_or(reg, mask, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&s_kick_mu);
    s_kick_events = 1;
    s_kick_seq++;
    if (s_kick_idle)
        pthread_cond_signal(&s_kick_kicked);
    for (;;) {
        struct timespec ts;
        __atomic_add_fetch(&s_kick_waiters, 1, __ATOMIC_SEQ_CST);
        if (!(__atomic_load_n(reg, __ATOMIC_SEQ_CST) & mask)) {
            __atomic_sub_fetch(&s_kick_waiters, 1, __ATOMIC_SEQ_CST);
            break;
        }
        kick_deadline(&ts, 1000000L);     /* 1 ms: the lost-wake-up bound */
        pthread_cond_timedwait(&s_kick_acked, &s_kick_mu, &ts);
        __atomic_sub_fetch(&s_kick_waiters, 1, __ATOMIC_SEQ_CST);
    }
    pthread_mutex_unlock(&s_kick_mu);
#endif
}

/* RECOMP_POLL_WAIT: lifted code is in a loop that only polls `word` for
 * `mask` (recomp_types.h). The one word the runtime answers by events is the
 * PFB flush kick: D3D's KickOff set bit 16 and waits for the ack thread to
 * clear it, in every XDK D3D8 build, so the title needs no override to get
 * xbox_Nv2aKickWait. The bit is set already; setting it again changes
 * nothing. Any other word: the loop polls as it always did.
 * RECOMP_POLL_WAIT=0 keeps the bare spin for the flush kick too.
 * Returns 1 when it waited for the ack thread, 0 when it returned at once. */
int xbox_GuestPollWait(volatile uint32_t *word, uint32_t mask, int while_set,
                       uint32_t va)
{
    static int on = -1;

    (void)va;
    if (on < 0) {
        const char *e = getenv("RECOMP_POLL_WAIT");
        on = !(e && e[0] == '0');
    }
    if (on && while_set && mask == 0x00010000u && g_nv2a_memory
        && word == (volatile uint32_t *)((char *)g_nv2a_memory + 0x100410u)) {
        xbox_Nv2aKickWait(word, mask);
        return 1;
    }
    return 0;
}

/* The ack thread cleared the PFB flush bit: wake whoever waits for it. */
static void kick_acked(void)
{
#if !defined(_WIN32)
    if (!__atomic_load_n(&s_kick_waiters, __ATOMIC_SEQ_CST))
        return;
    pthread_mutex_lock(&s_kick_mu);
    s_kick_released_ns = xbox_HostNowNs();
    pthread_cond_broadcast(&s_kick_acked);
    pthread_mutex_unlock(&s_kick_mu);
#endif
}

/* Clear the request bits "hardware" would clear on its own.
 *
 * Also called by the pushbuffer walker between commands. This thread executes
 * the pushbuffer below, and under RECOMP_PB_EXEC one pass can take a whole
 * rendered frame; with the bits cleared only here, every XDK D3D kickoff --
 * which sets 0x100410 bit 16 and spins until it clears -- waited that long,
 * and a level load, thousands of kickoffs long, looked frozen. Clearing them
 * from inside the walk answers the kickoff within a few hundred commands,
 * without a thread of its own spinning on a host core. */
/* PVIDEO's STOP handshake (NV_PVIDEO_STOP 0x8704, NV_PVIDEO_BUFFER 0x8700).
 * STOP is a command: bit 0 asks the overlay to stop, and the hardware then
 * empties BUFFER, the banks pending for scan-out. XDK D3D's
 * D3DDevice_EnableOverlay(FALSE) writes STOP and spins until BUFFER reads 0
 * (KOTOR's film thread at 0x00352CE1). The Win32 MMIO hook models the write
 * (nv2a_core.c, pvideo_write); against the plain memory of a POSIX host it is
 * answered here, from the ack pass, and not from the vblank: the spinning
 * thread may hold the one-CPU gate the vblank waits for. The stop count tells
 * the overlay scan-out (src/video/pvideo_posix.c) that the overlay went down,
 * since by the time it looks STOP reads 0 again. */
#define NV2A_PVIDEO_BLOCK  0x8000u
#define NV2A_PVIDEO_BUFFER 0x0700u
#define NV2A_PVIDEO_STOP   0x0704u
static volatile LONG s_pvideo_stops;

int xbox_Nv2aPvideoStop(volatile uint32_t *pvideo)
{
    if (!(pvideo[NV2A_PVIDEO_STOP / 4] & 1u))
        return 0;
    pvideo[NV2A_PVIDEO_BUFFER / 4] = 0;
    pvideo[NV2A_PVIDEO_STOP / 4] = 0;
    InterlockedIncrement(&s_pvideo_stops);      /* a full barrier: the clears first */
    return 1;
}

uint32_t xbox_Nv2aPvideoStops(void)
{
    return (uint32_t)InterlockedCompareExchange(&s_pvideo_stops, 0, 0);
}

static void nv2a_ack_busy_bits(int flip_held)
{
    volatile uint32_t *regs = (volatile uint32_t *)g_nv2a_memory;

    if (!regs)
        return;
#if !defined(_WIN32)
    (void)xbox_Nv2aPvideoStop(regs + NV2A_PVIDEO_BLOCK / 4);
#endif
    for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
        volatile uint32_t *r =
            (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);
        uint32_t mask = NV2A_ACK[i].busy_mask;
        if (flip_held && NV2A_ACK[i].offset == NV2A_ACK_PFB_FLUSH)
            continue;
        /* Once the vblank is delivered its two bits belong to the
         * vblank-ack thread (kernel_bridge.c), which clears them on the
         * DPC's ack. Cleared here too, a pass that landed between the
         * vblank and the DPC's read of PMC_INTR_0 took the vblank away
         * from the DPC: no ack, no count, and the title's present at
         * vblank slipped a frame. */
        if (xbox_VblankAckHeld()) {
            if (NV2A_ACK[i].offset == NV2A_ACK_PMC_INTR_0)
                mask &= ~NV2A_ACK_PMC_INTR_PCRTC;
            else if (NV2A_ACK[i].offset == NV2A_ACK_PCRTC_INTR_0)
                mask &= ~NV2A_ACK_PCRTC_INTR_VBLANK;
        }
        /* Interlocked: the vblank-ack thread and the guest change other
         * bits of the same word meanwhile. */
        if (*r & mask) {
            InterlockedAnd((volatile LONG *)r, (LONG)~mask);
            if (NV2A_ACK[i].offset == NV2A_ACK_PFB_FLUSH)
                kick_acked();
        }
    }
}

void xbox_Nv2aAckBusyBits(void)
{
    nv2a_ack_busy_bits(0);
}

/* One word of the NV2A register file (offset from the register base), or NULL
 * before it is mapped. The pushbuffer executor writes what the GPU would when
 * it executes a command: PGRAPH state a title reads back, and DMA_GET. */
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    if (!g_nv2a_memory || offset + 4 > XBOX_NV2A_SIZE)
        return NULL;
    return (volatile uint32_t *)((char *)g_nv2a_memory + offset);
}

/* Does the ack pass below have anything to answer right now? A new PUT (the
 * title kicked off), or a bit nv2a_ack_busy_bits would clear (a PFB flush
 * kick being waited on, an interrupt status the guest just wrote), or an
 * idle bit the guest cleared. */
static int nv2a_ack_wanted(volatile uint32_t *regs, uint32_t put_seen)
{
    size_t i;

    if (regs[NV2A_USER_DMA_PUT / 4] != put_seen)
        return 1;
#if !defined(_WIN32)
    if (regs[(NV2A_PVIDEO_BLOCK + NV2A_PVIDEO_STOP) / 4] & 1u)
        return 1;                           /* an EnableOverlay(FALSE) spins on it */
#endif
    for (i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
        uint32_t mask = NV2A_ACK[i].busy_mask;
        if (NV2A_ACK[i].offset == NV2A_ACK_PMC_INTR_0)
            mask &= ~NV2A_ACK_PMC_INTR_PCRTC;      /* the vblank-ack thread's */
        else if (NV2A_ACK[i].offset == NV2A_ACK_PCRTC_INTR_0)
            mask &= ~NV2A_ACK_PCRTC_INTR_VBLANK;
        if (regs[NV2A_ACK[i].offset / 4] & mask)
            return 1;
    }
    for (i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++)
        if ((regs[NV2A_IDLE[i].offset / 4] & NV2A_IDLE[i].idle_mask)
            != NV2A_IDLE[i].idle_mask)
            return 1;
    return 0;
}

/* The ack thread's wait between two passes.
 *
 * It used to be a bare yield, so with nothing submitted the thread still ran
 * flat out: on Fable's title screen 43 % of a core in SwitchToThread, a core
 * an Android handheld with five fast ones would rather give the rasteriser.
 * Nothing can wake it instead: PUT and the busy bits are plain memory the
 * title writes from translated code, so there is no store to hook. The wait
 * is therefore a bounded poll:
 *   - while there is work, or for NV2A_ACK_SPIN_NS after the last of it
 *     (D3D kicks off in bursts), it yields as before, so a burst is answered
 *     at once;
 *   - after that, and while a flip waits for its vblank (a wait on the
 *     clock, not on the title), it sleeps in steps of NV2A_ACK_STEP_US,
 *     looking at PUT and the busy bits after each, for at most
 *     RECOMP_ACK_IDLE_US before running a pass anyway, which keeps the
 *     clocks this thread ticks (KeTickCount, the counter mirrors) moving.
 * A kick that lands in an idle period waits one step (tens of microseconds,
 * more on a loaded host) more than before.
 *
 * Default 250 us (RECOMP_ACK_IDLE_US=0 restores the bare yield). Before the
 * segment scan followed jumps it learnt the ring's end from the highest PUT
 * it had seen, so a thread that saw fewer PUTs lost commands at a wrap and
 * Fable stopped at 0 flips in most starts; with the jump-following scan it
 * started 6 of 6. Windows keeps the yield: its sleeps are a millisecond or
 * more.
 *
 * A title that kicks through xbox_Nv2aKickWait (above) can wake it: then,
 * outside a flip hold, the thread waits on "kicked" for at most the same
 * RECOMP_ACK_IDLE_US instead of yielding, in the same 50 us steps (each
 * ended early by a kick), and yields only for the DMA_PUT that follows an
 * ack which released a waiter. */
/* The pushbuffer hand-over on a weakly ordered host (XBA-81, RECOMP_PUT_BARRIER).
 *
 * The title writes the pushbuffer words, then DMA_PUT, with plain stores; this
 * thread reads PUT and walks the words. x86 keeps the two stores in order for
 * every other core; arm64 may let PUT be seen first, and the walk would read
 * a word not yet written. A fence here alone cannot help: the writer's stores
 * are what may be out of order. membarrier(PRIVATE_EXPEDITED) makes every
 * thread of the process pass a full barrier before it returns, so once it has,
 * each word stored before the PUT just seen is visible.
 *
 * Opt-in (RECOMP_PUT_BARRIER=1), Linux and Android only: one IPI per new PUT
 * costs, the one-core pin of the guest threads already covers guest-to-guest
 * order, and whether app seccomp allows the call varies with the Android
 * release. Without it: an acquire fence, which orders this thread's reads. */
#if defined(__linux__)
#include <sys/syscall.h>
#ifndef MEMBARRIER_CMD_PRIVATE_EXPEDITED
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED (1 << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED (1 << 4)
#endif
#endif
static void nv2a_put_barrier(void)
{
#if defined(__linux__) && defined(__NR_membarrier)
    static int state = -1;              /* -1 unknown, 0 off, 1 on */
    if (state < 0) {
        const char *e = getenv("RECOMP_PUT_BARRIER");
        state = 0;
        if (e && *e == '1') {
            if (syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0) {
                state = 1;
                fprintf(stderr, "  [NV2A] RECOMP_PUT_BARRIER: membarrier at each new DMA_PUT\n");
            } else {
                fprintf(stderr, "  [NV2A] RECOMP_PUT_BARRIER: membarrier unavailable, ignored\n");
            }
        }
    }
    if (state == 1)
        (void)syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0);
#endif
#if defined(_MSC_VER)
    MemoryBarrier();
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

#define NV2A_ACK_SPIN_NS  (2u * 1000u * 1000u)
#define NV2A_ACK_STEP_US  50
static void nv2a_ack_wait(volatile uint32_t *regs, int flip_held)
{
#if defined(_WIN32)
    (void)regs; (void)flip_held;
    Sleep(0);
#else
    static int idle_us = -1;
    static uint64_t busy_ns;               /* the last pass that had work */
    static uint32_t put_seen;
    uint32_t put = regs[NV2A_USER_DMA_PUT / 4];
    uint64_t now = xbox_HostNowNs();
    int waited;

    if (idle_us < 0) {
        const char *e = getenv("RECOMP_ACK_IDLE_US");
        idle_us = e ? atoi(e) : 250;
        if (idle_us < 0) idle_us = 0;
        if (idle_us > 20000) idle_us = 20000;
    }
    if (put != put_seen || regs[NV2A_USER_DMA_GET / 4] != put) {
        put_seen = put;
        if (!flip_held)
            busy_ns = now;                 /* a kick, or one not yet answered */
    }
    if (idle_us && !flip_held && __atomic_load_n(&s_kick_events, __ATOMIC_RELAXED)) {
        /* The title kicks through xbox_Nv2aKickWait: wait for "kicked"
         * rather than poll. Except right after an ack released a waiter:
         * KickOff writes DMA_PUT just after its wait, and that store wakes
         * nothing, so it is polled for, briefly. */
        struct timespec ts;
        if (regs[NV2A_USER_DMA_GET / 4] != put || nv2a_ack_wanted(regs, put))
            return;
        if (now - __atomic_load_n(&s_kick_released_ns, __ATOMIC_RELAXED)
                < NV2A_KICK_PUT_NS) {
            sched_yield();
            return;
        }
        /* In NV2A_ACK_STEP_US steps, looking at the busy bits and PUT after
         * each as the sleeps below do: a title also writes PUT or an
         * interrupt status without a kick, and Fable's loader waits on
         * those (a single 250 us wait made its loading screen 3-10 fps
         * and once froze it). */
        for (waited = 0; waited < idle_us; waited += NV2A_ACK_STEP_US) {
            int kicked;
            pthread_mutex_lock(&s_kick_mu);
            if (s_kick_seq == s_kick_seen && !nv2a_ack_wanted(regs, put)) {
                s_kick_idle = 1;
                kick_deadline(&ts, NV2A_ACK_STEP_US * 1000L);
                pthread_cond_timedwait(&s_kick_kicked, &s_kick_mu, &ts);
                s_kick_idle = 0;
            }
            kicked = s_kick_seq != s_kick_seen;
            s_kick_seen = s_kick_seq;
            pthread_mutex_unlock(&s_kick_mu);
            if (kicked || regs[NV2A_USER_DMA_GET / 4] != put
                    || nv2a_ack_wanted(regs, put))
                return;
        }
        return;
    }
    if (!idle_us || (!flip_held && now - busy_ns < NV2A_ACK_SPIN_NS)) {
        Sleep(0);
        return;
    }
    for (waited = 0; waited < idle_us; waited += NV2A_ACK_STEP_US) {
        struct timespec ts = { 0, NV2A_ACK_STEP_US * 1000 };
        nanosleep(&ts, NULL);
        if (nv2a_ack_wanted(regs, put)) {
            busy_ns = xbox_HostNowNs();
            return;
        }
    }
#endif
}

static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        /* A flip held for its vblank (nv2a_flip_hold.h): this pass walks
         * nothing, leaves DMA_GET where it is and acknowledges no PFB flush,
         * which is how the title feels the console's FLIP_STALL. Nothing is
         * locked while it waits; the release depends only on the vblank
         * count and the clock.
         *
         * The hold is per submitted segment: the walk that arms it finishes
         * its segment and sets GET to PUT, and the next segment waits. A
         * D3D present kicks off right after its stall, so the stall ends
         * its segment and the two are the same; a segment carrying two
         * flips paces only the last. */
        extern int nv2a_pb_exec_flip_poll(void);
        int flip_held = nv2a_pb_exec_flip_poll();
        nv2a_ack_busy_bits(flip_held);
        for (size_t i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset);
            if ((*r & NV2A_IDLE[i].idle_mask) != NV2A_IDLE[i].idle_mask) {
                *r |= NV2A_IDLE[i].idle_mask;
            }
        }
        /* DMA_GET used to be set to DMA_PUT here, at the top of the tick,
         * before the scan below had executed anything.
         *
         * GET is what tells the title how far the GPU has consumed, and D3D
         * waits on it before reusing the ring. Reporting "all consumed"
         * while the commands were still unread gave the title permission to
         * overwrite them, and it took it: the executor then read whatever
         * part of the segment had survived, so each pass drew a different
         * subset of the frame. It looks like unstable geometry and is a
         * lost-command race.
         *
         * The advance now happens after the scan, further down, which is
         * also the only ordering that gives the title real back-pressure. */
        fence_mirrors_tick();
        dsp_ack_tick();
        poke_tick();
        counter_mirrors_tick();
        frame_counters_tick();
        framebuffer_probe_tick();

        /* Which framebuffer the display would be scanning out.
         *
         * PCRTC_START holds the address the CRTC reads pixels from, so
         * whatever the title last set there is the frame it believes is on
         * screen. Nothing here scans out, so this is the one place that says
         * whether the guest is producing an image at all -- and where it is.
         * Gated, because it is a bring-up question, not a runtime one. */
        /* Not gated on the trace flag: nv2a_pb_scan is what drives the
         * executor, and it already returns unless RECOMP_PB_SCAN or
         * RECOMP_PB_EXEC asked for it. Gating the call as well meant
         * RECOMP_PB_EXEC on its own did nothing at all, and the executor
         * only ran when someone happened to also be tracing. */
        {
            /* Is the title submitting GPU work at all? PUT is where the
             * title's pushbuffer writer has got to; if it never moves, nothing
             * is being drawn and the missing piece is upstream of the GPU. */
            static DWORD  last_put_ms;
            static uint32_t last_put;
            static uint32_t get_written = 0xFFFFFFFFu;  /* GET as we last left it */
            DWORD now_ms = GetTickCount();
            uint32_t put = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            int walked = 0;
            if (!flip_held
                    && (put != last_put || (now_ms - last_put_ms) > 2000)) {
                walked = 1;
                /* Survey the segment the title just submitted, once. */
                {
                    extern void nv2a_pb_scan(uint32_t);
                    extern void nv2a_pb_scan_report(void);
                    static DWORD last_report;

                    /* DMA_PUT holds a PHYSICAL address -- Xbox D3D writes
                     * `VA & 0x0FFFFFFF` and reads the GPU's position back as
                     * `GET | 0x80000000`. nv2a_pb_scan reads guest VAs, so
                     * handing it the raw register value pointed it at low
                     * memory: for the Xbox Dashboard, whose pushbuffer is at
                     * 0x80001000, PUT reads 0x1000 and the survey walked the
                     * fake TIB. It reported a plausible-looking inventory of
                     * nothing, which is worse than reporting none -- the
                     * conclusion drawn was "the title submits no methods"
                     * while it was submitting them the whole time.
                     *
                     * The contiguous window IS the physical-address view, so
                     * OR-ing its base is the documented round trip, not a
                     * guess. */
                    extern void nv2a_pb_resync(uint32_t);
                    extern int  nv2a_pb_dma_walk(void);
                    extern void nv2a_pb_scan_segment(uint32_t, uint32_t);

                    /* The words before PUT, as the title stored them (XBA-81). */
                    if (put != last_put)
                        nv2a_put_barrier();

                    if (nv2a_pb_dma_walk()) {
                        uint32_t get_now = *(volatile uint32_t *)
                                           ((char *)regs + NV2A_USER_DMA_GET);
                        /* GET is ours to advance; if it is not what we last
                         * wrote, the title reset the ring. The first time, it
                         * is where D3D started the ring: walking from there
                         * rather than from the first PUT keeps the one-time
                         * device state (depth function, and so on) sent
                         * before it. */
                        if (get_written == 0xFFFFFFFFu || get_now != get_written)
                            nv2a_pb_resync(get_now);
                        if (put != last_put)
                            nv2a_pb_scan(put);
                    } else {
                        /* The segment scan, the default: what the title
                         * submitted since the previous PUT, one walk from
                         * there to this PUT. The walk follows JUMPs, so a
                         * wrap of the ring (D3D's JUMP back to its base, PUT
                         * coming back below where it was) and a recorded
                         * pushbuffer run from the ring are both part of it.
                         * It used to scan up to the highest PUT seen on a
                         * wrap and stop at every JUMP, which dropped the
                         * commands past either.
                         *
                         * A title that moved GET itself reset the ring
                         * (XDK D3D writes DMA_GET and DMA_PUT together):
                         * its commands start at that GET, not at the old
                         * PUT. */
                        uint32_t from = last_put;
                        uint32_t get_now = *(volatile uint32_t *)
                                           ((char *)regs + NV2A_USER_DMA_GET);
                        if (get_written != 0xFFFFFFFFu && get_now != get_written)
                            from = get_now;
                        if (last_put && put != from)
                            nv2a_pb_scan_segment(XBOX_CONTIG_BASE | (from & 0x0FFFFFFFu),
                                                 XBOX_CONTIG_BASE | (put  & 0x0FFFFFFFu));
                    }
                    /* Periodic, because what the title submits at init is not
                     * what it submits once it is drawing a menu, and the
                     * question the survey answers is about the latter. */
                    /* RECOMP_PB_REPORT_MS shortens it: the report is also
                     * when RECOMP_FB_DUMP writes a frame, and stepping a
                     * scripted pad through a menu needs a picture per
                     * press rather than one every ten seconds. */
                    static long report_ms = -1;
                    if (report_ms < 0) {
                        const char *e = getenv("RECOMP_PB_REPORT_MS");
                        report_ms = e ? atol(e) : 10000;
                        if (report_ms < 100) report_ms = 100;
                    }
                    if (s_nv2a_trace && now_ms - last_report > (uint64_t)report_ms) {
                        last_report = now_ms;
                        nv2a_pb_scan_report();
                    }
                }
                /* Consumed, now that it has actually been executed. */
                {
                    volatile uint32_t *get =
                        (volatile uint32_t *)((char *)regs
                                              + NV2A_USER_DMA_GET);
                    *get = put;
                    get_written = put;
                }
                last_put = put; last_put_ms = now_ms;
                /* GET as well as PUT. A title that stops submitting has either
                 * finished or is spinning on the GPU catching up, and only GET
                 * tells those apart -- D3D waits for GET to reach PUT before it
                 * reuses the buffer, so GET stuck behind PUT is the shape of a
                 * pushbuffer-full hang. Also show the same pair as the Xbox
                 * Dashboard reads them: its D3D holds a register-block pointer
                 * in its device struct rather than assuming 0xFD800000, and
                 * mirroring the wrong block leaves it spinning on a GET that
                 * never moves. */
                if (s_nv2a_trace_kicks) {
                    uint32_t g = *(volatile uint32_t *)
                                 ((char *)regs + NV2A_USER_DMA_GET);
                    fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X  DMA_GET = "
                            "0x%08X%s\n", put, g,
                            g == put ? "" : "  (GPU behind)");
                    fflush(stderr);
                }
            }
            /* Caught up with PUT: visibility reports the title may be
             * waiting for are resolved now (nv2a_backend.h). */
            if (!walked) {
                extern void nv2a_pb_exec_idle(void);
                nv2a_pb_exec_idle();
            }
        }
        if (s_nv2a_trace) {
            static uint32_t last_start = 0xFFFFFFFFu;
            uint32_t start = *(volatile uint32_t *)((char *)regs + 0x600800);
            if (start != last_start) {
                last_start = start;
                fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
                fflush(stderr);
            }
        }

        if (g_mcpx_regs && !g_apu_mmio_trapped) {
            for (size_t i = 0; i < sizeof(MCPX_COUNTERS) / sizeof(MCPX_COUNTERS[0]); i++) {
                volatile uint32_t *c =
                    (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]);
                *c += 1;
            }
        }

        /* Advance KeTickCount. It was written once at init and left frozen,
         * which silently breaks every timeout that polls it: Halo's DHCP setup
         * waits on a tick deadline that never arrives and spins forever bringing
         * up XNet. A live clock is also just the truth -- KeTickCount ticks on
         * hardware whether or not anyone is asleep. GetTickCount() shares the
         * millisecond unit, so the rate matches. */
        *(volatile uint32_t *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
                               + g_memory_offset) = GetTickCount();

        nv2a_ack_wait(regs, flip_held);
    }
    return 0;
}

static void xbox_Nv2aAckStart(void)
{
    dsp_ack_init();
    poke_init();
    g_nv2a_ack_stop = 0;
    g_nv2a_ack_thread = CreateThread(NULL, 0, nv2a_ack_thread,
                                     g_nv2a_memory, 0, NULL);
    if (g_nv2a_ack_thread) {
        fprintf(stderr, "  NV2A busy-bit ack: %zu register(s) acknowledged\n",
                sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]));
    }
}

/* Separate allocation for Xbox kernel address space (0x80010000+).
 * Some RenderWare code reads the kernel PE header to detect features. */
static void *g_kernel_memory = NULL;

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Bounds of the title's executable sections, from its own XBE section table.
 *
 * RECOMP_ICALL uses these to decide whether an indirect-call target is code
 * before dispatching it. This used to be a hardcoded "0x00400000..0xFE000000 is
 * not code" test, which is true for Burnout 3 -- its .text ends at 0x002CC200,
 * so everything above 0x400000 really is data -- and false for any title with
 * more code than that. Half-Life 2's .text runs to 0x005F4A6C, so the constant
 * silently discarded every indirect call into the top two thirds of the game,
 * including the one that enters its main. No log, no crash: eax = 0 and carry
 * on, which looks exactly like a function that returned early.
 *
 * Zero until the layout is initialised, which the macro treats as "allow" so
 * nothing breaks before the title is loaded. */
uint32_t g_xbox_image_lo = 0;
uint32_t g_xbox_image_hi = 0;

/* Toolkit region placement (xbox_memory_layout.h). The initial values are the
 * small-image layout; xbox_MemoryLayoutInit recomputes them from the image. */
uint32_t g_xbox_region_base = XBOX_REGION_MIN;
uint32_t g_xbox_stack_base  = XBOX_REGION_MIN + XBOX_REGION_STACK_OFF;
uint32_t g_xbox_stack_size  = 8u * 1024 * 1024;
/* recomp_types.h's lock for a cmpxchg8b on a qword that is not 8-byte
 * aligned (XBA-71): one for the whole runtime, so every generated file uses
 * the same. */
volatile int g_recomp_cas64_lock;

uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;

/* The guest CPU registers for recompiled code (see RecompCpu in
 * xbox_memory_layout.h). One thread-local block per host thread; every
 * thread starts from these values.
 *
 * fs_base: each guest thread's TIB. The first thread uses the one the loader
 * built; a spawned thread gets its own from xbox_AllocThreadTib().
 * x87 control and status: the reset default masks every exception and rounds
 * to nearest, which is what the CRT expects before _control87.
 * eflags: the image pushfd and popfd use (see recomp_types.h). Bit 1 is
 * always set and IF is on, as in any guest thread the kernel schedules.
 * df: zero means the string instructions walk forwards, the ABI's resting
 * state. MSVC's strrchr/wcsrchr scan backwards with `std; repne scasb`, and
 * memmove goes backwards when its regions overlap the wrong way; per-thread
 * because `std` and the `cld` that undoes it can land in different lifted
 * bodies of the same guest routine. */
RECOMP_TLS RecompCpu g_cpu = {
    .r_fs_base = XBOX_TIB_MAIN,
    .r_fp_control_word = 0x037Fu,
    .r_mxcsr = 0x1F80u,
    .r_fp_cc = 0x4000,
    .r_eflags = 0x00000202u,
};

/* The shape of the TLS block the loader built, so a new thread can be
 * given one just like it: where the initialised image data starts, how
 * big the block is, and how big the per-thread structure slot 0 points
 * at is. Zero total means the image had no TLS directory. */
static uint32_t g_tls_template_va, g_tls_total, g_tls_thread_size = 64;

#ifdef RECOMP_ABI_CHECK
/* Report a lifted function that returned without restoring ebx/esi/edi.
 *
 * Those are callee-saved on x86, and the recompiler keeps them in globals, so
 * a function whose epilogue was never lifted corrupts its caller rather than
 * itself -- an error with no crash and no message, just less work silently
 * done. Ranked by hit count so the routine breaking a hot loop stands out from
 * the one-offs; -DRECOMP_ABI_CHECK only, since it costs three compares on
 * every indirect call.
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0)
{
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
        fprintf(stderr, "[ABI] sub_%08X:%s%s%s%s\n"
                        "      ebx %08X->%08X esi %08X->%08X"
                        " edi %08X->%08X esp %08X->%08X\n",
                va,
                g_ebx != ebx0 ? " ebx" : "",
                g_esi != esi0 ? " esi" : "",
                g_edi != edi0 ? " edi" : "",
                g_esp < esp0 + 4 ? " esp(epilogue never ran)" : "",
                ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp);
        /* esp coming back too HIGH means some callee popped arguments that
         * were never pushed -- a convention mismatch the one-sided invariant
         * above cannot see. The most recent indirect targets are the usual
         * suspects, so name them. */
        {
            int t;
            fprintf(stderr, "      esp delta %+d, recent icall targets:",
                    (int)(g_esp - esp0));
            for (t = 4; t >= 1; t--)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx - t) & 15]);
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    hits[i]++;
}
#endif

/* Set once at startup. The generated code reads it at the ret of every
 * --force-return function, so it has to be cheap and it has to default to
 * off: a build carrying forced functions behaves normally until the
 * variable is set. */
int g_force_return = 0;

/* ---- non-local jumps ---------------------------------------------------
 *
 * The native half of the guest's setjmp/longjmp. See recomp_types.h for why a
 * guest-only longjmp is not enough; in short, the recompiled frames are C
 * frames and something has to unwind them.
 *
 * Keyed by guest buffer address, per thread. Buffers nest, so jumping to an
 * outer one discards every inner entry -- those frames are gone.
 *
 * A thread that switches guest stacks (recomp_stack_switch.c, XBA-101) runs
 * on several host stacks, and a native longjmp is only valid on the stack the
 * buffer was armed on. Each slot names its host context: a jump to a buffer
 * armed on another one is refused (the guest's own longjmp runs instead, as
 * for a buffer with no native counterpart), and a jump only discards the
 * inner entries of its own context -- the others' frames are still alive,
 * parked on their stacks. docs/technical/stack-switching.md « Limits ».
 */
#define RECOMP_JMPBUF_SLOTS 32

typedef struct {
    uint32_t buf_va;
    uint32_t ctx;           /* recomp_stack_context() when armed */
    jmp_buf  native;
} recomp_jmp_slot;

static RECOMP_TLS recomp_jmp_slot s_jmp[RECOMP_JMPBUF_SLOTS];
static RECOMP_TLS int             s_jmp_used;

jmp_buf *recomp_setjmp_slot(uint32_t buf_va)
{
    uint32_t ctx = recomp_stack_context();
    int i;

    for (i = 0; i < s_jmp_used; i++)
        if (s_jmp[i].buf_va == buf_va) {
            s_jmp[i].ctx = ctx;           /* the same buffer, re-armed */
            return &s_jmp[i].native;
        }
    if (s_jmp_used >= RECOMP_JMPBUF_SLOTS)
        s_jmp_used = RECOMP_JMPBUF_SLOTS - 1;   /* keep the deepest */
    s_jmp[s_jmp_used].buf_va = buf_va;
    s_jmp[s_jmp_used].ctx = ctx;
    return &s_jmp[s_jmp_used++].native;
}

int recomp_guest_longjmp(uint32_t buf_va, uint32_t value)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t ctx = recomp_stack_context();
    int i, j, kept;

    for (i = s_jmp_used - 1; i >= 0; i--) {
        if (s_jmp[i].buf_va != buf_va)
            continue;
        if (s_jmp[i].ctx != ctx) {
            static int said;
            if (said++ < 8)
                fprintf(stderr, "[STACK] longjmp to jmp_buf 0x%08X armed on host stack %u from "
                                "host stack %u: the guest's own longjmp runs instead\n",
                        buf_va, s_jmp[i].ctx, ctx);
            return 0;
        }

        /* The callee-saved registers and the stack, exactly as the CRT's
         * longjmp restores them: esp is the setjmp-time esp plus the return
         * address that setjmp's own ret would have popped. */
        g_ebx = *(const uint32_t *)(mem + buf_va + 0x04);
        g_edi = *(const uint32_t *)(mem + buf_va + 0x08);
        g_esi = *(const uint32_t *)(mem + buf_va + 0x0C);
        g_esp = *(const uint32_t *)(mem + buf_va + 0x10) + 4;

        /* ebp is a C local in every translated function, and a local modified
         * after setjmp is indeterminate once longjmp lands. Hand the resumed
         * frame its saved value back through the globals it already reads. */
        g_seh_ebp = *(const uint32_t *)(mem + buf_va + 0x00);
        g_ebp     = g_seh_ebp;

        /* The inner buffers of this context died with their frames; the
         * other contexts' are still alive, parked on their stacks. */
        for (j = kept = i + 1; j < s_jmp_used; j++)
            if (s_jmp[j].ctx != ctx)
                s_jmp[kept++] = s_jmp[j];
        s_jmp_used = kept;
        longjmp(s_jmp[i].native, value ? (int)value : 1);
    }
    return 0;
}

/* Watchdog: dump the guest call stack if the title stops making progress.
 *
 * A hang gives nothing to work from -- no crash, no last log line, no native
 * stack that means anything, because the guest frames live in guest memory and
 * the native one only shows whichever translated function is spinning. Sampling
 * the guest stack from a second thread is the one view that says where the
 * title actually is. Same GS format the crash handler uses, so tools/
 * stackwalk.py reads either.
 *
 * Off unless RECOMP_WATCHDOG_SECS is set, so it costs a getenv in normal runs.
 */
/* Defined below, after the watchdog. */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

static uint32_t *s_watchdog_esp;
/* The other guest registers are thread-local too, so the watchdog has to be
 * handed the guest thread's copies rather than reading its own -- which are
 * always zero, and read as "every register is null" at exactly the moment the
 * registers are the thing being asked about. */
static uint32_t *s_watchdog_regs[6];
static unsigned  s_watchdog_secs;

/* Can RECOMP_PEEK dereference this guest address?
 *
 * It used to accept only the first 64 MB, which reads as "RAM" but is not the
 * question -- every window this file maps is mapped at va + g_memory_offset,
 * so the register apertures are just as dereferenceable as RAM is. Rejecting
 * them silently printed nothing for an address that was perfectly readable,
 * and a hang spinning on a GPU register is exactly the case where the value
 * that matters lives at 0xFD......  Peeking one is how the busy-wait in
 * DDS9's pushbuffer reserve was pinned to a DMA pointer rather than a flag.
 *
 * Every window is checked against its own pointer, because they are mapped
 * independently and any of them can be absent for this run. The 4 is the
 * width of the read below: an address one or two bytes short of the end is
 * inside the window and still faults. */
static int peek_readable(uint32_t va)
{
    struct { const void *mapped; uint32_t base; uint64_t size; } win[] = {
        { g_memory_base,   XBOX_BASE_ADDRESS, (uint64_t)g_memory_size },
        { g_contig_memory, XBOX_CONTIG_BASE,  XBOX_CONTIG_SIZE },
        { g_nv2a_memory,   XBOX_NV2A_BASE,    XBOX_NV2A_SIZE },
        { g_mcpx_memory,   XBOX_MCPX_BASE,    XBOX_MCPX_SIZE },
        { g_flash_memory,  XBOX_FLASH_BASE,   XBOX_FLASH_SIZE },
    };
    size_t i;

    for (i = 0; i < sizeof(win) / sizeof(win[0]); i++) {
        if (!win[i].mapped || !win[i].size)
            continue;
        if (va >= win[i].base
                && (uint64_t)va + 4 <= (uint64_t)win[i].base + win[i].size)
            return 1;
    }
    return 0;
}

/* ---- RECOMP_WATCH: name the guest code that changes a guest dword -------
 *
 * A peek says a value changed between two samples. It does not say who
 * changed it, and for a value produced deep inside a middleware layer that
 * is the only question that matters -- reading the lifted C outwards from
 * the write is guesswork, and reading it inwards from the caller is worse.
 *
 * Same mechanism as the AC'97 trap above: make the page read-only, catch the
 * write, single-step it, then report. What it adds is the guest call chain,
 * scanned off the guest stack the way the watchdog does, which turns "the
 * mask became 4" into a list of addresses to go and read.
 *
 * Off unless RECOMP_WATCH is set. Costs a page fault per write to that page,
 * so it is a bring-up tool and says so.
 */
static uint32_t g_watch_va;
static void    *g_watch_page;
static uint32_t g_watch_last;
static void    *g_watch_veh;
static RECOMP_TLS int s_watch_stepping;

/* A plausible guest code address: inside the image's executable sections,
 * as recorded from the section headers at load. */
static int watch_is_code(uint32_t va)
{
    return va >= g_xbox_code_lo && va < g_xbox_code_hi;
}

static void watch_report(void)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t now = *(const uint32_t *)(mem + g_watch_va);
    uint32_t esp = g_esp, i, shown = 0;

    if (now == g_watch_last)
        return;
    fprintf(stderr, "[WATCH] [%08X] %08X -> %08X  (esp=%08X)\n",
            g_watch_va, g_watch_last, now, esp);
    g_watch_last = now;

    /* Return addresses the recompiled code pushed, innermost first. Values
     * that merely look like code get printed too -- the chain is a lead, not
     * a proof, and saying so is cheaper than a stack walk that cannot be
     * done without frame information the lift does not keep. */
    for (i = 0; esp && i < 256u && shown < 12u; i++) {
        uint32_t slot = esp + i * 4u;
        uint32_t v;
        if (!peek_readable(slot))
            break;
        v = *(const uint32_t *)(mem + slot);
        if (watch_is_code(v)) {
            fprintf(stderr, "         [esp+%-4u] %08X\n", i * 4u, v);
            shown++;
        }
    }

    /* RECOMP_WATCH_RAW also prints the frame unfiltered. The filtered chain
     * answers "who wrote this"; the raw frame answers "to what object", which
     * is the next question every time -- saved registers and pointer
     * arguments live there and look nothing like code. */
    if (getenv("RECOMP_WATCH_RAW")) {
        for (i = 0; esp && i < 24u; i++) {
            uint32_t slot = esp + i * 4u;
            if (!peek_readable(slot))
                break;
            fprintf(stderr, "         raw[esp+%-4u] %08X\n", i * 4u,
                    *(const uint32_t *)(mem + slot));
        }
    }
    fflush(stderr);
}

static LONG CALLBACK watch_veh(PEXCEPTION_POINTERS ep)
{
#if defined(_WIN32)
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    if (!g_watch_page)
        return EXCEPTION_CONTINUE_SEARCH;

    if (code == EXCEPTION_SINGLE_STEP && s_watch_stepping) {
        s_watch_stepping = 0;
        watch_report();
        VirtualProtect(g_watch_page, 4096, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];

        if (fault >= (uintptr_t)g_watch_page
                && fault < (uintptr_t)g_watch_page + 4096) {
            if (!VirtualProtect(g_watch_page, 4096, PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            s_watch_stepping = 1;
            ep->ContextRecord->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
#else
    /* The non-Windows VEH shim never fires and EXCEPTION_POINTERS is
     * opaque there, so there is nothing to decode. POSIX arms its own
     * signal handler below and reports from it. */
    (void)ep;
    (void)watch_report;
    (void)s_watch_stepping;
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

#if !defined(_WIN32)
/* POSIX: no single-step from a signal handler on arm64, so the trap is
 * "fault, unprotect, re-protect later". The fault names the writer: lifted
 * calls are native calls, so the host stack at the fault is the guest call
 * chain (the lifted functions' frames). A helper thread re-protects the page a moment
 * later and prints which watched dwords changed in that window. A write by
 * another thread inside the window is seen in the diff but not attributed.
 *
 * RECOMP_WATCH_LEN=<bytes> (default 4, clamped to the host page) widens the range,
 * so the fields of one heap object can be watched at once. Writes through
 * the contiguous-window alias go to a different host mapping and are not
 * trapped. */
#include <signal.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>

static uint32_t g_watch_len = 4;
static uintptr_t g_watch_hpage, g_watch_hsize;   /* host page (16 KB on arm64 Darwin) */
static uint32_t g_watch_snap[4096];
static volatile int g_watch_open;     /* page is RW, re-protect pending */
static struct sigaction g_watch_prev_bus, g_watch_prev_segv;

static void watch_sym(void *pc, const char *tag, int n)
{
    Dl_info di;
    if (pc && dladdr(pc, &di) && di.dli_sname)
        fprintf(stderr, "         %s%-2d %s+0x%lX\n", tag, n, di.dli_sname,
                (unsigned long)((uintptr_t)pc - (uintptr_t)di.dli_saddr));
    else
        fprintf(stderr, "         %s%-2d %p\n", tag, n, pc);
}

static void watch_posix_stack(void *ucv)
{
#if defined(__APPLE__) && defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
    uintptr_t prev = 0;
    int n = 0, reps = 0, depth = 0;
    watch_sym((void *)(uintptr_t)uc->uc_mcontext->__ss.__pc, "#", n++);
    watch_sym((void *)(uintptr_t)uc->uc_mcontext->__ss.__lr, "#", n++);
    /* RECOMP_WATCH_DEPTH frames (default 14); identical consecutive return
     * addresses (recursion) are folded into one line with a count. */
    {
        const char *dv = getenv("RECOMP_WATCH_DEPTH");
        depth = dv && *dv ? atoi(dv) : 14;
    }
    while (n < depth && fp && (fp & 7) == 0) {
        uintptr_t next = ((const uintptr_t *)fp)[0];
        uintptr_t ret = ((const uintptr_t *)fp)[1];
        if (ret && ret == prev) {
            reps++;
        } else if (ret) {
            if (reps)
                fprintf(stderr, "            (previous frame x%d more)\n",
                        reps);
            reps = 0;
            watch_sym((void *)ret, "#", n++);
            prev = ret;
        }
        if (next <= fp)
            break;
        fp = next;
    }
    if (reps)
        fprintf(stderr, "            (previous frame x%d more)\n", reps);
#else
    (void)ucv;
    fprintf(stderr, "         (host stack not available on this host)\n");
#endif
}

static void watch_posix_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t fault = (uintptr_t)si->si_addr;
    uintptr_t page = g_watch_hpage;
    struct sigaction *prev;

    if (g_watch_page && fault >= page && fault < page + g_watch_hsize) {
        uintptr_t lo = (uintptr_t)g_memory_offset + g_watch_va;
        if (fault >= lo && fault < lo + g_watch_len) {
            fprintf(stderr, "[WATCH] write to %08X (+0x%X) esp=%08X\n",
                    (uint32_t)(fault - (uintptr_t)g_memory_offset),
                    (unsigned)(fault - lo), g_esp);
            watch_posix_stack(ucv);
            fflush(stderr);
        }
        mprotect((void *)g_watch_hpage, g_watch_hsize, PROT_READ | PROT_WRITE);
        g_watch_open = 1;
        return;   /* re-executes the store, now allowed */
    }

    prev = sig == SIGBUS ? &g_watch_prev_bus : &g_watch_prev_segv;
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) {
            prev->sa_sigaction(sig, si, ucv);
            return;
        }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);   /* returning re-faults and dies by the signal */
}

static void watch_posix_diff(void)
{
    const uint32_t *cur = (const uint32_t *)((const uint8_t *)g_memory_offset
                                             + g_watch_va);
    uint32_t i, n = (g_watch_len + 3) / 4;
    for (i = 0; i < n; i++) {
        if (cur[i] != g_watch_snap[i]) {
            fprintf(stderr, "[WATCH] [%08X] %08X -> %08X\n",
                    g_watch_va + i * 4, g_watch_snap[i], cur[i]);
            g_watch_snap[i] = cur[i];
        }
    }
    fflush(stderr);
}

static void *watch_posix_reprotect(void *unused)
{
    struct timespec ts = { 0, 200 * 1000 };   /* 200 us window */
    (void)unused;
    for (;;) {
        nanosleep(&ts, NULL);
        if (!g_watch_open)
            continue;
        mprotect((void *)g_watch_hpage, g_watch_hsize, PROT_READ);
        g_watch_open = 0;
        watch_posix_diff();
    }
    return NULL;
}

static int watch_posix_arm(void)
{
    struct sigaction sa;
    pthread_t th;
    const char *len = getenv("RECOMP_WATCH_LEN");
    uint32_t room;

    g_watch_hsize = (uintptr_t)sysconf(_SC_PAGESIZE);
    g_watch_hpage = ((uintptr_t)g_memory_offset + g_watch_va)
                    & ~(g_watch_hsize - 1);
    room = (uint32_t)(g_watch_hpage + g_watch_hsize
                      - ((uintptr_t)g_memory_offset + g_watch_va));
    if (room > sizeof g_watch_snap)
        room = sizeof g_watch_snap;
    if (len && *len)
        g_watch_len = (uint32_t)strtoul(len, NULL, 0);
    if (!g_watch_len)
        g_watch_len = 4;
    if (g_watch_len > room)
        g_watch_len = room;
    memcpy(g_watch_snap, (const uint8_t *)g_memory_offset + g_watch_va,
           (g_watch_len + 3) & ~3u);

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = watch_posix_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, &g_watch_prev_bus) != 0
            || sigaction(SIGSEGV, &sa, &g_watch_prev_segv) != 0)
        return 0;
    if (pthread_create(&th, NULL, watch_posix_reprotect, NULL) != 0)
        return 0;
    pthread_detach(th);
    if (mprotect((void *)g_watch_hpage, g_watch_hsize, PROT_READ) != 0)
        return 0;
    fprintf(stderr, "  WATCH: writes to %u bytes at 0x%08X are trapped "
                    "(POSIX, current %08X)\n", g_watch_len, g_watch_va,
            g_watch_last);
    fflush(stderr);
    return 1;
}
#endif /* !_WIN32 */

/* The target, which may be reached through pointers that do not exist yet.
 *
 * "[[0x006DF414]]+0x14" is two dereferences and an offset: the interesting
 * field of a heap object whose address changes run to run, but which is
 * always reachable from a static one. Without this the only way to watch such
 * a field is to learn its address from one run and hope the allocator repeats
 * it, which it does not. */
static unsigned g_watch_derefs;
static uint32_t g_watch_root;
static uint32_t g_watch_off;

static int watch_resolve(uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t a = g_watch_root;
    unsigned k;

    for (k = 0; k < g_watch_derefs; k++) {
        if (!peek_readable(a))
            return 0;
        a = *(const uint32_t *)(mem + a);
        if (!a)
            return 0;
    }
    a += g_watch_off;
    if (!peek_readable(a))
        return 0;
    *out = a;
    return 1;
}

static int watch_arm(uint32_t va);

/* Poll until the chain resolves, then arm. Twenty milliseconds, because the
 * object appears once during bring-up and never again -- this thread exists
 * for a few seconds and then does nothing for the rest of the run. */
static DWORD WINAPI watch_resolver(LPVOID unused)
{
    unsigned tries;

    (void)unused;
    for (tries = 0; tries < 15000u && !g_watch_page; tries++) {
        uint32_t va;
        if (watch_resolve(&va) && watch_arm(va))
            return 0;
        Sleep(20);
    }
    if (!g_watch_page)
        fprintf(stderr, "  WATCH: %u-deep chain from 0x%08X never resolved\n",
                g_watch_derefs, g_watch_root);
    return 0;
}

void xbox_WatchInit(void)
{
    const char *spec = getenv("RECOMP_WATCH");
    const char *q;
    char *endp;

    if (!spec || !*spec || g_memory_base == NULL || g_watch_page)
        return;

    for (q = spec; *q == '['; q++)
        g_watch_derefs++;
    g_watch_root = (uint32_t)strtoul(q, &endp, 0);
    while (*endp == ']')
        endp++;
    if (*endp == '+')
        g_watch_off = (uint32_t)strtoul(endp + 1, NULL, 0);

    if (g_watch_derefs) {
        fprintf(stderr, "  WATCH: resolving %u-deep chain from 0x%08X "
                        "+0x%X\n", g_watch_derefs, g_watch_root, g_watch_off);
        CloseHandle(CreateThread(NULL, 0, watch_resolver, NULL, 0, NULL));
        return;
    }
    watch_arm(g_watch_root + g_watch_off);
}

static int watch_arm(uint32_t va)
{
    DWORD old;

    g_watch_va = va;
    if (!peek_readable(g_watch_va)) {
        fprintf(stderr, "  WATCH: 0x%08X is not in a mapped window; "
                        "not armed\n", g_watch_va);
        return 0;
    }
    g_watch_last = *(const uint32_t *)((const uint8_t *)g_memory_offset
                                       + g_watch_va);
    /* The page holding the guest dword, in host terms. */
    g_watch_page = (void *)(((uintptr_t)((const uint8_t *)g_memory_offset
                                         + g_watch_va)) & ~(uintptr_t)4095);
#if !defined(_WIN32)
    (void)old;
    if (!watch_posix_arm()) {
        g_watch_page = NULL;
        fprintf(stderr, "  WATCH: cannot trap 0x%08X; not armed\n",
                g_watch_va);
        return 0;
    }
    return 1;
#endif
    g_watch_veh = AddVectoredExceptionHandler(1, watch_veh);
    if (!g_watch_veh
            || !VirtualProtect(g_watch_page, 4096, PAGE_READONLY, &old)) {
        if (g_watch_veh) {
            RemoveVectoredExceptionHandler(g_watch_veh);
            g_watch_veh = NULL;
        }
        g_watch_page = NULL;
        fprintf(stderr, "  WATCH: cannot trap 0x%08X; not armed\n",
                g_watch_va);
        return 0;
    }
    fprintf(stderr, "  WATCH: writes to the page of 0x%08X are trapped "
                    "(current %08X)\n", g_watch_va, g_watch_last);
    fflush(stderr);
    return 1;
}

/* Print the RECOMP_PEEK globals. Shared, because the two moments worth
 * sampling are a hang and an early exit, and only the first had it: a title
 * whose main() returns during init never reaches the watchdog, so the one
 * question that mattered -- which of its init calls failed -- was the one the
 * tooling could not answer. Silent unless RECOMP_PEEK is set. */
void xbox_PeekSample(const char *label)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    const char *spec = getenv("RECOMP_PEEK");
    char buf[256], *q, *end;

    if (!spec || !*spec || g_memory_base == NULL)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    fprintf(stderr, "  %s:", label ? label : "peek");
    for (q = buf; *q; ) {
        /* "[[0x006DF414]]+0x14" follows two pointers and adds an offset.
         * The fields worth watching during bring-up are usually inside heap
         * objects whose addresses change run to run but which are always
         * reachable from a static one, and a peek that cannot follow a
         * pointer cannot see them at all. */
        unsigned derefs = 0, k;
        unsigned long va;
        uint32_t a;
        int ok = 1;

        while (*q == '[') { derefs++; q++; }
        va = strtoul(q, &end, 0);
        if (end == q)
            break;
        while (*end == ']')
            end++;
        a = (uint32_t)va;
        for (k = 0; k < derefs && ok; k++) {
            if (!peek_readable(a) || !(a = *(const uint32_t *)(mem + a)))
                ok = 0;
        }
        if (*end == '+')
            a += (uint32_t)strtoul(end + 1, &end, 0);
        if (ok && peek_readable(a))
            fprintf(stderr, " [%08X]=%08X", a, *(const uint32_t *)(mem + a));
        else
            fprintf(stderr, " [%08X]=??", a);
        q = (*end == ',') ? end + 1 : end;
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

static DWORD WINAPI xbox_watchdog_thread(LPVOID unused)
{
    const uint8_t *mem;
    uint32_t esp, i;

    (void)unused;
    Sleep(s_watchdog_secs * 1000u);

    mem = (const uint8_t *)g_memory_offset;
    esp = s_watchdog_esp ? *s_watchdog_esp : 0;
    fprintf(stderr, "[WATCHDOG] no exit after %us; guest esp=0x%08X\n"
            "  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            s_watchdog_secs, esp,
            s_watchdog_regs[0] ? *s_watchdog_regs[0] : 0,
            s_watchdog_regs[1] ? *s_watchdog_regs[1] : 0,
            s_watchdog_regs[2] ? *s_watchdog_regs[2] : 0,
            s_watchdog_regs[3] ? *s_watchdog_regs[3] : 0,
            s_watchdog_regs[4] ? *s_watchdog_regs[4] : 0,
            s_watchdog_regs[5] ? *s_watchdog_regs[5] : 0);
    /* The recent indirect-call targets name whatever is spinning: a stuck loop
     * inside a function reached through a pointer leaves no clue on the stack
     * beyond the return address of the call that entered it. */
    {
        uint32_t k;
        /* The running indirect-call total separates a hang from mere
         * slowness. Kernel calls cannot: a pure CPU loop makes none, so
         * "same count at 20s and 60s" proves nothing about it. */
        fprintf(stderr, "  icalls so far: %llu\n",
                (unsigned long long)g_icall_count);
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X",
                    g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    /* Guest globals worth seeing at the moment of the hang.
     *
     * RECOMP_PEEK is otherwise only sampled by the pushbuffer reporter, which
     * a title that hangs before rendering never reaches -- and a spin that
     * makes no kernel calls is invisible to RECOMP_KERNEL_WATCH too. A pure
     * CPU loop polling a global is exactly the case neither of those covers.
     */
    xbox_PeekSample("peek");
    /* The pushbuffer pointers, unconditionally.
     *
     * "Extend the table as more handshakes turn up -- run the title and the
     * watchdog sample will name the register" is only true if the sample
     * actually shows them. It did not: a title spinning on a DMA pointer made
     * no kernel calls and no indirect calls, so every other line the watchdog
     * prints was identical between two samples taken 40 seconds apart, and the
     * register that was stuck did not appear at all.
     *
     * Both sides of the channel, because which one the title consults is a
     * property of its D3D and not of the hardware: Halo waits on the USER
     * pair, DDS9 reads USER first and falls back to PFIFO's DMA_SUBROUTINE.
     * Printing only the pair that some other title used is how this stayed
     * invisible. */
    if (g_nv2a_memory) {
        const char *r = (const char *)g_nv2a_memory;
#define WD_NV2A(off) (*(const volatile uint32_t *)(r + (off)))
        fprintf(stderr, "  NV2A USER  PUT=%08X GET=%08X\n"
                        "  NV2A PFIFO PUT=%08X GET=%08X REF=%08X SUBR=%08X\n",
                WD_NV2A(NV2A_USER_DMA_PUT), WD_NV2A(NV2A_USER_DMA_GET),
                WD_NV2A(NV2A_PFIFO_DMA_PUT), WD_NV2A(NV2A_PFIFO_DMA_GET),
                WD_NV2A(NV2A_PFIFO_REF), WD_NV2A(NV2A_PFIFO_DMA_SUBROUTINE));
#undef WD_NV2A
    }

    for (i = 0; i < 400 && esp; i++) {
        uint32_t a = esp + i * 4;
        if (a < XBOX_STACK_BASE || a >= XBOX_STACK_TOP) break;
        fprintf(stderr, "    GS %08X %08X\n", a,
                *(const uint32_t *)(mem + a));
    }
    fflush(stderr);
    /* Counts only, without the report's lock: the thread being reported
     * on may be wedged anywhere, inside that lock included. */
    {
        uint32_t hi, lo;
        xbox_missing_counts(&hi, &lo, NULL);
        if (hi + lo)
            fprintf(stderr, "  [FILE] missing at watchdog: %u high, %u low\n",
                    (unsigned)hi, (unsigned)lo);
        fflush(stderr);
    }
    /* _exit skips atexit: finalise a RECOMP_AUDIO_WAV dump here, or up to
     * its last second is past the header. Bounded, in case the frame
     * thread is wedged inside a write. */
    apu_wav_close_nowait(500);
    _exit(3);
    return 0;
}

void xbox_WatchdogStart(void)
{
    const char *secs = getenv("RECOMP_WATCHDOG_SECS");
    HANDLE h;

    if (!secs || !*secs)
        return;
    s_watchdog_secs = (unsigned)atoi(secs);
    if (!s_watchdog_secs)
        return;

    /* Taken on the guest thread: g_esp is thread-local, so the watchdog has to
     * be handed the address of the one that matters rather than reading its
     * own, which is always zero. */
    s_watchdog_esp = &g_esp;
    s_watchdog_regs[0] = &g_eax; s_watchdog_regs[1] = &g_ecx;
    s_watchdog_regs[2] = &g_edx; s_watchdog_regs[3] = &g_ebx;
    s_watchdog_regs[4] = &g_esi; s_watchdog_regs[5] = &g_edi;
    h = CreateThread(NULL, 0, xbox_watchdog_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* ICALL trace ring buffer */
volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;

static void xbox_heap_reset_base(void);

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    g_force_return = getenv("RECOMP_FORCE_RETURN") != NULL;
    recomp_status_start();      /* RECOMP_STATUS: the headless run's status file */
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full Xbox address space (covers all sections + stack + heap).
     * Size is runtime-configurable: retail 64 MB, devkit debug builds 128 MB. */
    /* The mapped range, which is not necessarily RAM. Mirrors are placed
     * at multiples of this, so growing it is what stops a title's
     * above-RAM allocations from aliasing low memory. */
    g_memory_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of size (64 MB) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
        static const uintptr_t try_bases[] = {
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
            0,                      /* sentinel - let OS choose */
        };

        /* Iterate the whole array, sentinel included. The old condition
         * (try_bases[i] != 0 || i == 0) stopped *at* the zero rather than
         * using it, so the "let the OS choose" fallback never ran: the loop
         * tried the fixed addresses and gave up. Invisible on Windows, where
         * one of the low bases succeeds -- fatal on arm64 macOS, where all of
         * them sit inside the 4 GB __PAGEZERO segment and none can. */
#if defined(_WIN32)
        /* RECOMP_WIN_PLACEHOLDER=1 reserves the whole guest window instead;
         * see win_reserve_guest_window. Opt-in: so far it has only been run
         * under Wine/Proton, where the OS-chosen base otherwise lands the
         * contiguous window and the tiled aperture on taken host ranges
         * (error 487). Off, the fixed base list below runs as before. The
         * POSIX span below depends on releasing part of a reservation, which
         * VirtualFree(MEM_RELEASE) refuses on Windows and on Wine: a non-zero
         * size fails with ERROR_INVALID_PARAMETER. */
        if (getenv("RECOMP_WIN_PLACEHOLDER"))
            g_memory_base = win_reserve_guest_window(g_mapping_handle,
                                                     g_memory_size);
#else
        /* Reserve base + mirrors as one range, and map the base at its head.
         * Each view replaces its slice of our own reservation in one mmap
         * (layout_map_view, span_holds) rather than racing for free space.
         *
         * POSIX only. Win32 VirtualFree cannot release part of a reservation
         * (MEM_RELEASE with a nonzero size is ERROR_INVALID_PARAMETER), so
         * there the base view never maps and the whole span stays
         * reserved. At a 64 MB map that is 1.8 GB the OS tends to place at
         * 0x80000000 -- exactly the host range the contiguous window (guest
         * 0x80000000) needs, which then fails with error 487 and the first
         * touch of the kernel page faults. Larger map sizes push the span
         * above 4 GB, which is why Half-Life 2 (768 MB) never saw it. Doing
         * this on Windows needs placeholder reservations (VirtualAlloc2),
         * which is what RECOMP_WIN_PLACEHOLDER above does. */
        g_span_size = g_memory_size * (size_t)(1 + XBOX_NUM_MIRRORS);
        /* Stop below the contiguous window: at 128 MB the span would
         * otherwise run to 0xE8000000 and hold 0x80000000 against it. */
        if (g_span_size > XBOX_CONTIG_BASE)
            g_span_size = XBOX_CONTIG_BASE;
        /* RECOMP_EXT_VMA: hold the whole 4 GB guest window, as Windows does
         * with its placeholder. Linux places mappings top-down, so the host
         * range just above a fresh span is usually already taken: guest_vmem's
         * fixed reservation there failed (ENOMEM), and a span grown only to
         * GUEST_VMEM_TOP pushed the contiguous window onto the neighbour
         * instead. Each fixed window is carved out of the span as it is
         * mapped, and guest_vmem's range is handed over just before
         * guest_vmem_init. RECOMP_GUEST_WINDOW (the default on 64-bit
         * hosts, XBA-103) holds the same window without the tracker. */
        if (sizeof(void *) >= 8
                && (ext_vma_span(g_span_size) || hold_guest_window())) {
            /* The 4 GB window plus one view past it: the first RAM mirror
             * above 4 GB is the wrap of a guest access that runs over the
             * top (a dword read at 0xFFFFFFFF, guest+disp carrying out of 32
             * bits). Left outside the span its host range is often taken in
             * an Android app (top-down placement again), the mirror fails
             * and the access faults; Fable reads a -1 vtable pointer there
             * during its front-end load and died on the Thor, while the Mac,
             * where that range was free, read the wrapped zero and skipped
             * the call. */
            g_span_size = ((size_t)1 << 32) + g_memory_size;
            g_span_whole_window = 1;
        }
        g_span_base = VirtualAlloc(NULL, g_span_size, MEM_RESERVE, PAGE_NOACCESS);
        if (g_span_base) {
            /* g_span_size keeps the length; left registered, the span would
             * answer VirtualQuery for every window carved out of it. */
            w32_view_forget(g_span_base);
            g_memory_base = layout_map_view(g_mapping_handle, g_memory_size,
                                            g_span_base);
            if (!g_memory_base) {
                VirtualFree(g_span_base, g_span_size, MEM_RELEASE);
                g_span_base = NULL;
                g_span_size = 0;
                g_span_whole_window = 0;
            }
        } else {
            g_span_whole_window = 0;
        }
#endif

        const size_t n_bases = sizeof(try_bases) / sizeof(try_bases[0]);
        for (size_t i = 0; !g_memory_base && i < n_bases; i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                hint            /* desired base address */
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;
#if defined(_WIN32)
    fprintf(stderr, "xbox_MemoryLayoutInit: guest window reservation: %s,"
            " base %p", g_win_reserve_method, g_memory_base);
    if (g_win_window_size)
        fprintf(stderr, ", %zu MB held\n", g_win_window_size >> 20);
    else
        fprintf(stderr, "\n");
#endif

    /* Guest page zero: no access.
     *
     * Nothing legitimate lives there -- every XBE's image base is 0x00010000
     * and the TIB now sits at XBOX_FS_BASE -- so any access is a null pointer
     * the title dereferenced. Left readable it did quiet damage: a null check
     * of the form `cmp byte [ecx], 0` read whatever happened to be at 0 and
     * decided the pointer was fine, and a store through a null pointer landed
     * on real memory and surfaced as corruption somewhere unrelated. Faulting
     * here turns both into one access violation at the instruction that made
     * the mistake, which the crash handler can name.
     *
     * Opt-in through RECOMP_TRAP_NULL, because it converts a class of bug the
     * title currently survives into a hard stop: a guest that dereferences null
     * and ignores the result keeps running while page zero reads as zero, and
     * stops dead once it faults. That is the right default for hunting one of
     * these and the wrong one for making progress past the rest, so it is a
     * switch rather than a policy.
     *
     * Note this is separate from moving the TIB off page zero, which is not
     * optional: with the TIB gone, address 0 reads as plain zero, so a null
     * check written as a load through the pointer now gets the answer it
     * expects whether or not the page is trapped.
     *
     * Best-effort: failing to protect it costs only the diagnostic. */
    if (XBOX_MAP_START == 0 && getenv("RECOMP_TRAP_NULL")) {
        DWORD old_protect;
        /* Protection is applied at host page granularity, and the host page
         * is not always the guest's 4 KB -- Apple Silicon uses 16 KB, so this
         * 0x1000 request actually covers guest 0..0x3FFF. That is why
         * XBOX_TIB_MAIN sits at 0x4000: the widest page any supported host
         * uses fits below the TIB, so the rounding costs nothing and the
         * guard installs everywhere.
         *
         * The check below is what remains of an earlier bug rather than dead
         * code. With the TIB at 0x1000 the rounding reached it, init wrote the
         * TIB moments later, and every run that asked for the guard died at
         * startup -- so the guard disabled itself on all of Apple Silicon and
         * the diagnostic silently did nothing. It stays as a floor for a host
         * with pages wider than the TIB offset, where skipping really is
         * better than breaking the run. */
#if defined(_WIN32)
        SYSTEM_INFO si;
        long host_page;
        GetSystemInfo(&si);
        host_page = (long)si.dwPageSize;
#else
        long host_page = sysconf(_SC_PAGESIZE);
#endif
        if (host_page > 0 && (uint32_t)host_page > XBOX_TIB_MAIN) {
            fprintf(stderr, "  RECOMP_TRAP_NULL: not available -- the host page "
                    "is %ld bytes, so trapping guest page zero would also trap "
                    "the TIB at 0x%08X\n", host_page, XBOX_TIB_MAIN);
        } else {
            if (VirtualProtect(g_memory_base, 0x1000, PAGE_NOACCESS, &old_protect)) {
                fprintf(stderr, "  guest page 0 is PAGE_NOACCESS"
                                " (null dereferences fault)\n");
            }
        }
    }

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }
#if !defined(_WIN32)
    if (g_span_whole_window)
        fprintf(stderr, "xbox_MemoryLayoutInit: whole 4 GB guest window held at %p-%p, "
                "plus its wrap mirror to %p (RECOMP_GUEST_WINDOW)\n", g_span_base,
                (void *)((uintptr_t)g_span_base + ((size_t)1 << 32)),
                (void *)((uintptr_t)g_span_base + g_span_size));
#endif

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        /* The certificate's title name (UTF-16, 40 chars at +0x0C), for
         * the framebuffer window's title bar. */
        DWORD cert_off = *(const DWORD *)(xbe + 0x118) - base_addr;
        if (cert_off + 0x0C + 80 <= xbe_size) {
            extern void xbox_FramebufferWindowSetTitle(const uint16_t *, int);
            xbox_FramebufferWindowSetTitle(
                (const uint16_t *)(xbe + cert_off + 0x0C), 40);
        }
        /* The title ID (+8): the utility drive Z: is per title. */
        if (cert_off + 0x0C <= xbe_size) {
            extern void xbox_path_set_title_id(uint32_t);
            xbox_path_set_title_id(*(const DWORD *)(xbe + cert_off + 8));
        }
        int sections_short = 0;
        size_t total_bytes = 0;

        /* This image's ranges, not the union with a previous init's: the app
         * shell's self-test (XBA-51) maps the synthetic XBE, shuts down, and
         * the title then maps its own in the same process. */
        g_xbox_image_lo = g_xbox_image_hi = 0;
        g_xbox_code_lo = g_xbox_code_hi = 0;

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* Validate: section must fit within our 64MB mapped region */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /*
             * Copy initialized data from XBE.
             *
             * A section whose raw data runs past the end of the buffer is a
             * truncated or corrupt image, not a BSS section, and it must not
             * be counted among the sections loaded. Reporting it as loaded is
             * how a 4MB title read through a 1MB buffer produced "Loaded
             * 17/17 sections" with every byte of every section still zero --
             * including the kernel thunk table, which then resolved 0 imports
             * and looked like a title that calls no kernel functions.
             */
            int have_data = (copy_size == 0) ||
                            (sec_raw_off + copy_size <= xbe_size);
            if (copy_size > 0 && have_data) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            } else if (!have_data) {
                fprintf(stderr,
                        "  WARNING: section %u (%s) raw data 0x%08X+%u runs past "
                        "the %zu-byte image -- left zeroed\n",
                        si, sec_name, sec_raw_off, copy_size, xbe_size);
                sections_short++;
            }

            /* Every loaded section, executable or not. Anything that writes
             * guest memory from outside the title -- the pushbuffer executor
             * clearing a surface, say -- needs to know where the title itself
             * lives, because scribbling on it is not a rendering artefact, it
             * is the title's code and globals gone. */
            if (!g_xbox_image_lo || sec_va < g_xbox_image_lo)
                g_xbox_image_lo = sec_va;
            if (sec_va + sec_vsize > g_xbox_image_hi)
                g_xbox_image_hi = sec_va + sec_vsize;

            /* Executable sections define the range indirect calls may target.
             * XBE section flag 0x04 is EXECUTABLE. */
            if (*(const DWORD *)(sh + SECTHDR_FLAGS) & 0x00000004u) {
                if (!g_xbox_code_lo || sec_va < g_xbox_code_lo)
                    g_xbox_code_lo = sec_va;
                if (sec_va + sec_vsize > g_xbox_code_hi)
                    g_xbox_code_hi = sec_va + sec_vsize;
            }

            if (have_data) {
                sections_loaded++;
                total_bytes += copy_size;
            }

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);
        if (sections_short) {
            fprintf(stderr,
                    "  ERROR: %d section(s) had no data in the image -- the XBE "
                    "is truncated or was read short; the title will not run\n",
                    sections_short);
        }
    }

    /*
     * Place the toolkit region above the image (xbox_memory_layout.h). Has to
     * come after the section loop -- that is what sets g_xbox_image_hi -- and
     * before anything below writes the scratch structures, the stack or the
     * heap.
     */
    {
        const uint32_t MB = 1024u * 1024u;
        uint32_t image_end = (g_xbox_image_hi + 0xFFFFu) & ~0xFFFFu;
        uint32_t region = image_end > XBOX_REGION_MIN ? image_end : XBOX_REGION_MIN;
        uint32_t stack_base = region + XBOX_REGION_STACK_OFF;
        uint32_t top = XBOX_HEAP_TOP;
        uint32_t avail = top > stack_base ? top - stack_base : 0;
        uint32_t stack_size = 8 * MB;

        if (avail < 2 * MB) {
            fprintf(stderr,
                    "  ERROR: the title image ends at 0x%08X; with the toolkit "
                    "region at 0x%08X that leaves %u KB below the end of RAM "
                    "(0x%08X), less than the 1 MB stack + 1 MB heap minimum\n",
                    g_xbox_image_hi, region, avail / 1024, top);
            return FALSE;
        }
        if (avail - stack_size < 16 * MB) {
            stack_size = avail > 17 * MB ? (avail - 16 * MB) & ~0xFFFFu : 1 * MB;
            if (stack_size < 1 * MB) stack_size = 1 * MB;
        }

        g_xbox_region_base = region;
        g_xbox_stack_base  = stack_base;
        g_xbox_stack_size  = stack_size;
        xbox_heap_reset_base();

        fprintf(stderr, "  Toolkit region: 0x%08X (image ends 0x%08X)%s\n",
                region, g_xbox_image_hi,
                region != XBOX_REGION_MIN ? " -- moved above the image" : "");
        if (stack_size != 8 * MB)
            fprintf(stderr, "  Stack shrunk to %u KB so the heap keeps %u MB\n",
                    stack_size / 1024, (avail - stack_size) / MB);
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    /* The exception dispatcher only follows SEH records on this stack. */
    xbox_SehSetGuestStack(XBOX_STACK_BASE, XBOX_STACK_BASE + XBOX_STACK_SIZE);
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = stack base (top of stack)
     *   fs:[0x08] = stack limit (bottom of stack)
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (→ fake structure)
     *   fs:[0x28] = TLS / RW engine context pointer
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB at address 0x0 */
        MEM32_INIT(XBOX_FS_BASE + 0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(XBOX_FS_BASE + 0x04, XBOX_STACK_TOP);   /* Stack base (high address) */
        MEM32_INIT(XBOX_FS_BASE + 0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(XBOX_FS_BASE + 0x18, XBOX_FS_BASE);     /* Self pointer */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure. We set it to 0 so the read
         * at offset 0x250 returns 0, causing the cache init to be skipped.
         */
        /* A zeroed block rather than a null pointer. The read is
         * [fs:[0x20] + 0x250], and this used to be left at 0 so that read
         * landed on guest address 0x250 and returned zero by accident -- which
         * only worked while page zero was mapped. Pointing at real zeroed
         * memory says the same thing to the title and survives that page being
         * unmapped, which is what makes a genuine null dereference visible. */
        #define FAKE_PRCB_VA (g_xbox_region_base + XBOX_REGION_PRCB_OFF)  /* zeroed KPCR Prcb stand-in */
        memset(XBOX_VA(FAKE_PRCB_VA), 0, 0x400);
        MEM32_INIT(XBOX_FS_BASE + 0x20, FAKE_PRCB_VA);
        #undef FAKE_PRCB_VA

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at region+0x60000
         * and a data buffer at the region base (0x00760000 / 0x00700000 for
         * small images).
         */
        #define FAKE_TLS_VA     (g_xbox_region_base + XBOX_REGION_TLS_OFF)  /* Fake TLS structure (in BSS) */
        #define FAKE_RWDATA_VA  (g_xbox_region_base)  /* RW engine data area (in BSS) */

        MEM32_INIT(XBOX_FS_BASE + 0x28, FAKE_TLS_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_RWDATA_VA);

        /*
         * XBE TLS directory.
         *
         * An image with __declspec(thread) data carries one, and on hardware
         * the loader acts on it. Nothing here did, so thread-local access read
         * whatever memory happened to be under fs:[4].
         *
         * Xbox reaches thread-local data through NtTib.StackBase -- fs:[4] --
         * not Win32's fs:[0x2C], and the block sits BELOW that pointer: the
         * image's entry point computes its own index, negative, as
         * -(blocksize/4). Wreckless does this at guest 0x000EB57E and arrives
         * at -5 for its 20-byte block, so [fs:[4] + index*4] is the block's
         * first dword. The rounding below mirrors that arithmetic exactly,
         * because fs:[4] has to land where the title's own index says it is.
         *
         * The index itself is deliberately NOT written here: the title
         * computes and stores it. What the loader owes it is a block in the
         * right place.
         *
         * Slot 0 holds a pointer to per-thread data -- XAPI's SetLastError is
         * [[fs:[4] + index*4] + 4] = err -- so it gets a zeroed block rather
         * than being left NULL, which had SetLastError writing the error code
         * over fs:[4] itself and the next call faulting at guest 0xFFFFFFEF.
         *
         * ponytail: one block for the whole process, not one per thread.
         * Every guest thread therefore shares LastError. Give this a per-thread
         * allocation when a title is observed to care.
         */
        #define FAKE_TLS_BLOCK_VA  (g_xbox_region_base + XBOX_REGION_TLSBLK_OFF)  /* image TLS data          */
        #define FAKE_TLS_THREAD_VA (g_xbox_region_base + XBOX_REGION_TLSBLK_OFF + 0x200)  /* what slot 0 points at   */
        {
            DWORD tls_dir_va = *(const DWORD *)(xbe + XBE_TLS_ADDR_OFFSET);

            if (tls_dir_va) {
                const uint32_t *tls = (const uint32_t *)XBOX_VA(tls_dir_va);
                uint32_t data_start = tls[0];
                uint32_t data_end   = tls[1];
                uint32_t zero_fill  = tls[4];
                uint32_t init_size  = (data_end > data_start)
                                    ? data_end - data_start : 0;
                uint32_t total      = ((init_size + zero_fill + 0xF) & ~0xFu) + 4;

                memset(XBOX_VA(FAKE_TLS_BLOCK_VA), 0, total);
                memset(XBOX_VA(FAKE_TLS_THREAD_VA), 0, 64);
                if (init_size)
                    memcpy(XBOX_VA(FAKE_TLS_BLOCK_VA),
                           XBOX_VA(data_start), init_size);

                MEM32_INIT(FAKE_TLS_BLOCK_VA, FAKE_TLS_THREAD_VA);
                MEM32_INIT(XBOX_FS_BASE + 0x04, FAKE_TLS_BLOCK_VA + total);

                g_tls_template_va = FAKE_TLS_BLOCK_VA;
                g_tls_total       = total;

                fprintf(stderr, "  TLS: %u-byte block at 0x%08X,"
                        " fs:[4] = 0x%08X (index will be %d)\n",
                        total, FAKE_TLS_BLOCK_VA, FAKE_TLS_BLOCK_VA + total,
                        -(int)(total / 4));
            }
        }
        #undef FAKE_TLS_BLOCK_VA
        #undef FAKE_TLS_THREAD_VA

        fprintf(stderr, "  TIB: fake TIB at VA 0x%X, TLS at 0x%08X, RW data at 0x%08X\n",
                XBOX_FS_BASE, FAKE_TLS_VA, FAKE_RWDATA_VA);

        #undef FAKE_TLS_VA
        #undef FAKE_RWDATA_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Contiguous / physical memory window at 0x80000000.
     *
     * MmAllocateContiguousMemory hands back addresses in this window: physical
     * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
     * physical addresses then use the whole range, so it has to be backed for
     * its full length - Halo pins 3.4 MB at 0x61000 and 22 MB at 0x3A6000, and
     * with only the fake kernel page mapped here a write walked off the end of
     * it a few pages in.
     *
     * Deliberately NOT a view of the 64 MB RAM mapping. On hardware this window
     * aliases physical RAM, but we load the XBE image into the low addresses of
     * that same region, so aliasing would put a title's pinned pools on top of
     * its own code. Separate storage costs an extra mapping and behaves
     * correctly; nothing here depends on the aliasing.
     *
     * Reserved before the kernel page below, which lives inside it.
     */
    {
        uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;
        g_contig_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            0, (DWORD)XBOX_CONTIG_SIZE, NULL);
        g_contig_memory = g_contig_mapping
            ? layout_map_view(g_contig_mapping, XBOX_CONTIG_SIZE,
                              (LPVOID)contig_native)
            : NULL;
        if (!g_contig_memory)
            g_contig_memory = layout_commit_at((LPVOID)contig_native,
                                               XBOX_CONTIG_SIZE);
        if (g_contig_memory) {
            fprintf(stderr, "  Contiguous window: %u MB at Xbox VA 0x%08X\n",
                    XBOX_CONTIG_SIZE / (1024 * 1024), XBOX_CONTIG_BASE);
        } else {
            /* Say what is in the way. Under Wine the base view lands wherever
             * the loader left room, and 0x80000000 + offset can be taken. */
            DWORD err = GetLastError();
            MEMORY_BASIC_INFORMATION mbi;
            fprintf(stderr, "  WARNING: contiguous window at 0x%08X (native %p) "
                    "failed (error %lu); pinned physical allocations will fault\n",
                    XBOX_CONTIG_BASE, (void *)contig_native, err);
            if (VirtualQuery((LPCVOID)contig_native, &mbi, sizeof(mbi)))
                fprintf(stderr, "    occupant: base %p alloc-base %p size 0x%zx "
                        "state 0x%lx type 0x%lx\n",
                        mbi.BaseAddress, mbi.AllocationBase, mbi.RegionSize,
                        (unsigned long)mbi.State, (unsigned long)mbi.Type);
        }
    }

    /*
     * NV2A hardware register aperture at 0xFD000000 (16 MB).
     *
     * The GPU's registers are memory-mapped here on real hardware. A title
     * that only calls D3D never notices, but the D3D8 library is linked into
     * the XBE rather than provided by the kernel, so once execution is inside
     * it the register pokes are just loads and stores in recompiled code.
     * Halo faults reading 0xFD001804 during rasterizer_preinitialize, a few
     * instructions after Direct3DCreate8 returns.
     *
     * Backed as ordinary zeroed RAM. That is enough to get through
     * initialisation, and reads returning zero are the benign answer for the
     * status and capability registers touched here.
     *
     * ponytail: plain memory, no register semantics. A spin loop waiting for
     * a bit to *set* would hang rather than fault -- if that shows up, the fix
     * is to bridge the D3D8 entry point that owns the loop, not to start
     * emulating NV2A. Nothing has needed that yet.
     */
    {
        uintptr_t nv2a_native = XBOX_NV2A_BASE + g_memory_offset;
        g_nv2a_memory = layout_commit_at((LPVOID)nv2a_native, XBOX_NV2A_SIZE);
        /* The pushbuffer survey rides on the same poll, so either
         * variable arms it. */
        s_nv2a_trace_kicks = getenv("RECOMP_NV2A_TRACE") != NULL;
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL
                    || getenv("RECOMP_PB_EXEC") != NULL;
        if (g_nv2a_memory) {
            fprintf(stderr, "  NV2A register aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, no register semantics)\n",
                    XBOX_NV2A_SIZE / (1024 * 1024), XBOX_NV2A_BASE);
        } else {
            fprintf(stderr, "  WARNING: NV2A aperture at 0x%08X failed "
                    "(error %lu); D3D register access will fault\n",
                    XBOX_NV2A_BASE, GetLastError());
        }
    }

    /*
     * MCPX device apertures.
     *
     * The NV2A block above is not the only hardware the title touches
     * directly. The southbridge devices live higher up:
     *
     *   0xFE800000  APU (audio processing unit)
     *   0xFEC00000  AC97
     *   0xFED00000  USB0 / USB1
     *   0xFEF00000  NIC
     *
     * Halo faults reading 0xFED00000 during input initialisation -- the XDK's
     * USB code talks to the host controller's registers rather than going
     * through a driver. Back the whole span as plain RAM for the same reason
     * the NV2A aperture is backed: a read of zero is survivable, a fault is
     * not.
     *
     * ponytail: no register semantics anywhere in here. If something spins
     * waiting for a bit to set, extend the NV2A ack thread's table rather than
     * emulating the device.
     */
    {
        uintptr_t mcpx_native = XBOX_MCPX_BASE + g_memory_offset;
        g_mcpx_memory = layout_commit_at((LPVOID)mcpx_native, XBOX_MCPX_SIZE);
        g_mcpx_regs = g_mcpx_memory;
        if (g_mcpx_memory) {
            /* AC'97 codec ready.
             *
             * DirectSound resets the codec by setting a bit in 0xFEC0012C and
             * then polls 0xFEC00130 for bit 8 a thousand times waiting for the
             * codec to come up. On zeroed registers that bit never appears, so
             * the wait times out and DirectSoundCreate returns DSERR_NODRIVER
             * (0x88780078).
             *
             * That failure is not confined to audio. Wreckless initialises its
             * whole engine object behind `if (DirectSoundCreate() >= 0)`, so a
             * failed create skips the initialisation, leaves the object's table
             * pointer null, and the null propagates: a null-derived divisor
             * produces a NaN transform matrix, which produces a garbage index,
             * which crashes. Reporting the codec as present is what lets the
             * engine initialise at all.
             *
             * The aperture is plain memory, so setting the bit once is enough:
             * nothing clears it, and the poll reads it on the first pass. */
            #define MCPX_AC97_CODEC_STATUS 0x00400130u   /* 0xFEC00130 */
            #define MCPX_AC97_CODEC_READY  0x00000100u
            /* Opt-in, and not because it is wrong.
             *
             * Reporting the codec is the correct answer -- DSERR_NODRIVER is
             * not what hardware returns -- but it is only correct as far as it
             * goes. DirectSound then hands the audio DSP a command block in
             * RAM and spins until the DSP clears it, and there is no DSP here,
             * so the title trades a late crash for an early hang: 44 assets
             * loaded and then a fault, versus one asset and a stall in audio
             * init. Until the DSP handshake is answered, the honest default is
             * the failure that gets further, with the correct behaviour one
             * variable away. */
            if (getenv("RECOMP_AC97_READY")) {
                /* The APU's registers have to fault so they can be routed to
                 * the emulated APU, which is the half that answers the DSP
                 * handshake. Backed as plain memory the guest's writes go
                 * nowhere the APU can see, so it initialises and then waits
                 * forever. Only the APU's own 512K is unmapped: AC'97 above it
                 * stays plain memory, which is what the codec-ready bit needs.
                 *
                 * Enabled by the same variable, because neither half is any
                 * use without the other. */
                /* Registers and the VP only (0x00000-0x2FFFF). The GP and EP
                 * windows above them (0x30000-0x7FFFF) are the DSPs' own
                 * X/Y/P memories, which behave as RAM on hardware and which
                 * the APU model does not implement -- it drops writes there
                 * and reads back 0. Trapping them bought nothing, and cost a
                 * fault on any access the MMIO decoder cannot emulate:
                 * Burnout 3's DirectSound bulk-copies DSP memory with
                 * rep movsd, which the lifter lowers to a host memcpy, and
                 * that faulted at 0xFE830B78 with no way to resume. Plain
                 * memory keeps what the title writes. */
                enum { APU_TRAP_BYTES = 0x00030000 };
                DWORD old_protect;
                if (VirtualProtect((char *)g_mcpx_memory, APU_TRAP_BYTES,
                                   PAGE_NOACCESS, &old_protect))
                    g_apu_mmio_trapped = 1;
                if (g_apu_mmio_trapped)
                    fprintf(stderr, "  APU: 0x%08X..0x%08X trapped for MMIO"
                                    " (GP/EP DSP memory left as RAM)\n",
                            XBOX_MCPX_BASE, XBOX_MCPX_BASE + APU_TRAP_BYTES);
                *(volatile uint32_t *)((char *)g_mcpx_memory
                                       + MCPX_AC97_CODEC_STATUS)
                    |= MCPX_AC97_CODEC_READY;
                /* Before the trap is armed: this write would otherwise be
                 * the first thing to fault. */
                ac97_arm_write_trap();
                fprintf(stderr, "  AC97: codec reported ready at 0x%08X"
                                " (DirectSound will initialise)\n",
                        XBOX_MCPX_BASE + MCPX_AC97_CODEC_STATUS);
            }
            fprintf(stderr, "  MCPX device aperture: %u MB at Xbox VA "
                    "0x%08X (APU/AC97/USB/NIC, zeroed)\n",
                    XBOX_MCPX_SIZE / (1024 * 1024), XBOX_MCPX_BASE);
        } else {
            fprintf(stderr, "  WARNING: MCPX aperture at 0x%08X failed "
                    "(error %lu); USB/audio register access will fault\n",
                    XBOX_MCPX_BASE, GetLastError());
        }
    }

    /* Flash ROM aperture -- see XBOX_FLASH_BASE for why. */
    {
        uintptr_t flash_native = XBOX_FLASH_BASE + g_memory_offset;

        g_flash_memory = layout_commit_at((LPVOID)flash_native,
                                          XBOX_FLASH_SIZE);
        if (g_flash_memory) {
            fprintf(stderr, "  Flash ROM aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, not a real BIOS image)\n",
                    XBOX_FLASH_SIZE / (1024 * 1024), XBOX_FLASH_BASE);
        } else {
            fprintf(stderr, "  WARNING: flash aperture at 0x%08X failed "
                    "(error %lu); a title reading flash will fault\n",
                    XBOX_FLASH_BASE, GetLastError());
        }
    }

    if (g_nv2a_memory) {
        xbox_Nv2aAckStart();
    }

    /*
     * Allocate a page at Xbox kernel address space (0x80010000).
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * The actual native address is 0x80010000 + g_memory_offset.
     */
    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096
        /* Windows reserves at 64 KB granularity, and a placeholder piece is
         * carved at the same granularity, so ask for what the OS gives. */
#if defined(_WIN32)
        #define KERNEL_ALLOC_SIZE 0x10000
#else
        #define KERNEL_ALLOC_SIZE KERNEL_PAGE_SIZE
#endif
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;
        /* Already committed if the contiguous window above succeeded -
         * 0x80010000 sits inside it - so just use that storage. */
        g_kernel_memory = g_contig_memory
            ? (void *)kernel_native
            : layout_commit_at((LPVOID)kernel_native, KERNEL_ALLOC_SIZE);
        if (g_kernel_memory) {
            /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
             * With the rest zeroed, NumberOfSections = 0 and the INIT
             * section search finds nothing, which is the safe path. */
            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;  /* e_lfanew */
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
        #undef KERNEL_ALLOC_SIZE
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            (unsigned)((XBOX_HEAP_TOP - XBOX_HEAP_BASE) / (1024 * 1024)),
            XBOX_HEAP_BASE, XBOX_HEAP_TOP);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        /* The tiled aperture is a specific architectural alias -- physical RAM
         * a second time at 0xF0000000, which is where titles render -- while
         * these mirrors are a generic emulation of the address wrap. When the
         * mapped size is large enough that a mirror would cover 0xF0000000,
         * the mirror wins the address and the tiled mapping fails with
         * ERROR_INVALID_ADDRESS; Half-Life 2 then faults on its first surface
         * write. The specific alias is worth more than one wrap mirror, so
         * skip any that would overlap it.
         *
         * Guest addresses, not host: mirror m covers guest
         * (m + 1) * g_memory_size. */
        uint64_t tiled_lo = XBOX_TILED_BASE;
        uint64_t tiled_hi = tiled_lo + xbox_TiledApertureSize();

        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            uint64_t guest_lo = (uint64_t)(m + 1) * g_memory_size;
            uint64_t guest_hi = guest_lo + g_memory_size;

            if (guest_lo < tiled_hi && tiled_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_TILED_BASE);
                continue;
            }
            /* The same goes for the contiguous window and the device
             * apertures, which are mapped above, before the mirrors. With
             * 128 MB (devkit) the 16th mirror lands on 0x80000000 exactly. */
            if (guest_lo < (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE
                    && XBOX_CONTIG_BASE < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the contiguous"
                                " window at 0x%08X\n",
                        m + 1, (unsigned)XBOX_CONTIG_BASE);
                continue;
            }
            if (guest_lo < (uint64_t)XBOX_FLASH_BASE + XBOX_FLASH_SIZE
                    && XBOX_NV2A_BASE < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the device"
                                " apertures at 0x%08X\n",
                        m + 1, (unsigned)XBOX_NV2A_BASE);
                continue;
            }
            /* Inside the span the view replaces its slice of the reservation
             * (span_holds). This loop used to free the slice first, in every
             * case: outside the span (128 MB, whose span stops at the
             * contiguous window) that was a munmap of a range we never held. */
            g_mirror_views[m] = layout_map_view(g_mapping_handle,
                                                g_memory_size,
                                                (LPVOID)mirror_base);
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    /* Guest address space above the mirrors, for titles that ask for a specific
     * high address. RECOMP_EXT_VMA; a no-op without it. Straight after the
     * mirrors, while a fixed host address is still likely to be free. */
    {
        uint64_t mirror_top = (uint64_t)g_memory_size * (1u + XBOX_NUM_MIRRORS);
        uint64_t ext = ext_vma_span(mirror_top);

        /* guest_vmem reserves its range itself, with a plain fixed-address
         * reservation, and only where the host reports it free. Our own hold
         * on that range has to go first: on Windows the 4 GB placeholder
         * window covers it, on POSIX the span above. Nothing happens here
         * with the switch off. */
        if (ext) {
            uintptr_t host = (uintptr_t)(mirror_top + (uint64_t)g_memory_offset);
#if defined(_WIN32)
            if (win_in_window(host, (size_t)ext)) {
                win_split_placeholder(host, (size_t)ext);
                VirtualFree((LPVOID)host, 0, MEM_RELEASE);
            }
#else
            /* From here on the range is guest_vmem's: it reserves what the
             * host leaves free there and releases that itself, and the span
             * release at shutdown skips the whole range (span_holds), so
             * nothing is unmapped twice. */
            if (span_holds((void *)host, (size_t)ext)) {
                VirtualFree((LPVOID)host, (SIZE_T)ext, MEM_RELEASE);
                g_span_gap_lo = host;
                g_span_gap_hi = host + (uintptr_t)ext;
            }
#endif
        }
        guest_vmem_init(g_memory_offset, g_memory_size, mirror_top);
    }

    /*
     * Tiled / write-combined aperture at 0xF0000000.
     *
     * The NV2A exposes physical RAM a second time here and titles render
     * through it. Wreckless's first surface write goes to guest 0xF1954000 --
     * the tiled alias of physical 0x01954000, already inside our RAM -- and
     * faulted because nothing was mapped there.
     *
     * A view of the same section rather than fresh storage: the title writes a
     * surface through the tiled address and reads it back through the normal
     * one, so the two have to be the same bytes. That is the whole reason the
     * RAM lives in a file mapping.
     */
    {
        uintptr_t tiled_native = XBOX_TILED_BASE + g_memory_offset;
        size_t tiled_size = xbox_TiledApertureSize();
        /* A view of the CONTIGUOUS window, not of RAM.
         *
         * On hardware all three -- physical P, 0x80000000+P and 0xF0000000+P
         * -- are one and the same memory. Here they cannot be: the XBE image
         * is loaded at its own VA in the RAM mapping, so aliasing the
         * contiguous window onto RAM would drop a title's pinned physical
         * pools on top of its own code (Halo pins 3.4 MB at 0x61000, which is
         * inside its image). The contiguous window therefore has separate
         * storage, and the question becomes which of the two the tiled
         * aperture should be a view of.
         *
         * It is the contiguous one. A tiled address is a GPU surface address
         * by construction, and GPU surfaces come from
         * MmAllocateContiguousMemory -- so the pairing that has to hold is
         * tiled to contiguous. Against RAM instead, Half-Life 2's loader wrote
         * every decoded video frame through 0xF1C63000 while D3D sampled the
         * texture at 0x81C63000, and the sampler read zeros: 1.8 billion black
         * pixels rasterised, perfectly, from an empty texture.
         */
        if (tiled_size > XBOX_CONTIG_SIZE)
            tiled_size = XBOX_CONTIG_SIZE;
        g_tiled_view = g_contig_mapping
            ? layout_map_view(g_contig_mapping, tiled_size,
                              (LPVOID)tiled_native)
            : NULL;
        if (g_tiled_view) {
            /* Prove the alias rather than assert it. Everything the title
             * renders goes through this window and is read back through the
             * physical address, so if the two are not the same bytes the GPU
             * sees empty buffers and the screen stays black -- with nothing
             * anywhere to say why. One write and one read turns that into a
             * startup line. Only when the contiguous window is where it
             * should be: if it fell back or failed, reading it would fault. */
            if (g_contig_memory !=
                    (void *)(uintptr_t)(XBOX_CONTIG_BASE + g_memory_offset)) {
                fprintf(stderr, "  WARNING: tiled aperture alias not checked:"
                        " contiguous window is not mapped\n");
            } else {
                volatile uint32_t *via_tiled =
                    (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE + 0x1000)
                                          + g_memory_offset);
                volatile uint32_t *via_contig =
                    (volatile uint32_t *)((uintptr_t)(XBOX_CONTIG_BASE + 0x1000)
                                          + g_memory_offset);
                uint32_t saved = *via_contig;

                *via_tiled = 0xA5C30F17u;
                if (*via_contig != 0xA5C30F17u)
                    fprintf(stderr, "  WARNING: tiled aperture does NOT alias"
                            " the contiguous window (wrote A5C30F17, read"
                            " %08X) -- the GPU will sample empty textures\n",
                            *via_contig);
                else
                    fprintf(stderr, "  Tiled aperture alias verified"
                            " (tiled 0x%08X == contiguous 0x%08X)\n",
                            XBOX_TILED_BASE, XBOX_CONTIG_BASE);
                *via_contig = saved;
            }
            fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X"
                    " (aliases the contiguous window)\n",
                    (unsigned)(g_memory_size / (1024 * 1024)),
                    XBOX_TILED_BASE);
        } else {
            fprintf(stderr, "  WARNING: tiled aperture at 0x%08X failed"
                    " (error %lu); rendering writes will fault\n",
                    XBOX_TILED_BASE, GetLastError());
        }
    }

#if defined(_WIN32)
    if (g_win_window_size)
        win_settle_placeholders();
#endif
    lazy_reset();
    xbox_WatchInit();
    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

/*
 * Make every RAM mirror read-only, for finding writes that reach low memory
 * through an alias.
 *
 * Xbox RAM is visible at 28 virtual addresses that alias the same pages, so a
 * store to 0x04000004 changes Xbox VA 4 without ever touching VA 4. Both a
 * page-protection watchpoint and a DR0 hardware watchpoint on VA 4 therefore
 * report nothing while the memory demonstrably changes -- which is exactly
 * what happened chasing Halo's fs:[4] corruption.
 *
 * Debug aid, not part of normal startup: a title that legitimately writes
 * through a mirror will fault here too, and the fault address names the alias
 * and the code.
 */
void xbox_ProtectMirrorsForDebug(void)
{
    int n = 0;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        uint64_t lo = (uint64_t)(m + 1) * g_memory_size;
        /* Not a mirror any more where a lazy reservation replaced it. */
        if (g_lazy_next < g_lazy_top && lo + g_memory_size > g_lazy_next)
            continue;
        if (g_mirror_views[m] &&
            VirtualProtect(g_mirror_views[m], g_memory_size,
                           PAGE_READONLY, &old)) {
            n++;
        }
    }
    fprintf(stderr, "  Mirrors: %d/%d made read-only (debug)\n",
            n, XBOX_NUM_MIRRORS);
}

int xbox_GuestWindowHeld(void)
{
#if defined(_WIN32)
    return g_win_window_size != 0;
#else
    return g_span_whole_window;
#endif
}

void xbox_MemoryLayoutShutdown(void)
{
    /* The ack thread first: every tick writes the NV2A aperture and guest RAM
     * (KeTickCount), so nothing it touches may go before it has stopped. It
     * used to be given 1000 ms and then left running, with g_kernel_memory
     * already released. */
    if (g_nv2a_ack_thread) {
        InterlockedExchange(&g_nv2a_ack_stop, 1);
        if (WaitForSingleObject(g_nv2a_ack_thread, 5000) == WAIT_TIMEOUT) {
            fprintf(stderr, "xbox_MemoryLayoutShutdown: NV2A ack thread still"
                            " running after 5 s; waiting for it\n");
            WaitForSingleObject(g_nv2a_ack_thread, INFINITE);
        }
        CloseHandle(g_nv2a_ack_thread);
        g_nv2a_ack_thread = NULL;
    }
    /* Inside the span (POSIX), layout_release only forgets a window: the span
     * release below unmaps them all in one call, so no hole opens in the span
     * for another thread's mmap to land in and be unmapped with it
     * (span_holds). Outside it, each window is unmapped as before. */
    if (g_kernel_memory) {
        /* Inside the contiguous window when that mapped, and then not a
         * registered range: a no-op, the window's release covers it. */
        layout_release(g_kernel_memory, 0);
        g_kernel_memory = NULL;
    }
    if (g_nv2a_memory) {
        layout_release(g_nv2a_memory, 0);
        g_nv2a_memory = NULL;
    }
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            layout_release(g_mirror_views[m], 1);
            g_mirror_views[m] = NULL;
        }
    }
    if (g_memory_base) {
        layout_release(g_memory_base, 1);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
    /* The lazy reservations go with the span below; nothing is held now. */
    AcquireSRWLockExclusive(&g_lazy_lock);
    g_lazy_count = 0;
    g_lazy_top = g_lazy_next = g_lazy_floor = 0;
    ReleaseSRWLockExclusive(&g_lazy_lock);
    /* The apertures. Left mapped, a second init cannot place them: the first
     * run still owns 0x80000000, 0xFD000000, 0xFE800000, 0xFF000000 and the
     * tiled alias, and every one of those comes back as "failed" while init
     * still returns TRUE because they are best-effort. The result is a layout
     * that looks initialised and has no device apertures at all. */
    if (g_tiled_view) {
        layout_release(g_tiled_view, 1);
        g_tiled_view = NULL;
    }
    if (g_contig_memory) {
#if defined(_WIN32)
        /* Usually a view of g_contig_mapping, which VirtualFree cannot
         * release. Committed memory only when that view failed. */
        if (!UnmapViewOfFile(g_contig_memory))
            VirtualFree(g_contig_memory, 0, MEM_RELEASE);
#else
        layout_release(g_contig_memory, 0);
#endif
        g_contig_memory = NULL;
    }
#if defined(_WIN32)
    if (g_contig_mapping) {
        CloseHandle(g_contig_mapping);
        g_contig_mapping = NULL;
    }
#endif
    if (g_mcpx_memory) {
        ac97_disarm_write_trap();
        layout_release(g_mcpx_memory, 0);
        g_mcpx_memory = NULL;
    }
    if (g_flash_memory) {
        layout_release(g_flash_memory, 0);
        g_flash_memory = NULL;
    }

    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }

    /* Its own reservation, inside the gap the span release skips. */
    guest_vmem_shutdown();

    /* The span, with every window still in it: one munmap, or one on each
     * side of guest_vmem's range, which is not ours to unmap a second time. */
    if (g_span_base) {
        uintptr_t lo = (uintptr_t)g_span_base, hi = lo + g_span_size;
        if (g_span_gap_hi > g_span_gap_lo) {
            if (g_span_gap_lo > lo)
                VirtualFree(g_span_base, g_span_gap_lo - lo, MEM_RELEASE);
            if (hi > g_span_gap_hi)
                VirtualFree((LPVOID)g_span_gap_hi, hi - g_span_gap_hi,
                            MEM_RELEASE);
        } else {
            VirtualFree(g_span_base, g_span_size, MEM_RELEASE);
        }
        g_span_base = NULL;
        g_span_size = 0;
        g_span_whole_window = 0;
        g_span_gap_lo = g_span_gap_hi = 0;
    }
#if defined(_WIN32)
    /* The rest of the Windows guest window: the leftover placeholders and
     * reservations between the windows released above. */
    if (g_win_window_size)
        win_release_window();
#endif
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

/* Bump allocator for pure address-space reservations, above RAM.
 *
 * A MEM_RESERVE costs no memory on real hardware -- it takes address space out
 * of a 4 GB range, not pages out of the 64 MB the console has -- so titles
 * reserve far more than exists and commit a fraction. Satisfying that out of
 * the RAM heap does not work: Half-Life 2 asks for 128 MB and then 200 MB, and
 * clamping those to what the heap can back left it sub-allocating across a
 * range it believed it owned, walking past the top of RAM and aliasing low
 * memory through the mirrors.
 *
 * So reservations come from the mapped space *above* RAM instead. Those pages
 * are already backed and distinct, nothing else hands them out, and a commit
 * inside one is a no-op because it is real memory already.
 *
 * That range only exists when the title asked for a mapping larger than RAM
 * (xbox_SetMapSize). Without one, reservations come from lazy_reserve below,
 * which needs no up-front size (XBA-103).
 *
 * ponytail: a bump allocator with no free. A reservation is address space, the
 * range is large, and a title that reserves and releases repeatedly would need
 * a real allocator; none has yet.
 */
static uint32_t g_reserve_next;

/* Lazy reservations (XBA-103): address space the title reserves, taken from
 * the window the layout already holds, with no map size set in advance.
 *
 * Fable reserves 608 MB for its own allocator and Half-Life 2 reserves 128 MB
 * and then 200 MB. Both used to need xbox_SetMapSize, which grows the RAM
 * mapping for every title-visible address up to that size, moves the mirrors
 * out by the same amount, and has to be picked per title. Here a reservation
 * is carved top-down from GUEST_VMEM_TOP (0x7FFE0000, the end of Xbox user
 * space) and its host range is replaced, in one mmap(MAP_FIXED) inside the span
 * we hold, by fresh anonymous memory: MAP_NORESERVE, so it costs no commit
 * charge, and its pages exist only once the title touches them (zero-filled,
 * which is what a commit gives on the console). Nothing else is ever placed
 * in the span, so MAP_FIXED can only replace our own views.
 *
 * Top-down, because what it replaces are RAM mirrors, and the ones titles walk
 * into (RenderWare's memory walker, writes that wrap past 64 MB) are the low
 * ones: a title that never reserves more than its heap keeps all 28, and one
 * that reserves 608 MB loses mirrors 22-28 only. The range is
 * [g_lazy_floor, g_lazy_top): the floor is the top of the RAM view (or of the
 * xbox_SetMapSize mapping), so it never overlaps what the old path hands out.
 *
 * POSIX only, and only while the span holds the range (the whole-window
 * layout, or the old base + mirrors span below 2 GB); not under RECOMP_EXT_VMA,
 * whose tracker owns everything above the mirrors and refuses addresses inside
 * them. Windows keeps the old path. RECOMP_LAZY_RESERVE=0 turns it off: an
 * over-large reserve is then clamped to what the heap can back, as before.
 *
 * Reservations are listed so a release can find its size and give the pages
 * back; the address is not handed out again (a bump allocator, like the old
 * path). */

static void lazy_reset(void)
{
    AcquireSRWLockExclusive(&g_lazy_lock);
    g_lazy_count = 0;
    g_lazy_floor = (uint32_t)(g_memory_size < GUEST_VMEM_TOP ? g_memory_size : GUEST_VMEM_TOP);
    g_lazy_top = GUEST_VMEM_TOP;
    /* The old base + mirrors span (RECOMP_GUEST_WINDOW=0) stops at 1.8 GB. */
    if (g_span_base && g_span_size < GUEST_VMEM_TOP)
        g_lazy_top = (uint32_t)g_span_size & ~0xFFFFu;
    if (g_lazy_top < g_lazy_floor)
        g_lazy_top = g_lazy_floor;
    g_lazy_next = g_lazy_top;
    g_reserve_next = 0;     /* the old path's cursor: a re-init starts over too */
    ReleaseSRWLockExclusive(&g_lazy_lock);
}

static int lazy_reserve_enabled(void)
{
    const char *e = getenv("RECOMP_LAZY_RESERVE");
    return !(e && *e == '0');
}

#if !defined(_WIN32)
/* Fresh zero-filled pages over [va, va + size), all inside the span. */
static int lazy_fresh_pages(uint32_t va, uint32_t size)
{
    void *host = (void *)((uintptr_t)va + (uintptr_t)g_memory_offset);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    if (!span_holds(host, size))
        return 0;
    if (mmap(host, size, PROT_READ | PROT_WRITE, flags, -1, 0) == host)
        return 1;
    /* A failed MAP_FIXED may have unmapped the range already: hold it again
     * so nothing else lands in the span. */
    mmap(host, size, PROT_NONE, flags, -1, 0);
    return 0;
}
#endif

static uint32_t lazy_reserve(uint32_t size, uint32_t align)
{
#if defined(_WIN32)
    (void)size; (void)align;
    return 0;
#else
    uint64_t rounded = ((uint64_t)size + 0xFFFFu) & ~(uint64_t)0xFFFFu;
    uint32_t base = 0;

    if (!size || !g_span_base || !lazy_reserve_enabled() || guest_vmem_active())
        return 0;
    if (align < 0x10000u)
        align = 0x10000u;           /* the allocation granule */
    if (align & (align - 1))
        return 0;
    AcquireSRWLockExclusive(&g_lazy_lock);
    if (g_lazy_count < LAZY_RESERVE_MAX
            && rounded <= (uint64_t)g_lazy_next - g_lazy_floor) {
        uint32_t want = (uint32_t)((g_lazy_next - rounded) & ~(uint64_t)(align - 1));
        /* The whole run down from the cursor, alignment slack included, so
         * [g_lazy_next, g_lazy_top) stays one run of our own pages. */
        if (want >= g_lazy_floor && lazy_fresh_pages(want, g_lazy_next - want)) {
            base = want;
            g_lazy[g_lazy_count].base = base;
            g_lazy[g_lazy_count].size = (uint32_t)rounded;
            g_lazy[g_lazy_count].released = 0;
            g_lazy_count++;
            g_lazy_next = base;
        }
    }
    ReleaseSRWLockExclusive(&g_lazy_lock);
    if (base)
        fprintf(stderr, "  [MEM] lazy reservation: %u KB at guest 0x%08X-0x%08X"
                " (%u MB of lazy reservations, floor 0x%08X)\n",
                (unsigned)(rounded >> 10), base, (uint32_t)(base + rounded),
                (unsigned)((g_lazy_top - base) >> 20), g_lazy_floor);
    return base;
#endif
}

int xbox_ReserveHolds(uint32_t va, uint32_t bytes)
{
    uint32_t next = g_lazy_next;
    return next < g_lazy_top && va >= next
        && (uint64_t)va + bytes <= (uint64_t)g_lazy_top;
}

/* Index of the reservation holding va, or -1. Caller holds g_lazy_lock. */
static int lazy_find(uint32_t va)
{
    for (int i = 0; i < g_lazy_count; i++)
        if (va >= g_lazy[i].base && va - g_lazy[i].base < g_lazy[i].size)
            return i;
    return -1;
}

int xbox_ReserveFree(uint32_t va, uint32_t size, int release,
                     uint32_t *out_base, uint32_t *out_size)
{
#if defined(_WIN32)
    (void)va; (void)size; (void)release; (void)out_base; (void)out_size;
    return 0;
#else
    int i, ok = 0;
    uint32_t page_base = va & ~0xFFFu, end, lo, hi;
    uint64_t end64;
    long hp = sysconf(_SC_PAGESIZE);
    uint32_t host_page = hp > 0 ? (uint32_t)hp : 4096u;

    AcquireSRWLockExclusive(&g_lazy_lock);
    i = lazy_find(page_base);
    if (i < 0 || g_lazy[i].released) {
        ReleaseSRWLockExclusive(&g_lazy_lock);
        return 0;
    }
    if (release) {
        if (va != g_lazy[i].base) {
            ReleaseSRWLockExclusive(&g_lazy_lock);
            return -1;                         /* not at the allocation base */
        }
        page_base = g_lazy[i].base;
        end = g_lazy[i].base + g_lazy[i].size;
        g_lazy[i].released = 1;
    } else {
        end64 = size ? (((uint64_t)va + size + 0xFFFu) & ~(uint64_t)0xFFFu)
                     : (uint64_t)g_lazy[i].base + g_lazy[i].size;
        if (end64 > (uint64_t)g_lazy[i].base + g_lazy[i].size)
            end64 = (uint64_t)g_lazy[i].base + g_lazy[i].size;
        end = (uint32_t)end64;
    }
    /* The pages go back to the host: fresh ones over every whole host page
     * (16 KB on Apple Silicon), zeroes on the guest pages at either edge. */
    lo = (page_base + host_page - 1) & ~(host_page - 1);
    hi = end & ~(host_page - 1);
    if (lo < hi) {
        ok = lazy_fresh_pages(lo, hi - lo);
        if (page_base < lo)
            memset((void *)((uintptr_t)page_base + (uintptr_t)g_memory_offset), 0, lo - page_base);
        if (hi < end)
            memset((void *)((uintptr_t)hi + (uintptr_t)g_memory_offset), 0, end - hi);
    } else {
        memset((void *)((uintptr_t)page_base + (uintptr_t)g_memory_offset), 0, end - page_base);
        ok = 1;
    }
    ReleaseSRWLockExclusive(&g_lazy_lock);
    if (out_base) *out_base = page_base;
    if (out_size) *out_size = end - page_base;
    return ok ? 1 : 0;
#endif
}

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    uint32_t base;

    if (size == 0)
        return 0;
    if (!align)
        align = 4096;
    if (g_memory_size > g_xbox_total_ram) {
        if (!g_reserve_next)
            g_reserve_next = (uint32_t)g_xbox_total_ram;
        base = (g_reserve_next + align - 1) & ~(align - 1);
        if ((size_t)base + size <= g_memory_size) {
            g_reserve_next = base + size;
            return base;
        }
    }
    return lazy_reserve(size, align);
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

void *xbox_GetMcpxAperture(void)
{
    return g_mcpx_memory;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* ── Dynamic heap allocator ────────────────────────────────
 *
 * Simple bump allocator for MmAllocateContiguousMemory and similar.
 * Returns Xbox VAs within the mapped region so MEM32() works correctly.
 * No free support (bump-only for now).
 */
/* Not a static initializer: the heap base is only known once the image is
 * loaded (xbox_heap_reset_base, from xbox_MemoryLayoutInit). */
static uint32_t g_heap_next = 0;

static void xbox_heap_reset_base(void)
{
    g_heap_next = XBOX_HEAP_BASE;
}

static int g_heap_alloc_count = 0;

/* Block table backing xbox_HeapFree. A bump pointer alone never reclaims,
 * which is fine for a title that allocates once and fatal for a debug build
 * that churns. Flat array rather than an intrusive list: allocations come back
 * in bump order, so index order is address order and coalescing is a
 * neighbour check. */
#define XBOX_HEAP_MAX_BLOCKS 65536
/* `named`: MmGetPhysicalAddress was given an address inside this live block
 * (xbox_PhysNoteRamName); cleared whenever the block is handed out or freed. */
static struct { uint32_t addr; uint32_t size; uint8_t free; uint8_t named; }
    g_heap_blocks[XBOX_HEAP_MAX_BLOCKS];
static int g_heap_block_count = 0;

/* Guest threads allocate at once (audio, movie and streaming threads next to
 * the main one), and the table above is shared state. */
static SRWLOCK g_heap_lock = SRWLOCK_INIT;

/* RECOMP_HEAP_RECLAIM: give memory back properly.
 *
 * Reuse below hands a whole freed block to whatever asks next, however small,
 * and freeing skips over the empty slots that merging leaves behind, so a
 * title that allocates and frees a lot drains the heap far faster than it
 * uses it. Fixing that changes which address every later allocation gets, in
 * every title that ever frees, so it is off unless asked for. Read once: the
 * heap is hit constantly and the answer does not change. */
int xbox_HeapReclaimEnabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_HEAP_RECLAIM") != NULL;
    return on;
}

/* Insert a free block at index `at`, keeping address order. Reuses an empty
 * slot (size 0, left behind by coalescing) when one is already there. */
static int heap_insert_free(int at, uint32_t addr, uint32_t size)
{
    if (!(at < g_heap_block_count && g_heap_blocks[at].size == 0)) {
        if (g_heap_block_count >= XBOX_HEAP_MAX_BLOCKS)
            return 0;
        memmove(&g_heap_blocks[at + 1], &g_heap_blocks[at],
                (size_t)(g_heap_block_count - at) * sizeof g_heap_blocks[0]);
        g_heap_block_count++;
    }
    g_heap_blocks[at].addr = addr;
    g_heap_blocks[at].size = size;
    g_heap_blocks[at].free = 1; g_heap_blocks[at].named = 0;
    return 1;
}

/* Take exactly `size` bytes, aligned, out of the first free block that has room
 * for them; what is left in front of and behind the piece stays free. Returns 0
 * when no free block fits. Caller holds g_heap_lock. */
static uint32_t heap_carve_free(uint32_t size, uint32_t alignment)
{
    for (int i = 0; i < g_heap_block_count; i++) {
        uint32_t a, start, end, front, back;
        if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size)
            continue;
        a = g_heap_blocks[i].addr;
        start = (a + alignment - 1) & ~(alignment - 1);
        end = a + g_heap_blocks[i].size;
        if (start + size > end || start + size < start)
            continue;   /* not enough room once aligned */
        front = start - a;
        back = end - (start + size);
        if (front && !heap_insert_free(i, a, front))
            continue;   /* table full: leave this block alone */
        if (front)
            i++;        /* the taken piece moved up one slot */
        g_heap_blocks[i].addr = start;
        g_heap_blocks[i].size = size;
        g_heap_blocks[i].free = 0; g_heap_blocks[i].named = 0;
        if (back && !heap_insert_free(i + 1, start + size, back))
            g_heap_blocks[i].size += back;   /* table full: keep it attached */
        memset((void *)((uintptr_t)start + g_memory_offset), 0, size);
        return start;
    }
    return 0;
}

/*
 * Simulated stacks for spawned threads.
 *
 * The main thread owns the top of the XBOX_STACK region and grows down; worker
 * stacks are carved from the bottom upward so the two cannot meet until the
 * whole 8 MB is gone. Xbox VAs, not host memory: recompiled code addresses its
 * stack through MEM32() like any other Xbox pointer.
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/* How many worker stacks may be alive at once: as many as the guest heap
 * holds (XBA-104). Each is a 512 KB slice of it, so the real cap is
 * (XBOX_HEAP_TOP - XBOX_HEAP_BASE) less what the title has allocated: about
 * 100 stacks in a retail 64 MB window if the title took nothing, far more in a
 * larger mapped window.
 *
 * The pool used to stop at 8, and an exhausted pool was a deadlock, not an
 * error: PsCreateSystemThreadEx ran the worker inline on its creator, and a
 * worker that loops waiting for requests never returned to the creator that
 * would have sent them (Fable's 12th loader worker). Running out of guest heap
 * now fails the call with STATUS_INSUFFICIENT_RESOURCES, as the console does
 * when it cannot allocate a kernel stack.
 *
 * RECOMP_THREAD_STACKS=N still caps the count, for A/B against the old
 * limit (8); unset or 0 means no cap. */
static int max_thread_stacks(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("RECOMP_THREAD_STACKS");
        int v = e ? atoi(e) : 0;
        n = v > 0 ? v : 0;
    }
    return n;
}

/* Alive now and the most ever alive, under g_thread_stacks_lock: threads are
 * created and exit on different host threads. */
static int g_thread_stacks_used = 0;
static int g_thread_stacks_high = 0;
static SRWLOCK g_thread_stacks_lock = SRWLOCK_INIT;

int xbox_ThreadStacksHighWater(void)
{
    int h;
    AcquireSRWLockShared(&g_thread_stacks_lock);
    h = g_thread_stacks_high;
    ReleaseSRWLockShared(&g_thread_stacks_lock);
    return h;
}

/* A TIB and TLS block for a newly spawned guest thread.
 *
 * A TIB is per-thread on the console and was per-process here: one address,
 * 0x1000, for everyone. Two things live in it that must not be shared. fs:[0]
 * is the SEH chain head, so two threads unwinding at once walk each other's
 * frames. fs:[4] points at the image's TLS block, whose slot 0 is the CRT's
 * per-thread data -- errno, the locale, and the bookkeeping _lock() uses to
 * decide who owns which lock.
 *
 * Half-Life 2 deadlocked on the last of those: two threads inside _lock(),
 * each holding the CRT lock the other was waiting for, because "which thread
 * am I" was a single shared answer.
 *
 * The new block is a copy of the template the loader built, so a thread starts
 * with the image's initialised thread-local data rather than zeros, and its
 * own per-thread structure behind slot 0.
 */
uint32_t xbox_AllocThreadTib(void)
{
    /* XBOX_VA is scoped to the loader; the same arithmetic, spelled here. */
    #define TIB_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
    const uint32_t tib_size = 0x40;
    uint32_t tib, block, thread_data, total;

    if (!g_tls_total)
        return 0;                    /* image has no TLS; nothing to copy */

    total = g_tls_total;
    tib = xbox_HeapAlloc(tib_size + total + g_tls_thread_size, 16);
    if (!tib)
        return 0;
    block       = tib + tib_size;
    thread_data = block + total;

    /* The TIB itself, copied so stack bounds and the fields the title filled
     * in are inherited, then the two that must not be. */
    memcpy(TIB_VA(tib), TIB_VA(XBOX_TIB_MAIN), tib_size);
    memcpy(TIB_VA(block), TIB_VA(g_tls_template_va), total);
    memset(TIB_VA(thread_data), 0, g_tls_thread_size);

    *(uint32_t *)TIB_VA(tib + 0x00) = 0xFFFFFFFFu;   /* own SEH chain    */
    /* KPCR.Irql, fs:[0x24]: a new thread starts at PASSIVE_LEVEL, whatever
     * the first thread is at while this copy is taken. The byte is where
     * guest code reads the level (kernel_hal.c, irql_publish) and it is
     * written only when this thread's own level changes. Copied from a main
     * thread that was at DISPATCH_LEVEL (inside a DSOUND or USB call, a DPC
     * drained on it) or at a device level (a posted ISR), it read 2 or 16 on
     * the new thread until that thread made a kernel raise of its own -- and
     * DirectSound's lock is what would have made it: it reads the byte and,
     * at DISPATCH_LEVEL and above, takes neither its critical section nor a
     * raise. KOTOR's stream thread (0x00155C50) then ran DirectSoundDoWork
     * with no lock at all, beside the main thread, the DPCs and the APU
     * interrupt; a callback node of its timer queue was linked twice and the
     * main thread's DoWork (0x003AC0B8) looped on it for ever. */
    *(uint8_t  *)TIB_VA(tib + 0x24) = 0;
    *(uint32_t *)TIB_VA(block)      = thread_data;   /* slot 0           */
    *(uint32_t *)TIB_VA(tib + 0x04) = block + total; /* fs:[4], see above*/

    return tib;
    #undef TIB_VA
}

uint32_t xbox_AllocThreadStack(void)
{
    uint32_t base;
    int cap = max_thread_stacks(), used, high = 0;

    AcquireSRWLockExclusive(&g_thread_stacks_lock);
    if (cap && g_thread_stacks_used >= cap) {
        ReleaseSRWLockExclusive(&g_thread_stacks_lock);
        fprintf(stderr, "  [MEM] guest thread stacks: %d alive, the"
                        " RECOMP_THREAD_STACKS cap\n", cap);
        return 0;
    }
    g_thread_stacks_used++;          /* reserved; undone below on failure */
    ReleaseSRWLockExclusive(&g_thread_stacks_lock);

    /* From the heap, not from XBOX_STACK_BASE.
     *
     * The stack region begins at 0x00780000, which is fine only while the
     * title's image ends below that. Half-Life 2's image runs to 0x009B68C0,
     * so the first thread stack (0x00780000..0x00800000) landed inside its
     * .rdata and .data: the worker spawned during engine init wrote its
     * frames over the game's own static data. Nothing faults -- the pages are
     * mapped and writable -- so it shows up later as globals that were
     * correct when written and wrong when read.
     *
     * The heap already starts above the image and knows how big it is, so
     * taking slices from it is correct for any image size instead of only
     * for small ones.
     */
    base = xbox_HeapAlloc(XBOX_THREAD_STACK_SIZE, 4096);
    AcquireSRWLockExclusive(&g_thread_stacks_lock);
    if (!base) {
        used = --g_thread_stacks_used;
    } else {
        used = g_thread_stacks_used;
        if (used > g_thread_stacks_high)
            high = g_thread_stacks_high = used;
    }
    ReleaseSRWLockExclusive(&g_thread_stacks_lock);
    if (!base) {
        fprintf(stderr, "  [MEM] guest thread stacks: guest heap exhausted with"
                        " %d alive (%u KB each)\n", used,
                XBOX_THREAD_STACK_SIZE / 1024);
        fflush(stderr);
        return 0;
    }
    /* The high-water mark at 8 (the old fixed pool) and each doubling. */
    if (high >= 8 && !(high & (high - 1))) {
        fprintf(stderr, "  [MEM] guest thread stacks: high-water mark %d alive"
                        " (%u KB of guest heap)\n", high,
                (unsigned)high * (XBOX_THREAD_STACK_SIZE / 1024));
        fflush(stderr);
    }

    /* Top of the slice, 16-byte aligned, growing down. */
    return base + XBOX_THREAD_STACK_SIZE - 16;
}

/* Give a worker's stack back when the worker ends.
 *
 * The counter used to only ever go up, so a title that creates and destroys
 * threads ran the pool dry no matter how few were alive at once. The Xbox
 * Dashboard spawns one worker per ambient WAV and terminates it before loading
 * the next; after 8 files (the old fixed pool) the pool was empty and
 * PsCreateSystemThreadEx fell back to running the worker inline. That fallback
 * is a deadlock here rather than a slowdown: the worker ran to completion
 * before the caller reached its wait, so the main thread then waited forever on
 * events whose only signaller had already finished. It looked like an audio
 * hang, three layers away from the cause.
 *
 * Takes the value AllocThreadStack returned, so callers never do the arithmetic.
 */
void xbox_FreeThreadStack(uint32_t stack_top)
{
    if (!stack_top)
        return;
    xbox_HeapFree(stack_top + 16 - XBOX_THREAD_STACK_SIZE);
    AcquireSRWLockExclusive(&g_thread_stacks_lock);
    if (g_thread_stacks_used > 0)
        g_thread_stacks_used--;
    ReleaseSRWLockExclusive(&g_thread_stacks_lock);
}

/* Page allocator over the contiguous window mapped at XBOX_CONTIG_BASE.
 *
 * MmAllocateContiguousMemory hands back physical memory, and on Xbox physical
 * page P is visible at 0x80000000 + P. Drivers rely on that being an exact
 * round trip: Xbox D3D writes its pushbuffer position to the NV2A as
 * `VA & 0x0FFFFFFF` and reads the GPU's position back as `GET | 0x80000000`,
 * then compares the two. That holds for any address in this window and for
 * nothing in the general heap, whose position depends on what the title
 * reserved first -- Half-Life 2 reserves 128 MB and then 200 MB before D3D
 * allocates its pushbuffer, which put the buffer at 0x15782000 and left the
 * engine comparing 0x857844C0 against it forever.
 *
 * A first-fit page allocator: one byte of state per 4 KB page, plus the
 * length of each allocation at its first page. 16,384 pages, so a linear scan
 * is cheap next to the memset every allocation already does. Honours the
 * alignment and the physical [lowest, highest] range of
 * MmAllocateContiguousMemoryEx, which a bump pointer cannot: a request limited
 * to the low 16 MB fails once the pointer has passed it, however much below
 * it is free. XBOX_GPU_INSTANCE_DEFAULT stays carved off the top for the
 * GPU-instance bridge.
 *
 * MmFreeContiguousMemory gives a block back (RECOMP_CONTIG_RECLAIM, on by
 * default; see contig_reclaim()). Contiguous blocks are every texture and
 * vertex buffer a D3D title creates, and those come and go with the scene; a
 * title that frees and reallocates them as each texture pack loads ran the
 * 64 MB window out when frees were ignored.
 *
 * Locked: textures are created from loader threads as well as the main one. */
#define CONTIG_PAGE       4096u
#define CONTIG_PAGES      (XBOX_CONTIG_SIZE / CONTIG_PAGE)
#define CONTIG_USABLE     ((XBOX_CONTIG_SIZE - XBOX_GPU_INSTANCE_DEFAULT) / CONTIG_PAGE)

/* The last megabyte below 16 MB is kept for requests that must sit there.
 *
 * Some buffers have to be below 16 MB because the GPU addresses them with a
 * 24-bit offset. XDK D3D's EndVisibilityTest allocates its report pages with
 * MmAllocateContiguousMemoryEx(0x1000, 0, 0xFFFFFF, ...), the first time each
 * page of 256 tests is used. The console kernel fills contiguous memory from
 * the top, so low memory is still free when that call comes. This arena fills
 * it from the bottom, so a few minutes into one title the low 16 MB was full (26 MB
 * in use, all of it starting at 0). Every report-page call failed, 700 to 1,900
 * times a run, EndVisibilityTest returned E_OUTOFMEMORY, and
 * GetVisibilityTestResult read its results through a NULL page.
 *
 * Allocating from the top would match the console, but it would move every
 * surface and buffer the logs and tools name by address (a title's back
 * buffers, for one). Keeping one megabyte back
 * changes nothing that is placed below 15 MB. A request whose range ends
 * below 16 MB can use the reserve; any other request skips it. */
#define CONTIG_LOW_RESERVE_START ((15u << 20) / CONTIG_PAGE)
#define CONTIG_LOW_RESERVE_END   ((16u << 20) / CONTIG_PAGE)

/* Physical page 0 is never handed out. The real kernel keeps it, and the
 * XDK's USB stack relies on that: XPP carves its host controller structures
 * from a private 0xFE0-byte arena ending at 0x80001000, with no allocation
 * call at all. Handing that page to the
 * title's first MmAllocateContiguousMemory -- XAPI's launch data page --
 * let whichever wrote last win, and the pad enumerated or not depending on
 * timing. Page 0 starts marked CONTIG_RESERVED: no allocation, claim or
 * range request (lowest=0 included) gets it, and the page map still reports
 * it as window memory, which is what a device resolving XPP's physical
 * addresses needs. */
#define CONTIG_RESERVED   2u

static uint8_t  g_contig_used[CONTIG_PAGES] = { CONTIG_RESERVED };
                                               /* 1 = page belongs to a block */
static uint32_t g_contig_len[CONTIG_PAGES];    /* pages, at a block's first page */
static uint32_t g_contig_pages_in_use;
static uint32_t g_contig_high = CONTIG_PAGE;   /* high-water mark, bytes */
static uint32_t g_contig_first_free = 1;       /* no free page below this */
static uint32_t g_contig_allocs;
static uint32_t g_contig_released;             /* blocks given back */
static uint32_t g_contig_free_calls;           /* frees asked for, kept or not */
static uint32_t g_contig_peak_pages;
static DWORD    g_contig_t0;
static volatile LONG g_contig_lock;

static void contig_lock(void)
{
    while (InterlockedCompareExchange(&g_contig_lock, 1, 0) != 0)
        SwitchToThread();
}

static void contig_unlock(void)
{
    InterlockedExchange(&g_contig_lock, 0);
}

/* Usage over time, at most a line per 2 MB of movement or per 1024 calls --
 * enough to see the arena breathe without a line per texture. Lock held. */
static void contig_report(const char *what, uint32_t va, uint32_t pages)
{
    static uint32_t last_pages, ops;
    uint32_t delta = g_contig_pages_in_use > last_pages
                   ? g_contig_pages_in_use - last_pages
                   : last_pages - g_contig_pages_in_use;

    if (!g_contig_t0)
        g_contig_t0 = GetTickCount();
    if (++ops > 16 && delta < 512 && (ops & 1023) != 0)
        return;
    last_pages = g_contig_pages_in_use;
    fprintf(stderr, "  [CONTIG] t=%.1fs %s 0x%08X %u KB: in use %u KB, "
            "peak %u KB, high-water %u KB (allocs=%u released=%u "
            "free calls=%u)\n",
            (GetTickCount() - g_contig_t0) / 1000.0, what, va,
            pages * (CONTIG_PAGE / 1024),
            g_contig_pages_in_use * (CONTIG_PAGE / 1024),
            g_contig_peak_pages * (CONTIG_PAGE / 1024),
            g_contig_high / 1024, g_contig_allocs, g_contig_released,
            g_contig_free_calls);
    fflush(stderr);
}

static void contig_take(uint32_t first, uint32_t n)
{
    uint32_t end = (first + n) * CONTIG_PAGE;

    memset(&g_contig_used[first], 1, n);
    g_contig_len[first] = n;
    g_contig_pages_in_use += n;
    if (g_contig_pages_in_use > g_contig_peak_pages)
        g_contig_peak_pages = g_contig_pages_in_use;
    if (end > g_contig_high)
        g_contig_high = end;
    while (g_contig_first_free < CONTIG_PAGES
           && g_contig_used[g_contig_first_free])
        g_contig_first_free++;
}

/* Whether page q may start or extend a new block.
 *
 * The first pass also keeps out of the loaded image's physical range. Here
 * the window and the RAM the image sits in are separate storage, so a block
 * placed at an image page gives one physical number two meanings, and a
 * device model holding only the number has to guess. Darkwatch's XPP lost
 * that guess both ways: a 1200 KB block at 0x0045C000 covered its .data
 * control-transfer buffer (0x004C20D4), and its OHCI descriptors, allocated
 * inside the block, resolved to the image and read back zero, so the pad
 * never enumerated. The console never has the overlap: its kernel keeps the
 * image's pages out of the pool. The second pass allows them, because an
 * image whose BSS runs past 48 MB would otherwise leave almost no window.
 *
 * Opt-in (RECOMP_CONTIG_AVOID_IMAGE=1): it moves every block above the
 * image, and game glue names some of them by address (a DSP doorbell, say). */
static int contig_avoid_image(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CONTIG_AVOID_IMAGE");
        on = e && atoi(e) != 0;
    }
    return on;
}

static int contig_page_free(uint32_t q, int avoid_image)
{
    if (g_contig_used[q])
        return 0;
    return !(avoid_image && g_xbox_image_hi
             && q >= g_xbox_image_lo / CONTIG_PAGE
             && q < (g_xbox_image_hi + CONTIG_PAGE - 1) / CONTIG_PAGE);
}

uint32_t xbox_ContiguousAllocEx(uint32_t size, uint32_t alignment,
                                uint32_t phys_lo, uint32_t phys_hi)
{
    uint32_t n, align_pages, lo_page, limit, p, q, pass, result = 0;

    if (alignment < CONTIG_PAGE) alignment = CONTIG_PAGE;
    if (alignment & (alignment - 1)) {            /* round up to a power of 2 */
        uint32_t a = CONTIG_PAGE;
        while (a && a < alignment) a <<= 1;
        alignment = a ? a : 0x80000000u;
    }
    if (size == 0) size = 1;
    n = (uint32_t)(((uint64_t)size + CONTIG_PAGE - 1) / CONTIG_PAGE);
    align_pages = alignment / CONTIG_PAGE;

    /* Physical range -> page range. Highest is inclusive. */
    lo_page = (uint32_t)(((uint64_t)phys_lo + CONTIG_PAGE - 1) / CONTIG_PAGE);
    limit = (uint32_t)(((uint64_t)phys_hi + 1) / CONTIG_PAGE);
    if (limit > CONTIG_USABLE) limit = CONTIG_USABLE;

    contig_lock();
    if (lo_page < g_contig_first_free) lo_page = g_contig_first_free;
    for (pass = contig_avoid_image() ? 0 : 1; pass < 2 && !result; pass++) {
        int avoid = pass == 0;
        p = (lo_page + align_pages - 1) & ~(align_pages - 1);
        while ((uint64_t)p + n <= limit) {
            /* A request free to go above 16 MB steps over the low reserve. */
            if (limit > CONTIG_LOW_RESERVE_END && p < CONTIG_LOW_RESERVE_END
                    && p + n > CONTIG_LOW_RESERVE_START) {
                p = (CONTIG_LOW_RESERVE_END + align_pages - 1) & ~(align_pages - 1);
                continue;
            }
            for (q = p; q < p + n && contig_page_free(q, avoid); q++)
                ;
            if (q == p + n) {
                contig_take(p, n);
                result = XBOX_CONTIG_BASE + p * CONTIG_PAGE;
                break;
            }
            /* Skip the run that stopped us, then realign. */
            while (q < limit && !contig_page_free(q, avoid)) q++;
            p = (q + align_pages - 1) & ~(align_pages - 1);
        }
    }
    if (result) {
        g_contig_allocs++;
        contig_report("alloc", result, n);
    } else {
        uint32_t best = 0, run = 0;
        for (q = 0; q < CONTIG_USABLE; q++) {
            run = g_contig_used[q] ? 0 : run + 1;
            if (run > best) best = run;
        }
        fprintf(stderr, "  [CONTIG] arena exhausted (%u requested, align %u, "
                "phys 0x%08X..0x%08X; %u of %u KB in use, largest free run "
                "%u KB)\n", size, alignment, phys_lo, phys_hi,
                g_contig_pages_in_use * (CONTIG_PAGE / 1024),
                (unsigned)(CONTIG_USABLE * (CONTIG_PAGE / 1024)),
                best * (CONTIG_PAGE / 1024));
        fflush(stderr);
    }
    contig_unlock();

    if (result)
        memset((void *)((uintptr_t)result + g_memory_offset), 0,
               (size_t)n * CONTIG_PAGE);
    return result;
}

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    return xbox_ContiguousAllocEx(size, alignment, 0, 0xFFFFFFFFu);
}

/* A block the caller placed itself (MmAllocateContiguousMemoryEx with a range
 * exactly one allocation wide). Recorded so nothing else is handed the same
 * pages and so the matching free releases them. Pages already in use are
 * left to their owner, since the caller has been given them regardless; each
 * run of pages still free is claimed as its own block, so a partly overlapped
 * placement still keeps the rest from being handed out again. */
void xbox_ContiguousClaim(uint32_t va, uint32_t size)
{
    uint32_t first, n, q, run;

    if (va < XBOX_CONTIG_BASE || size == 0)
        return;
    first = (va - XBOX_CONTIG_BASE) / CONTIG_PAGE;
    n = (uint32_t)(((uint64_t)(va & (CONTIG_PAGE - 1)) + size + CONTIG_PAGE - 1)
                   / CONTIG_PAGE);
    if ((uint64_t)first + n > CONTIG_PAGES)
        return;
    contig_lock();
    for (q = first; q < first + n; q += run) {
        if (g_contig_used[q]) { run = 1; continue; }
        for (run = 0; q + run < first + n && !g_contig_used[q + run]; run++)
            ;
        contig_take(q, run);
        g_contig_allocs++;
        contig_report("claim", XBOX_CONTIG_BASE + q * CONTIG_PAGE, run);
    }
    contig_unlock();
}

/* RECOMP_CONTIG_RECLAIM: MmFreeContiguousMemory gives the block back. On
 * unless set to 0, which keeps every block for good, as the arena did before
 * (then only under RECOMP_HEAP_RECLAIM, which now governs the general heap and
 * NtFreeVirtualMemory alone). Off, the window fills in address order and a
 * block's address is never seen twice; on, a freed block's pages go to the
 * next request that fits. Fable frees about 16 MB per load transition and ran
 * the 64 MB window out without it. Read once. */
static int contig_reclaim(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CONTIG_RECLAIM");
        on = !e || !*e || atoi(e) != 0;
    }
    return on;
}

/* Release a block (unless RECOMP_CONTIG_RECLAIM=0). Returns 1 if va is in the
 * window, 0 if this arena never handed it out -- the caller decides whether
 * that is someone else's. */
int xbox_ContiguousFree(uint32_t va)
{
    static int warned;
    uint32_t first, n;

    if (va < XBOX_CONTIG_BASE
            || (uint64_t)va >= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return 0;
    first = (va - XBOX_CONTIG_BASE) / CONTIG_PAGE;

    contig_lock();
    g_contig_free_calls++;
    /* Ours, but kept: 1 stops the caller handing it to the general heap. */
    if (!contig_reclaim()) {
        contig_unlock();
        return 1;
    }
    n = (va & (CONTIG_PAGE - 1)) ? 0 : g_contig_len[first];
    if (!n) {
        contig_unlock();
        if (warned++ < 8) {
            fprintf(stderr, "  [CONTIG] free of 0x%08X: not the start of a "
                    "block, ignored\n", va);
            fflush(stderr);
        }
        return 0;
    }
    memset(&g_contig_used[first], 0, n);
    g_contig_len[first] = 0;
    g_contig_pages_in_use -= n;
    if (first < g_contig_first_free)
        g_contig_first_free = first;
    g_contig_released++;
    contig_report("free", va, n);
    contig_unlock();
    return 1;
}

/* Bytes from va to the end of the block containing it, or 0 if va is not in
 * an allocated block -- the same convention as xbox_HeapBlockSize, and for
 * the same caller, MmQueryAllocationSize. */
uint32_t xbox_ContiguousBlockSize(uint32_t va)
{
    uint32_t page, first, result = 0;

    if (va < XBOX_CONTIG_BASE
            || (uint64_t)va >= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return 0;
    page = (va - XBOX_CONTIG_BASE) / CONTIG_PAGE;
    contig_lock();
    if (g_contig_used[page] == 1) {             /* not the reserved page 0 */
        first = page;
        while (first > 0 && !g_contig_len[first])
            first--;
        result = XBOX_CONTIG_BASE + (first + g_contig_len[first]) * CONTIG_PAGE
               - va;
    }
    contig_unlock();
    return result;
}

/* High-water mark of the window: everything below it was, at some point,
 * memory a MmAllocateContiguousMemory call returned.
 *
 * Lets a caller holding a physical address decide whether it names contiguous
 * memory this runtime allocated. The pushbuffer executor needs exactly that:
 * a surface offset is physical, and only the window makes it addressable.
 * Never decreases, so a freed-and-reused texture stays addressable. */
uint32_t xbox_ContiguousAllocatedBytes(void)
{
    return g_contig_high;
}

/* The in-use map, one byte per 4 KB page of the window: for a device model
 * (the APU) deciding whether a physical address names the window or RAM. */
const volatile uint8_t *xbox_ContiguousPageMap(void)
{
    return g_contig_used;
}

/* Bytes in use right now. */
uint32_t xbox_ContiguousInUseBytes(void)
{
    return g_contig_pages_in_use * CONTIG_PAGE;
}

/* Physical pages below 64 MB that MmGetPhysicalAddress has handed out for a
 * heap address, whose physical number is its virtual address.
 *
 * On the console a physical page is one page of RAM. Here the window and the
 * RAM below 64 MB are separate storage, and the arena above hands out
 * physical numbers that the heap and the image also use. So one number can
 * name two different pages, and a device given only the number cannot tell
 * which the title meant. A title that loads its sound effects and music
 * stream into the heap has DirectSound put each page's MmGetPhysicalAddress
 * into the voice's scatter-gather list; when those numbers fall inside a
 * block the arena has handed out, the APU read the window copy and played
 * whatever the window held there as PCM.
 *
 * MmGetPhysicalAddress is the one moment the runtime sees which copy the title
 * means, so it records RAM names here. Heap pages only: the image, its BSS and
 * the stacks are never freed, so a record there would never clear, and a
 * title's DirectSound voice array can sit at a window address whose number
 * falls inside a large image (see mcpx_apu_phys). The record is kept until the
 * heap frees the page. A window name does not clear it, because D3D names its
 * window pages all the time and the effect buffer in RAM is still live. If one
 * page holds live sound data in both copies, the APU picks RAM; this record
 * cannot tell them apart.
 *
 * Plain byte stores and loads. The APU thread may see a mark a voice-processor
 * pass late (or a cleared one a pass early); the title fills a buffer after
 * naming it and frees it after stopping the voice, so a stale read costs at
 * most one pass of one voice. */
static volatile uint8_t g_phys_ram_named[CONTIG_PAGES];

/* The heap block holding `va`, live or free, or -1 when none does. Index
 * order is address order (empty slots keep the address they had), so the
 * search finds the last slot starting at or below `va` and steps back over
 * empty ones. Caller holds g_heap_lock (shared is enough). */
static int heap_block_at(uint32_t va)
{
    int lo = 0, hi = g_heap_block_count - 1, at = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_heap_blocks[mid].addr <= va) {
            at = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    for (; at >= 0; at--) {
        if (!g_heap_blocks[at].size)
            continue;
        return (uint64_t)va < (uint64_t)g_heap_blocks[at].addr + g_heap_blocks[at].size ? at : -1;
    }
    return -1;
}

void xbox_PhysNoteRamName(uint32_t va)
{
    if (va >= XBOX_HEAP_BASE && va < XBOX_HEAP_TOP && va < XBOX_CONTIG_SIZE) {
        int i;
        g_phys_ram_named[va / CONTIG_PAGE] = 1;
        /* The block too: a page can hold the end of a window buffer and the
         * start of a heap block the title named, and only the block says
         * which bytes the name covers (xbox_PhysRamNamedAt). A byte store
         * under the shared lock: nothing moves the table meanwhile. */
        AcquireSRWLockShared(&g_heap_lock);
        i = heap_block_at(va);
        if (i >= 0 && !g_heap_blocks[i].free)
            g_heap_blocks[i].named = 1;
        ReleaseSRWLockShared(&g_heap_lock);
    }
}

/* Whether physical byte `pa` is RAM the title named, at byte precision.
 *
 * The page map says a heap page was named; a page is 4 KB and is shared.
 * Def Jam: Fight for NY's six-channel mix buffer is a window allocation
 * whose last 344 bytes share their physical page with a heap block that
 * starts after them and that the title named (MmGetPhysicalAddress), so the
 * page map sent those bytes to RAM: the last 28 frames of every lap of the
 * buffer played whatever RAM held there, a click 20 times a second in the
 * opening scene. Here a named page only counts for the bytes inside a live
 * block that was named. Bytes the heap table does not place in a block keep
 * the page's answer, as before. */
int xbox_PhysRamNamedAt(uint32_t pa)
{
    int i, named;
    if (pa >= XBOX_CONTIG_SIZE || !g_phys_ram_named[pa / CONTIG_PAGE])
        return 0;
    AcquireSRWLockShared(&g_heap_lock);
    i = heap_block_at(pa);
    named = i < 0 || (!g_heap_blocks[i].free && g_heap_blocks[i].named);
    ReleaseSRWLockShared(&g_heap_lock);
    return named;
}

/* Heap block m is free (and merged with any free neighbours): its pages name
 * nothing now, except an edge page it shares with a live block. Index order is
 * address order, so the blocks that can share an edge page are the nearest
 * ones on either side; empty slots (size 0) and free blocks are stepped over.
 * Caller holds g_heap_lock. */
static void heap_forget_ram_names(int m)
{
    uint32_t lo = g_heap_blocks[m].addr;
    int64_t first, last;

    if (!g_heap_blocks[m].size)
        return;
    first = lo / CONTIG_PAGE;
    last = ((uint64_t)lo + g_heap_blocks[m].size - 1) / CONTIG_PAGE;
    for (int j = m - 1; j >= 0; j--) {
        if (!g_heap_blocks[j].size)
            continue;
        if ((uint64_t)g_heap_blocks[j].addr + g_heap_blocks[j].size <=
                (uint64_t)first * CONTIG_PAGE)
            break;
        if (!g_heap_blocks[j].free) {
            first++;
            break;
        }
    }
    for (int j = m + 1; j < g_heap_block_count; j++) {
        if (!g_heap_blocks[j].size)
            continue;
        if ((uint64_t)g_heap_blocks[j].addr >= (uint64_t)(last + 1) * CONTIG_PAGE)
            break;
        if (!g_heap_blocks[j].free) {
            last--;
            break;
        }
    }
    if (last >= (int64_t)CONTIG_PAGES)
        last = (int64_t)CONTIG_PAGES - 1;
    for (int64_t p = first; p <= last; p++)
        g_phys_ram_named[p] = 0;
}

const volatile uint8_t *xbox_PhysRamNameMap(void)
{
    return g_phys_ram_named;
}


static uint32_t heap_alloc_locked(uint32_t size, uint32_t alignment);

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t r;

    AcquireSRWLockExclusive(&g_heap_lock);
    r = heap_alloc_locked(size, alignment);
    ReleaseSRWLockExclusive(&g_heap_lock);
    return r;
}

static uint32_t heap_alloc_locked(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    if (alignment < 4) alignment = 4;

    /* Enforce minimum allocation size.
     * The Xbox D3D8 code sometimes computes resource sizes from GPU
     * capabilities that return 0 (since we don't have real NV2A hardware),
     * resulting in zero-size allocations. With a bump allocator, these all
     * return the same address, causing overlapping structures. Enforce a
     * minimum of 4096 bytes so each allocation gets its own memory. */
    if (size < 16) size = 16;

    /* Reuse a freed block first. Without this the heap only ever grows: Halo's
     * debug build allocates and releases heavily through init, exhausted all
     * 48 MB in 4,726 allocations, and its second D3D CreateDevice then failed
     * with E_OUTOFMEMORY -- which the title reports by clearing
     * global_d3d_device, so the rasterizer asserts and startup stops. */
    if (xbox_HeapReclaimEnabled()) {
        /* Take only what the request needs; see xbox_HeapReclaimEnabled(). */
        size = (size + 15) & ~15u;
        result = heap_carve_free(size, alignment);
        if (result)
            return result;
    } else {
        for (int i = 0; i < g_heap_block_count; i++) {
            if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size) {
                continue;
            }
            if (g_heap_blocks[i].addr & (alignment - 1)) {
                continue;   /* wrong alignment for this request */
            }
            g_heap_blocks[i].free = 0; g_heap_blocks[i].named = 0;
            result = g_heap_blocks[i].addr;
            memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
            return result;
        }
    }

    /* Align the next pointer */
    if (!g_heap_next) xbox_heap_reset_base();
    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_TOP) {
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, used %u/%u)\n",
                size, g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        /* Who ate the heap? Group live blocks by size -- an exhausted heap is
         * nearly always one request size repeated, and the count names it. */
        {
            static int dumped = 0;
            static struct { uint32_t size; int n; } hist[256];
            if (!dumped) {
                int used = 0;
                dumped = 1;
                for (int i = 0; i < g_heap_block_count; i++) {
                    int j = 0;
                    if (g_heap_blocks[i].free || !g_heap_blocks[i].size) continue;
                    while (j < used && hist[j].size != g_heap_blocks[i].size) j++;
                    if (j == used) {
                        if (used == 256) continue;   /* ponytail: 256 distinct sizes is plenty */
                        hist[used].size = g_heap_blocks[i].size;
                        hist[used++].n = 0;
                    }
                    hist[j].n++;
                }
                for (int j = 0; j < used; j++) {
                    if ((uint64_t)hist[j].n * hist[j].size < 1024 * 1024) continue;
                    fprintf(stderr, "  [HEAP] %d live blocks of %u bytes (%u KB)\n",
                            hist[j].n, hist[j].size,
                            (unsigned)((uint64_t)hist[j].n * hist[j].size / 1024));
                }
                fflush(stderr);
            }
        }
        return 0;
    }

    g_heap_next = result + size;

    /* Zero-fill the allocated block (Xbox memory is always zeroed) */
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    if (g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
        g_heap_blocks[g_heap_block_count].addr = result;
        g_heap_blocks[g_heap_block_count].size = size;
        g_heap_blocks[g_heap_block_count].free = 0; g_heap_blocks[g_heap_block_count].named = 0;
        g_heap_block_count++;
    }

    g_heap_alloc_count++;
    /* Rate-limited: a debug title makes thousands of these and the log is a
     * diagnostic, not a transaction record. */
    if (g_heap_alloc_count <= 32 || (g_heap_alloc_count % 512) == 0) {
        fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u)\n",
                g_heap_alloc_count, size, alignment, result, result + size,
                g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        fflush(stderr);
    }

    return result;
}

/* How big is the block at this guest address?
 *
 * MmQueryAllocationSize and ExQueryPoolBlockSize both ask this, and both used
 * to answer 0 -- ExQueryPoolBlockSize by returning a literal, and
 * MmQueryAllocationSize by having no bridge at all. The host cannot answer it:
 * VirtualQuery on the translated address reports the size of the whole 64 MB
 * guest mapping, which is a worse answer than none. The block table already
 * has the real one, and it is the same table xbox_HeapFree matches against.
 *
 * Interior addresses count: a title that asks about a pointer it has walked
 * forward is asking about the block that contains it. Returns 0 for an address
 * this heap never handed out, which is what "not one of mine" has to look like.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va)
{
    int i;
    uint32_t r = 0;

    if (!xbox_va)
        return 0;
    AcquireSRWLockShared(&g_heap_lock);
    for (i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].free)
            continue;
        if (xbox_va >= g_heap_blocks[i].addr &&
            xbox_va <  g_heap_blocks[i].addr + g_heap_blocks[i].size) {
            r = g_heap_blocks[i].size - (xbox_va - g_heap_blocks[i].addr);
            break;
        }
    }
    ReleaseSRWLockShared(&g_heap_lock);
    return r;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    static int frees = 0, matched = 0;

    if (!xbox_va) {
        return;
    }
    AcquireSRWLockExclusive(&g_heap_lock);
    frees++;
    if (frees <= 8) {
        fprintf(stderr, "  [HEAP] free #%d va=0x%08X blocks=%d\n",
                frees, xbox_va, g_heap_block_count);
        fflush(stderr);
    }
    for (int i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].addr != xbox_va || g_heap_blocks[i].free) {
            continue;
        }
        g_heap_blocks[i].free = 1; g_heap_blocks[i].named = 0;
        if (++matched % 512 == 0) {
            fprintf(stderr, "  [HEAP] frees=%d matched=%d blocks=%d\n",
                    frees, matched, g_heap_block_count);
            fflush(stderr);
        }

        /* Coalesce with neighbours. Blocks are recorded in bump order, so
         * index order is address order and adjacency is a simple end==start
         * test. Keeps large contiguous requests satisfiable after a lot of
         * small churn. Under RECOMP_HEAP_RECLAIM the neighbour search steps
         * over the empty slots (size 0) earlier merges leave behind; without
         * that, one merge stops every later one dead. */
        {
            int n = i + 1, p = i - 1, m = i;
            if (xbox_HeapReclaimEnabled()) {
                while (n < g_heap_block_count && g_heap_blocks[n].size == 0) n++;
                while (p >= 0 && g_heap_blocks[p].size == 0) p--;
            }
            if (n < g_heap_block_count && g_heap_blocks[n].free &&
                g_heap_blocks[i].addr + g_heap_blocks[i].size == g_heap_blocks[n].addr) {
                g_heap_blocks[i].size += g_heap_blocks[n].size;
                g_heap_blocks[n].size = 0;
                g_heap_blocks[n].addr = 0;
            }
            if (p >= 0 && g_heap_blocks[p].free &&
                g_heap_blocks[p].addr + g_heap_blocks[p].size == g_heap_blocks[i].addr) {
                g_heap_blocks[p].size += g_heap_blocks[i].size;
                g_heap_blocks[i].size = 0;
                g_heap_blocks[i].addr = 0;
                m = p;
            }
            /* After the merge, so a page this block shared with a neighbour
             * that is free too is forgotten. Under the lock, so a block
             * reused at once is named afresh after this, not wiped by it. */
            heap_forget_ram_names(m);
        }
        ReleaseSRWLockExclusive(&g_heap_lock);
        return;
    }
    ReleaseSRWLockExclusive(&g_heap_lock);
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}
