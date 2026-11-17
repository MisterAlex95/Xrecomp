/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA→native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "kernel.h"
#include "guest_vmem.h"
#include "kernel_missing.h"
#include "kernel_ksync.h"
#include "kernel_stubs.h"
#include "kernel_wait_stats.h"
#include "kernel_park.h"
#include "xbox_resume.h"
#include "xbox_memory_layout.h"
#include "xbox_seh.h"
#include "xbox_xbe_sections.h"
#include "platform/host_time.h"
#include "guest_clock.h"            /* fast-forward (XBA-240) */
#include "recomp_icall_feedback.h"
#include "recomp_discovery.h"
#include "recomp_status.h"
#include "kernel_disc.h"
#include "xbox_cheats.h"
#include "kernel_kobj_state.h"   /* save states (XBA-246) */
#include <stdio.h>
/* stdlib.h is load-bearing, not tidiness. Without it C89 implicit declaration
 * makes malloc return `int`, so bridge_spawn_thread truncated its heap pointer
 * to 32 bits and sign-extended it into a `struct bridge_thread_start *`. Every
 * subsequent s->field wrote to an address that had nothing to do with the
 * allocation. MSVC says so (C4013 + C4047 "differs in levels of indirection")
 * but only as warnings, and this file is compiled with /W4 /WX-. */
#include <stdlib.h>
#include <float.h>
/* Section B string helpers: wcslen helper, case folding.
 * ctype.h/wctype.h are not pulled in by the platform headers on either host. */
#include <string.h>
#include <ctype.h>
#include <wctype.h>

/* The recompiled code's registers (g_eax, g_esp, ...) are fields of the
 * per-thread g_cpu, declared in xbox_memory_layout.h. */
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
extern ptrdiff_t g_xbox_mem_offset;

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
/* Timer DPCs run on a host thread, which needs a guest stack under it. */
int  xbox_worker_stack_alloc(void);

recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* Translate Xbox VA to native pointer (NULL-safe: 0 → NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)

/* ── Guest buffers the host is about to touch ───────────
 *
 * A bridge turns a guest VA into a host pointer by adding an offset, so a VA
 * the guest got wrong does not fail the call -- it faults inside the kernel
 * implementation, on a host stack with no recompiled frame in it and a fault
 * address that means nothing on its own.
 *
 * The dangerous shape is an address AND a length that both come from the
 * guest. NtReadFile is the clearest case: the host WRITES `length` bytes
 * through the pointer, so a buffer near the top of the mapping, or a length
 * that does not match the buffer it names, walks the host past the end of
 * guest memory writing file contents into whatever follows. Nothing above this
 * layer can catch it, because xbox_NtReadFile receives a host pointer and a
 * count and cannot know where the mapping ends.
 */
static int bridge_va_mapped(uint32_t va, uint32_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    uint64_t mapped = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    if (va < 0x1000)            /* page zero is deliberately unmapped */
        return 0;
    if (end <= mapped)
        return 1;
    if (xbox_ReserveHolds(va, bytes))   /* a lazy reservation (XBA-103) */
        return 1;
    return va >= XBOX_CONTIG_BASE
        && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
}

/* STATUS_ACCESS_VIOLATION is what NT answers for a user buffer it cannot
 * touch, and it is far more useful to a title than a host crash: the call
 * fails, the guest gets a status it has a branch for, and the log names the
 * export, the buffer and the length. Warned once per export so a title that
 * does this in a loop does not bury the rest of the log. */
static int bridge_buf_ok(uint32_t va, uint32_t bytes, const char *export_name)
{
    static const char *seen[16];
    static int distinct;
    int i;

    if (!bytes)                                   /* nothing is accessed */
        return 1;
    if (va && bridge_va_mapped(va, bytes))
        return 1;

    for (i = 0; i < distinct; ++i)
        if (seen[i] == export_name)
            return 0;
    if (distinct < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[distinct++] = export_name;

    fprintf(stderr,
            "  [KERNEL] %s: buffer 0x%08X length %u is not mapped guest "
            "memory; returning STATUS_ACCESS_VIOLATION\n",
            export_name, va, (unsigned)bytes);
    fflush(stderr);
    return 0;
}

/* ── Synthetic VA range (for function exports) ─────────── */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

/* ── Kernel data exports ──────────────────────────────────
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs — they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* KdDebuggerEnabled / KdDebuggerNotPresent (88, 89) are BOOLEAN data
 * exports (nxdk xboxkrnl.h): a title reads *KdDebuggerNotPresent through its
 * thunk. They were routed as functions returning 0 and 1, so the read found
 * whatever byte the synthetic thunk address held (XBA-114). */
#define KDATA_KD_DEBUGGER_ENABLED     0x0F0  /* BOOLEAN, FALSE */
#define KDATA_KD_DEBUGGER_NOT_PRESENT 0x0F4  /* BOOLEAN, TRUE  */

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    /* Ordinals here are checked BEFORE function routing (see the thunk build
     * loop), so an ordinal listed by mistake turns a real kernel function into
     * a data address -- the title then calls it and jumps into kernel data.
     *
     * This table had a whole block shifted. 17 (ExFreePool), 65
     * (IoCreateDevice), 327 (XeLoadSection) and 328 (XeUnloadSection) are all
     * functions and were all being handed data addresses; Crimson Skies imports
     * every one of them. In the other direction, the genuine exports at 16, 353,
     * 354, 355, 356 and 357 got no thunk at all, so a title reading
     * XboxLANKey or KeTimeIncrement read whatever the function fallback left.
     *
     * test_bridge_ordinals.py now checks every entry below against the export
     * table, which is why the block cannot drift again unnoticed. */
    switch (ordinal) {
    case  16: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  22: return XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    case  30: return XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    case  31: return XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    case  40: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS;
    case  41: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_MODEL_STR;
    case  42: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_SERIAL_STR;
    case  64: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  70: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case  88: return XBOX_KERNEL_DATA_BASE + KDATA_KD_DEBUGGER_ENABLED;
    case  89: return XBOX_KERNEL_DATA_BASE + KDATA_KD_DEBUGGER_NOT_PRESENT;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 157: return XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 353: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 354: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO;
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
    default:  return 0;  /* Not a data export */
    }
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void kernel_data_init(void)
{
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156) - initialized to current tick count.
     * A background thread in main.c updates this every ~1ms. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = GetTickCount();

    /* No kernel debugger (ordinals 88, 89). */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_KD_DEBUGGER_ENABLED) = 0;
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_KD_DEBUGGER_NOT_PRESENT) = 1;

    /* LaunchDataPage (ordinal 164).
     *
     * This is how a title receives a command line. XGetLaunchInfo reads the
     * page, takes LaunchDataType from its first dword, and copies 3072 bytes
     * from +0x400; a type of 3 means those bytes *are* the command line.
     * Half-Life 2 does exactly that in sub_00596710, and falls back to the
     * empty string at 0x00772EA7 when the call fails -- which is what a NULL
     * page produces, so the engine started with no arguments and no map.
     *
     * On hardware the launcher fills this in before rebooting into the title.
     * RECOMP_CMDLINE does the same thing here, so a title can be told to load
     * a level the way the console would tell it: "+map intro".
     */
    /* A quick reboot into this title (kernel_reboot.c): the page the last
     * run's XLaunchNewImage filled, back where XGetLaunchInfo looks. It wins
     * over RECOMP_CMDLINE, which the restarted process still has in its
     * environment. Contiguous memory, as on the console, where XAPI keeps the
     * page with MmPersistContiguousMemory and may free or reuse it. */
    uint8_t reboot_page[XBOX_LAUNCH_PAGE_BYTES];
    uint32_t reboot_va = 0;

    if (xbox_launch_data_take(reboot_page) == 0) {
        reboot_va = xbox_ContiguousAlloc(XBOX_LAUNCH_PAGE_BYTES, 4096);
        if (!reboot_va)
            reboot_va = xbox_HeapAlloc(XBOX_LAUNCH_PAGE_BYTES, 4096);
        if (reboot_va) {
            char path[64];
            memcpy((void *)((uintptr_t)reboot_va + g_xbox_mem_offset),
                   reboot_page, XBOX_LAUNCH_PAGE_BYTES);
            memcpy(path, reboot_page + 8, sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
            fprintf(stderr, "  Launch data: quick reboot page at 0x%08X, type=%u"
                            " titleid=0x%08X path='%s'\n", reboot_va,
                    BRIDGE_MEM32(reboot_va), BRIDGE_MEM32(reboot_va + 4), path);
        }
    }

    if (reboot_va) {
        BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = reboot_va;
    } else {
        const char *cmdline = getenv("RECOMP_CMDLINE");

        if (cmdline && *cmdline) {
            uint32_t page = xbox_HeapAlloc(0x1000 + 0x0C00, 4096);
            if (page) {
                size_t n = strlen(cmdline);
                size_t i;

                if (n > 0x0BFF)
                    n = 0x0BFF;
                BRIDGE_MEM32(page + 0) = 3;          /* LaunchDataType */
                BRIDGE_MEM32(page + 4) = xbox_path_title_id(); /* this title's */
                for (i = 0; i < n; i++)
                    BRIDGE_MEM8(page + 0x400 + (uint32_t)i) =
                        (uint8_t)cmdline[i];
                BRIDGE_MEM8(page + 0x400 + (uint32_t)n) = 0;

                BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) =
                    page;
                fprintf(stderr, "  Launch data: type 3 at 0x%08X, "
                                "command line %s\n", page, cmdline);
            } else {
                BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
            }
        } else {
            BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
        }
    }

    /* Object-type exports. Each gets a DISTINCT non-zero value rather than 0.
     *
     * They were all zero, which is wrong twice over: a title that null-checks
     * one sees "no such type", and a title that distinguishes two of them --
     * ObReferenceObjectByHandle takes an expected type and compares it -- sees
     * every type as equal, so a mutant handle passes a check meant for events.
     * The values are opaque to the game; only identity and non-nullness matter,
     * so they are the export ordinal offset into the kernel data area, which
     * also makes a stray one recognisable in a crash dump.
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE) = XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE)      = XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;

    /* KeTimeIncrement (ordinal 157) - 100ns units per clock tick. 0x2710 is
     * 1 ms, which is what KeTickCount above is updated at. A title dividing by
     * this to convert ticks to time gets a division by zero if it is left 0. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT) = 0x2710;

    /* HalBootSMCVideoMode (ordinal 356) - the AV pack the kernel read from the
     * SMC at boot: the one HalReadSMBusValue(SMC_CMD_AV_PACK) answers
     * (XBOX_SMC_AV_PACK, the HDTV pack; XBA-121). 0 was the SCART pack. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO) = xbox_HalBootSMCVideoMode;

    /* IdexChannelObject is a structure, not an opaque pointer. Guest file-close
     * code walks DeviceQueue.DeviceListHead at +0x28. Host-backed synchronous
     * I/O does not enqueue guest IRPs, so this must be an empty circular list.
     * Reserve separate storage: the old 16-byte slot overlapped the key exports. */
    {
        uint32_t channel=XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
        memset(XBOX_TO_NATIVE(channel),0,0x200);
        BRIDGE_MEM32(channel+0x28)=channel+0x28;
        BRIDGE_MEM32(channel+0x2C)=channel+0x28;
    }

    /* HalDiskCachePartitionCount (ordinal 40) - number of cache partitions.
     * Retail consoles report 3 (X, Y, Z). Titles size a partition array from
     * this, so 0 gives a zero-length array and 1 hides two drives. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS) = 3;

    /* IoCompletionObjectType (ordinal 64) - type object */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    /* IoDeviceObjectType (ordinal 71) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    /* XboxHDKey (ordinal 323) - 16 bytes of zeros (no key) */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxSignatureKey (ordinal 325) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxLANKey (ordinals 326, 355) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinals 327, 356) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 357) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    /* HAL disk identity strings (ordinals 41/42). The exported symbol is an
     * XBOX_ANSI_STRING whose Buffer must be an Xbox VA the title can deref --
     * HalRandGather reads the bytes for entropy. Build the struct and its text
     * inside the kernel data area so both are addressable. */
    {
        struct { uint32_t str_off, buf_off; const char *text; } d[] = {
            { KDATA_DISK_MODEL_STR,  KDATA_DISK_MODEL_BUF,  "XBOXRECOMP VIRTUAL HDD" },
            { KDATA_DISK_SERIAL_STR, KDATA_DISK_SERIAL_BUF, "XR0000000000" },
            /* XeImageFileName (ordinal 326). Declared but never filled in, so
             * its Buffer held whatever was in the page -- Half-Life 2's CRT
             * reads it while working out the running image's path, took the
             * uninitialised bytes as a char*, and dereferenced 0x68737572
             * (the ASCII "rush"). A disc-booted title's value looks like this. */
            { KDATA_XE_IMAGE_FILENAME, KDATA_XE_IMAGE_BUF,
              xbox_image_file_name() },   /* default.xbe, or $RECOMP_IMAGE (XBA-82) */
        };
        for (int k = 0; k < (int)(sizeof(d) / sizeof(d[0])); k++) {
            uint32_t str_va = XBOX_KERNEL_DATA_BASE + d[k].str_off;
            uint32_t buf_va = XBOX_KERNEL_DATA_BASE + d[k].buf_off;
            size_t len = strlen(d[k].text);
            memcpy(XBOX_TO_NATIVE(buf_va), d[k].text, len + 1);
            BRIDGE_MEM16(str_va + 0) = (uint16_t)len;        /* Length */
            BRIDGE_MEM16(str_va + 2) = (uint16_t)(len + 1);  /* MaximumLength */
            BRIDGE_MEM32(str_va + 4) = buf_va;               /* Buffer (Xbox VA) */
        }
    }

    xbox_image_file_name_init();
    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* ── Per-slot ordinal and bridge function ────────────────── */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Log counter - limit output to avoid flooding */
/* Calls per ordinal, for the ranking in the periodic summary. 378 counters
 * is smaller than one of the strings this file prints. */
static unsigned long long g_ordinal_calls[XBOX_KERNEL_THUNK_TABLE_SIZE];
/* 64-bit: a title that polls the clock through the kernel passes 2^31 calls
 * within minutes (X-Men Legends does). As a 32-bit int it wrapped negative,
 * "count <= log budget" turned true, and every kernel call then wrote two log
 * lines through the shared stderr lock -- the frame rate fell to ~1 FPS. */
static long long g_kernel_call_count = 0;

/* How many kernel calls get logged before the log goes quiet.
 *
 * The cap keeps a title that makes thousands of calls from burying the
 * console, but a bring-up that gets past early init then has no visibility at
 * exactly the point it stops being obvious. Override with
 * RECOMP_KERNEL_LOG_BUDGET, the same way RECOMP_TRACE_BUDGET works for the
 * function tracer. 0 silences the log entirely.
 */
static long kernel_log_budget(void)
{
    static long budget = -1;

    if (budget < 0) {
        const char *env = getenv("RECOMP_KERNEL_LOG_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 200;
        if (budget < 0)
            budget = 0;
    }
    return budget;
}

#define KERNEL_LOG_ON()      (g_kernel_call_count <= kernel_log_budget())
/* Some sites logged at a tighter cap than the rest; keep them proportional. */
#define KERNEL_LOG_ON_HALF() (g_kernel_call_count <= kernel_log_budget() / 2)

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* Guest return address of the call currently in a bridge. */
RECOMP_TLS uint32_t g_xbox_kernel_caller;

/* ── Per-ordinal bridge functions ─────────────────────────
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA→native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* ── PsCreateSystemThreadEx (ordinal 255) ────────────────
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA → pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA → pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA → opaque
 *   PVOID StartContext2,       // arg6: Xbox VA → opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 */
static int g_thread_call_count = 0;

/* Thread entry shim. Sets up the new thread's own simulated stack, pushes the
 * two Xbox start-context arguments plus the dummy return address the callee's
 * `ret` consumes, and runs. */
/* Set on threads this bridge spawned; see PsTerminateSystemThread. */
static RECOMP_TLS int g_is_spawned_thread = 0;

/* This thread's simulated stack, so both exits can give it back. Thread-local
 * for the obvious reason, and needed at all because the common exit is
 * ExitThread from PsTerminateSystemThread -- bridge_thread_main's own return
 * path is the rare one. */
static RECOMP_TLS uint32_t g_thread_stack_top = 0;
/* The worker's own TIB + TLS + KTHREAD block (xbox_AllocThreadTib), given
 * back with its stack when it ends (KRN-8); 0 when it is not ours to free. */
static RECOMP_TLS uint32_t g_thread_tib = 0;

/* KRN-8: each worker's TIB block (0x40 + image TLS + KTHREAD + 0x140 bytes
 * of guest heap) was never freed, though its stack was: a title that starts
 * a thread per streamed asset (the dashboard, one per WAV) leaked one block
 * and one heap-table entry per thread until creates failed with
 * STATUS_INSUFFICIENT_RESOURCES. Called on both exit paths, after
 * thobj_exit. Only a worker with a pool object remembers its block: one
 * without keeps its KTHREAD inside the TIB, and other threads may still
 * hold that. */
static void bridge_free_thread_tib(void)
{
    uint32_t tib = g_thread_tib;
    g_thread_tib = 0;
    if (tib) {
        if (g_fs_base == tib)
            g_fs_base = 0;
        xbox_HeapFree(tib);
    }
}

/* Deferred file I/O APCs; see xbox_SetDeferredIoApc and deliver_one_apc.
 *
 * Per thread, because an APC belongs to the thread that issued the request and
 * runs only when that thread waits alertably. A thread that exits with some
 * still queued loses them with its thread-local storage, as the console's
 * thread rundown flushes a dying thread's user APCs without running them.
 * 64 is generous: a title waits between batches of reads, and an overflowing
 * request is delivered inline (the old behaviour) rather than dropped.
 *
 * On by default, the console's rule (XBA-102). RECOMP_INLINE_APC=1 restores
 * the inline delivery for one release, for A/B. -1: not decided yet. */
static int g_defer_io_apc = -1;
#define IO_APC_QUEUE 64
static RECOMP_TLS uint32_t g_io_apc[IO_APC_QUEUE][3];   /* routine, ctx, iosb */
static RECOMP_TLS int g_io_apc_count = 0;

void xbox_SetDeferredIoApc(int on) { g_defer_io_apc = on ? 1 : 0; }

static int defer_io_apc(void)
{
    int v = g_defer_io_apc;
    if (v < 0) {
        const char *e = getenv("RECOMP_INLINE_APC");
        v = !(e && *e && *e != '0');
        if (!v)
            fprintf(stderr, "  [KERNEL] file I/O APCs run inline at the request"
                            " (RECOMP_INLINE_APC)\n");
        g_defer_io_apc = v;
    }
    return v;
}

static int bridge_run_io_apcs(uint32_t alertable, uint32_t wait_mode);
static void bridge_wait_done(uint32_t alertable, uint32_t wait_mode);
static void thalert_forget(uint32_t kthread);

struct bridge_thread_start {
    recomp_func_t fn;
    uint32_t ctx1, ctx2, stack_top, obj_va, tls_size;
    uint32_t unique_id;   /* ETHREAD.UniqueThread the creator gave out */
};

/* Lay out a thread's TLS the way the Xbox kernel does.
 *
 * PsCreateSystemThreadEx takes a TlsDataSize, and the kernel carves that many
 * bytes off the top of the new thread's stack: KTHREAD.TlsData (+0x28) points
 * at them and NtTib.StackBase, fs:[4], is the address just past them. XAPI's
 * entry point stores -(TlsDataSize/4) as the image's TLS index, so
 * [fs:[4] + index*4] and [[fs:[0x28]]+0x28] are the same dword -- the thread
 * startup writes it through the second, the CRT reads it through the first.
 *
 * The loader cannot know that size: it is computed at run time from the TLS
 * directory in .rdata, and a title's header copy of that directory can be all
 * zeros. The loader then built a 4-byte block, fs:[4] sat 4 bytes past it,
 * and the CRT read its per-thread pointer from 0x90 bytes below the block --
 * zeros on most threads (so the CRT's exit path cleared guest address 0xC)
 * and, on one loader thread, the tail of a texture someone else had allocated
 * next to that thread's TIB, which faulted inside the CRT.
 *
 * `kthread` is the object fs:[0x28] names for this thread; `top` is where the
 * block ends. Returns the new fs:[4], 0 when there was nothing to do. */
static uint32_t bridge_layout_tls(uint32_t kthread, uint32_t top,
                                  uint32_t tls_size)
{
    uint32_t data;

    if (!kthread || !top || !tls_size || (tls_size & 3) || tls_size > 0x10000)
        return 0;
    data = (top - tls_size) & ~0xFu;
    memset(XBOX_TO_NATIVE(data), 0, top - data);
    BRIDGE_MEM32(kthread + 0x28) = data;              /* KTHREAD.TlsData */
    BRIDGE_MEM32(g_fs_base + 0x04) = data + tls_size; /* NtTib.StackBase */
    return data + tls_size;
}

static void bridge_write_handle(uint32_t handle_va, HANDLE h);
static void bridge_call_guest(uint32_t va, const uint32_t *args, int n);
static void thread_notify_run(uint32_t thread, int create);   /* XBA-114 */
static int  thread_notify_on(void);
static void bridge_run_shutdown_notifications(void);

/* ── Guest thread objects ─────────────────────────────────
 *
 * A thread handle has to lead to something in guest memory. XAPI's
 * GetExitCodeThread is
 *
 *     ObReferenceObjectByHandle(h, &PsThreadObjectType, &thread);
 *     code = thread->Header.SignalState ? thread->ExitStatus : STILL_ACTIVE;
 *     ObfDereferenceObject(thread);
 *
 * reading SignalState at +4 and ExitStatus at +0x120. ObReferenceObjectByHandle
 * used to hand back NULL, so the title read guest page 0, saw STILL_ACTIVE
 * forever, and a title that waits for its loader thread this way spun on it
 * after the loader had exited.
 *
 * Every spawned thread now gets a zeroed 0x200-byte object (Type 6 =
 * ThreadObject), carved from contiguous pages because that allocator is
 * locked and kernel objects live up there on hardware anyway. The object is
 * reference counted host-side: one reference for the running thread, one per
 * handle token, one per ObReferenceObjectByHandle. When the thread ends, by
 * PsTerminateSystemThread or by returning, ExitStatus is written and then
 * SignalState is set to 1. KeWaitForSingleObject on the object waits on the
 * host thread through the KE shadow table. A slot goes back to the pool once
 * its last reference is dropped.
 */
#define THOBJ_SIZE        0x200u
#define THOBJ_SIGNAL      0x004u
#define THOBJ_EXIT_STATUS 0x120u
#define THOBJ_MAX         256
#define THOBJ_PER_PAGE    (0x1000u / THOBJ_SIZE)

static uint32_t s_thobj_va[THOBJ_MAX];     /* 0 = slot never carved */
static LONG     s_thobj_refs[THOBJ_MAX];   /* 0 = free */
static uint32_t s_thobj_count;             /* slots carved so far */
static uint32_t s_token_thobj[16384];      /* token index -> object VA */
static volatile LONG s_thobj_lock;
static RECOMP_TLS uint32_t g_thread_obj_va;

static void ke_shadow_insert(uint32_t guest_va, HANDLE host);
static void ke_shadow_remove(uint32_t guest_va);

static void thobj_lock(void)
{
    while (InterlockedCompareExchange(&s_thobj_lock, 1, 0) != 0)
        SwitchToThread();
}

static void thobj_unlock(void) { InterlockedExchange(&s_thobj_lock, 0); }

/* Slot of a thread object VA, or -1. Lock held. */
static int thobj_slot(uint32_t va)
{
    uint32_t i;

    if (!va)
        return -1;
    for (i = 0; i < s_thobj_count; i++)
        if (s_thobj_va[i] == va)
            return (int)i;
    return -1;
}

/* A fresh object holding one reference (the thread's). */
static uint32_t thobj_alloc(void)
{
    uint32_t i, va = 0;

    thobj_lock();
    for (i = 0; i < s_thobj_count; i++)
        if (s_thobj_refs[i] == 0)
            break;
    if (i == s_thobj_count && s_thobj_count < THOBJ_MAX) {
        uint32_t page = xbox_ContiguousAlloc(0x1000, 0x1000);
        uint32_t k;

        if (page) {
            for (k = 0; k < THOBJ_PER_PAGE && s_thobj_count < THOBJ_MAX; k++)
                s_thobj_va[s_thobj_count++] = page + k * THOBJ_SIZE;
        }
    }
    if (i < s_thobj_count) {
        va = s_thobj_va[i];
        s_thobj_refs[i] = 1;
    }
    thobj_unlock();

    if (!va) {
        fprintf(stderr, "  [KERNEL] no guest thread object (pool of %d full)\n",
                THOBJ_MAX);
        fflush(stderr);
        return 0;
    }
    memset(XBOX_TO_NATIVE(va), 0, THOBJ_SIZE);
    BRIDGE_MEM8(va + 0) = 6;                      /* Type: ThreadObject */
    BRIDGE_MEM8(va + 2) = (uint8_t)(THOBJ_SIZE / 4);
    BRIDGE_MEM32(va + 8)  = va + 8;               /* WaitListHead, empty */
    BRIDGE_MEM32(va + 12) = va + 8;
    BRIDGE_MEM32(va + THOBJ_EXIT_STATUS) = 0x103; /* STILL_ACTIVE */
    return va;
}

static void thobj_ref(uint32_t va)
{
    int s;

    thobj_lock();
    s = thobj_slot(va);
    if (s >= 0 && s_thobj_refs[s] > 0)
        s_thobj_refs[s]++;
    thobj_unlock();
}

static void thobj_unref(uint32_t va)
{
    int s, freed = 0;

    thobj_lock();
    s = thobj_slot(va);
    if (s >= 0 && s_thobj_refs[s] > 0 && --s_thobj_refs[s] == 0)
        freed = 1;
    thobj_unlock();
    if (freed)
        ke_shadow_remove(va);   /* closes the shadow's host thread handle */
}

/* The thread has ended: publish ExitStatus, then SignalState, then drop the
 * thread's own reference. */
static void thobj_exit(uint32_t va, uint32_t status)
{
    if (!va)
        return;
    BRIDGE_MEM32(va + THOBJ_EXIT_STATUS) = status;
    InterlockedExchange((volatile LONG *)XBOX_TO_NATIVE(va + THOBJ_SIGNAL), 1);
    thobj_unref(va);
}

/* Handle tokens that lead to a thread object. */
static void thobj_attach_token(uint32_t token, uint32_t va)
{
    uint32_t i = token & 0x00FFFFFFu;

    if ((token & 0xFF000000u) != 0x48000000u || !i || i >= 16384 || !va)
        return;
    thobj_ref(va);
    thobj_lock();
    i = InterlockedExchange((volatile LONG *)&s_token_thobj[i], (LONG)va);
    thobj_unlock();
    if (i)
        thobj_unref(i);   /* a stale mapping the slot never cleared */
}

static uint32_t thobj_from_token(uint32_t token)
{
    uint32_t i = token & 0x00FFFFFFu;

    if ((token & 0xFF000000u) != 0x48000000u || !i || i >= 16384)
        return 0;
    return s_token_thobj[i];
}

static void thobj_detach_token(uint32_t token)
{
    uint32_t i = token & 0x00FFFFFFu, va;

    if ((token & 0xFF000000u) != 0x48000000u || !i || i >= 16384)
        return;
    va = (uint32_t)InterlockedExchange((volatile LONG *)&s_token_thobj[i], 0);
    if (va)
        thobj_unref(va);
}

/* The calling thread's object, which is also its KTHREAD: what fs:[0x28]
 * names (XBA-113). A spawned guest thread has had its own since it started;
 * the main thread's is the KTHREAD the loader built, and each TIB from
 * xbox_AllocThreadTib carries one. A thread whose TIB names none gets a pool
 * object, bound to fs:[0x28] so the two answers stay one. */
static uint32_t thobj_current(void)
{
    if (!g_thread_obj_va) {
        uint32_t kt = g_fs_base ? BRIDGE_MEM32(g_fs_base + 0x28) : 0;
        if (!kt) {
            kt = thobj_alloc();
            if (kt) {
                xbox_InitGuestKthread(kt, 0, g_fs_base ? BRIDGE_MEM32(g_fs_base + 4) : 0,
                                      g_fs_base ? BRIDGE_MEM32(g_fs_base + 8) : 0);
                if (g_fs_base)
                    BRIDGE_MEM32(g_fs_base + 0x28) = kt;
            }
        }
        g_thread_obj_va = kt;
    }
    return g_thread_obj_va;
}

/* XBA-64: StartContext1 is often the title's real entry (the thread
 * convention hands the wrapper its routine through it), and nothing calls it
 * directly. One in the title's code range with no dispatch entry goes to the
 * discovery file; tools.seed_from_log's gates decide whether it is code. */
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
static void bridge_discover_thread_ctx(uint32_t routine, uint32_t ctx1)
{
    if (g_xbox_code_hi && ctx1 >= g_xbox_code_lo && ctx1 < g_xbox_code_hi
        && !recomp_lookup(ctx1) && !recomp_lookup_manual(ctx1)) {
        /* Said here, by name: the routine's call of it is skipped (« [ICALL]
         * Failed to resolve VA ») and the thread ends at once, which reads as
         * a thread that had nothing to do; whatever waits for its work then
         * hangs much later with nothing in the log (GitHub issue 1: three
         * such workers, a black screen after the title screen). */
        static int said;
        if (said < 8) {
            said++;
            fprintf(stderr, "  [KERNEL] PsCreateSystemThread: StartContext1 0x%08X is in the title's code but "
                            "the pack has no function there (the analysis missed it). If routine 0x%08X "
                            "calls it, as a library's thread startup does, this thread ends without running: "
                            "prepare the title again (the discovery file names it)\n", ctx1, routine);
            fflush(stderr);
        }
        recomp_discovery_thread_ctx(ctx1, routine);
    }
}

static void bridge_run_thread_inline(recomp_func_t fn, uint32_t ctx1,
                                     uint32_t ctx2)
{
    /* The worker's code is guest code on a guest thread: it holds the guest
     * CPU, which the creating call let go of (kernel_thunk_dispatch). On
     * the spawned path bridge_thread_main joins instead. */
    int took = xbox_GuestCpuEnterCallback();

    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    g_seh_ebp = g_esp;
    xbox_ParkThreadStart();      /* a guest thread for the park detector (XBA-244) */
    fn();
    xbox_ParkThreadEnd();
    g_esp += 12;
    xbox_GuestCpuLeaveCallback(took);
}

void guest_cpu_join(void);
void guest_cpu_part(void);

static DWORD WINAPI bridge_thread_main(LPVOID param)
{
    struct bridge_thread_start *s = (struct bridge_thread_start *)param;
    recomp_func_t fn = s->fn;
    uint32_t ctx1 = s->ctx1, ctx2 = s->ctx2;

    /* Own register set (RECOMP_TLS), own simulated stack -- and own TIB.
     *
     * The TIB carries the SEH chain head and, through fs:[4], the CRT's
     * per-thread data. Sharing one made "which thread am I" a single answer
     * for every thread, which is how two of them ended up inside _lock() each
     * holding the lock the other wanted. */
    g_is_spawned_thread = 1;
    g_esp = s->stack_top;
    g_thread_stack_top = s->stack_top;
    /* xbox_AllocThreadStack hands out the top of the slice, less 16. */
    xbox_SehSetGuestStack(s->stack_top + 16 - XBOX_THREAD_STACK_SIZE,
                          s->stack_top + 16);
    g_thread_obj_va = s->obj_va;
    {
        uint32_t tib = xbox_AllocThreadTib();
        if (tib)
            g_fs_base = tib;
        else
            fprintf(stderr, "  [KERNEL] worker thread has no TIB of its own;"
                            " it shares the main thread's\n");
        /* The thread object stands in for the KTHREAD, so fs:[0x28] names it
         * and its TlsData is this thread's own, at the top of its stack.
         * Without a TlsDataSize it keeps the TIB's (the main thread's), and
         * it is the KTHREAD all the same: KeGetCurrentThread returns it and
         * the fields XAPI reads are filled in (XBA-113). */
        if (tib && s->obj_va) {
            uint32_t top = (s->stack_top + 16) & ~0xFu;
            uint32_t tib_kt = BRIDGE_MEM32(tib + 0x28);
            /* The object is the KTHREAD from here on, so the block is the
             * thread's alone. (Without a TlsDataSize the object borrows the
             * block's TlsData: only this thread's code reads it, and not
             * after the thread has ended.) */
            g_thread_tib = tib;
            xbox_InitGuestKthread(s->obj_va,
                                  tib_kt ? BRIDGE_MEM32(tib_kt + 0x28) : 0,
                                  top, top - 0x80000u);
            BRIDGE_MEM32(tib + 0x28) = s->obj_va;
            if (s->tls_size && bridge_layout_tls(s->obj_va, top, s->tls_size))
                g_esp = ((top - s->tls_size) & ~0xFu) - 16;
        } else if (tib && s->unique_id && BRIDGE_MEM32(tib + 0x28)) {
            /* No pool object: the TIB's own KTHREAD is this thread's, and
             * it answers with the id the creator already returned. */
            BRIDGE_MEM32(BRIDGE_MEM32(tib + 0x28) + 0x12C) = s->unique_id;
        }
    }
    free(s);

    /* A guest thread: it drains queued DPCs at its raises and releases
     * (kernel_hal.c) and shares one host core with the others
     * (kernel_thread.c). The timer, ack and device-model threads do
     * neither. */
    xbox_DpcMarkGuestThread();
    xbox_GuestThreadPin("guest-worker");
    guest_cpu_join();
    bridge_run_thread_inline(fn, ctx1, ctx2);
    guest_cpu_part();

    fprintf(stderr, "  [KERNEL] worker thread returned (eax=0x%08X)\n", g_eax);
    fflush(stderr);
    thread_notify_run(g_thread_obj_va, 0);   /* on its own stack, still ours */
    /* The routine returned instead of calling PsTerminateSystemThread; the
     * stack is still ours to give back. */
    xbox_FreeThreadStack(g_thread_stack_top);
    g_thread_stack_top = 0;
    /* The kernel's thread startup ends a returning routine with
     * PsTerminateSystemThread(STATUS_SUCCESS). */
    thalert_forget(g_thread_obj_va);
    ks_thread_exit();
    thobj_exit(g_thread_obj_va, 0);
    g_thread_obj_va = 0;
    bridge_free_thread_tib();
    return 0;
}

/* ── Threads re-entering a save state's chains (XBA-250) ─────────────────────
 *
 * A save state loaded in a fresh process (xbox_StateResume) recreates each guest thread on a new host thread,
 * with the guest stack, TIB and KTHREAD it had (guest memory, restored), and calls its routine in resume mode
 * (xbox_resume.h): the lifted functions re-enter the saved chain and the innermost one its kernel call or host
 * wait. Created suspended, before the kernel objects are made again (their thread handles are these threads);
 * xbox_StateResume lets them go once everything is back. The INLINE main thread too: it ran inside its creator's
 * kernel call, whose frames are not re-entered; if its routine ever returns, the thread ends and the process
 * main (xbox_BridgeResumeWaitMain) goes on to shut down, as after a main thread returning in the first run. */
struct bridge_resume_start {
    recomp_func_t fn;
    XboxResumePlan *plan;     /* NULL: the resume was abandoned, end at once */
    uint32_t tib, kthread, stack_base, stack_limit;
    int is_main;
};

static HANDLE s_resume_main_done;

static DWORD WINAPI bridge_resume_main(LPVOID param)
{
    struct bridge_resume_start *s = (struct bridge_resume_start *)param;
    struct bridge_resume_start r = *s;

    free(s);
    if (!r.plan || !r.fn) {
        if (r.is_main && s_resume_main_done)
            SetEvent(s_resume_main_done);
        return 0;
    }
    g_is_spawned_thread = !r.is_main;
    /* A worker's stack is a slice of the guest heap (xbox_AllocThreadStack: its top less 16), given back when it
     * ends; the main thread's is the loader's. */
    g_thread_stack_top = r.is_main ? 0 : r.stack_base - 16;
    if (r.is_main)
        xbox_SehSetGuestStack(r.stack_limit, r.stack_base);
    else
        xbox_SehSetGuestStack(r.stack_base - XBOX_THREAD_STACK_SIZE, r.stack_base);
    g_fs_base = r.tib;
    g_thread_obj_va = r.kthread;
    xbox_DpcMarkGuestThread();
    xbox_GuestThreadPin(r.is_main ? "guest-main" : "guest-worker");
    recomp_status_thread_started();
    guest_cpu_join();
    {
        int took = xbox_GuestCpuEnterCallback();
        xbox_ParkThreadStart();
        xbox_ResumeBegin(r.plan);
        r.fn();
        xbox_ParkThreadEnd();
        xbox_GuestCpuLeaveCallback(took);
    }
    guest_cpu_part();
    fprintf(stderr, "  [KERNEL] resumed %s thread returned (eax=0x%08X)\n", r.is_main ? "main" : "worker", g_eax);
    fflush(stderr);
    thread_notify_run(g_thread_obj_va, 0);
    xbox_FreeThreadStack(g_thread_stack_top);
    g_thread_stack_top = 0;
    thalert_forget(g_thread_obj_va);
    ks_thread_exit();
    thobj_exit(g_thread_obj_va, 0);
    g_thread_obj_va = 0;
    if (r.is_main && s_resume_main_done)
        SetEvent(s_resume_main_done);
    return 0;
}

void *xbox_BridgeResumeThread(uint32_t routine, void *plan, uint32_t tib, uint32_t kthread, uint32_t stack_base,
                              uint32_t stack_limit, int is_main)
{
    struct bridge_resume_start *s = (struct bridge_resume_start *)calloc(1, sizeof *s);
    HANDLE th;

    if (!s)
        return NULL;
    s->fn = recomp_lookup(routine);
    if (!s->fn)
        s->fn = recomp_lookup_manual(routine);
    s->plan = (XboxResumePlan *)plan;
    s->tib = tib;
    s->kthread = kthread;
    s->stack_base = stack_base;
    s->stack_limit = stack_limit;
    s->is_main = is_main;
    if (!s->fn) {
        free(s);
        return NULL;
    }
    if (is_main && !s_resume_main_done)
        s_resume_main_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    th = CreateThread(NULL, 0, bridge_resume_main, s, CREATE_SUSPENDED, NULL);
    if (!th)
        free(s);
    else
        xbox_set_game_thread(th);
    return th;
}

void xbox_BridgeResumeSetup(uint32_t max_guest_tid)
{
    /* The INLINE main thread was started in the first run: a PsCreateSystemThreadEx now makes a worker. */
    if (g_thread_call_count == 0)
        g_thread_call_count = 1;
    xbox_GuestThreadIdAtLeast(max_guest_tid);
}

int xbox_BridgeResumeWaitMain(void)
{
    if (!s_resume_main_done)
        return 0;
    WaitForSingleObject(s_resume_main_done, INFINITE);
    return 1;
}

static HANDLE bridge_spawn_thread(recomp_func_t fn, uint32_t ctx1,
                                  uint32_t ctx2, uint32_t stack_top,
                                  uint32_t obj_va, uint32_t tls_size,
                                  int suspended, uint32_t unique_id)
{
    struct bridge_thread_start *s = malloc(sizeof(*s));
    HANDLE th;

    if (!s) return NULL;
    s->fn = fn; s->ctx1 = ctx1; s->ctx2 = ctx2; s->stack_top = stack_top;
    s->obj_va = obj_va;
    s->tls_size = tls_size;
    s->unique_id = unique_id;

    /* CreateSuspended: the thread waits for NtResumeThread before its first
     * instruction. A title creates a worker suspended to finish its context
     * first; started at once, on a multi-core host it ran on a context still
     * zero (Fable: a loading worker spun forever on a NULL object). */
    th = CreateThread(NULL, 0, bridge_thread_main, s,
                      suspended ? CREATE_SUSPENDED : 0, NULL);
    if (!th) free(s);
    /* Record the game thread so a host-tick-driven title's watchdog can sample
     * it via xbox_thread_debug_handle. Harmless for default-model titles: they
     * spawn workers too, but never read it back. See kernel_thread.c. */
    else xbox_set_game_thread(th);
    return th;
}

/* A worker with its guest thread object: the object is made first and holds
 * an extra reference until the handle token owns one, so a worker that ends
 * at once cannot free it underneath us. The KE shadow gets its own duplicate
 * of the host thread handle, which is what KeWaitForSingleObject waits on. */
static HANDLE bridge_spawn_guest_thread(recomp_func_t fn, uint32_t ctx1,
                                        uint32_t ctx2, uint32_t stack_top,
                                        uint32_t handle_va, uint32_t tls_size,
                                        int suspended, uint32_t thread_id_va)
{
    recomp_status_thread_started();
    uint32_t obj = thobj_alloc();
    uint32_t id = xbox_NewGuestThreadId();
    HANDLE th, shadow = NULL;

    int notify = obj && thread_notify_on();

    /* ThreadId is the new thread's ETHREAD.UniqueThread, known to the creator
     * before the thread runs: a title keeps CreateThread's lpThreadId and
     * compares it with GetCurrentThreadId() (fs:[0x28]+0x12C) on the thread
     * itself, e.g. to tell whether it owns a heap. With the object pool full
     * the thread's TIB KTHREAD takes the id instead (bridge_thread_main). */
    if (obj)
        BRIDGE_MEM32(obj + 0x12C) = id;
    if (thread_id_va)
        BRIDGE_MEM32(thread_id_va) = id;
    thobj_ref(obj);
    /* With thread notify routines, the thread waits for them: the console
     * calls them before the new thread runs (PsSetCreateThreadNotifyRoutine). */
    th = bridge_spawn_thread(fn, ctx1, ctx2, stack_top, obj, tls_size,
                             suspended || notify, id);
    if (!th) {
        thobj_unref(obj);   /* the thread's own: it never ran */
    } else {
        if (obj && DuplicateHandle(GetCurrentProcess(), th, GetCurrentProcess(),
                                   &shadow, 0, FALSE, DUPLICATE_SAME_ACCESS))
            ke_shadow_insert(obj, shadow);
        if (handle_va) {
            bridge_write_handle(handle_va, th);
            thobj_attach_token(BRIDGE_MEM32(handle_va), obj);
        }
        if (notify) {
            thread_notify_run(obj, 1);
            if (!suspended)
                ResumeThread(th);
        }
    }
    thobj_unref(obj);
    return th;
}

/* Two ways a title expects its first PsCreateSystemThreadEx to behave.
 *
 * INLINE (default): the first call IS the game starting -- run the routine
 * inline, inheriting register state, and it drives its own main loop forever.
 * This is what Halo and Crimson Skies need and the historical behavior.
 *
 * SPAWN: the title's entry spawns an init thread and RETURNS, expecting the host
 * to drive the per-frame tick afterwards (Burnout 3 is tick-driven). Here the
 * first call must spawn a real thread and return, so control comes back to the
 * host. Opt in with xbox_SetThreadMode before the game starts. See
 * docs/technical/burnout3-reunification.md. */
/* XBOX_THREAD_MODE_* and xbox_SetThreadMode are declared in xbox_memory_layout.h. */
static int g_thread_mode = XBOX_THREAD_MODE_INLINE;
void xbox_SetThreadMode(int mode) { g_thread_mode = mode; }

static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t tls_size        = STACK_ARG(3);
    uint32_t xbox_thread_id_ptr = STACK_ARG(4);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);
    int create_suspended     = (STACK_ARG(7) & 0xFF) != 0;   /* BOOLEAN */
    /* In SPAWN mode there is no privileged "first call": every thread is real,
     * so the entry can return. In INLINE mode the first call runs the game. */
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);
    g_thread_call_count++;

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%d: routine=0x%08X ctx1=0x%08X ctx2=0x%08X\n",
            g_thread_call_count, start_routine, start_context1, start_context2);
    fflush(stderr);
    bridge_discover_thread_ctx(start_routine, start_context1);

    /* Write a fake handle to the output pointer */
    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;  /* fake handle */
    }
    /* ThreadId: a routine run on this thread (the first call, or
     * RECOMP_WORKERS=inline) has this thread's id; a spawned worker's
     * replaces it before the worker runs (bridge_spawn_guest_thread). */
    if (xbox_thread_id_ptr) {
        uint32_t kt = thobj_current();
        BRIDGE_MEM32(xbox_thread_id_ptr) = kt ? BRIDGE_MEM32(kt + 0x12C) : 0;
    }

    /* Call the start routine synchronously through the recomp dispatch.
     * Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * We push both onto the simulated stack (right-to-left).
     *
     * First call: the game's main thread entry point. Must run synchronously
     * and inherit the current register state (this IS the game starting).
     *
     * Subsequent calls: worker threads. Must save/restore ALL global registers
     * because on real Xbox each thread has its own register set. Without this,
     * the worker clobbers the caller's g_esi, g_ebx, etc. */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                /* Main game thread: run directly, inheriting register state */
                /* Its TLS too: the stack is already in use, so the block
                 * comes from the heap, and the main TIB's fs:[0x28] object
                 * gets the TlsData pointer (see bridge_layout_tls). */
                if (tls_size) {
                    uint32_t blk = xbox_HeapAlloc((tls_size + 0xFu) & ~0xFu, 16);
                    uint32_t fs4 = blk ? bridge_layout_tls(
                        BRIDGE_MEM32(g_fs_base + 0x28),
                        blk + ((tls_size + 0xFu) & ~0xFu), tls_size) : 0;
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: %u-byte"
                            " TLS at 0x%08X, fs:[4] = 0x%08X\n", tls_size,
                            fs4 ? fs4 - tls_size : 0, fs4);
                    fflush(stderr);
                }
                xbox_DpcMarkGuestThread();
                xbox_GuestThreadPin("guest-main");
                recomp_status_thread_started();   /* the title's main thread, run here (XBA-79) */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                /* The title's main thread runs inside this kernel call, which
                 * let the guest CPU go (RECOMP_GUEST_LOCK): take it back for
                 * the thread's whole life, as for any guest code a bridge
                 * runs, or the main thread runs unlocked against every
                 * worker, the race the lock is for. */
                {
                    int took = xbox_GuestCpuEnterCallback();
                    xbox_ParkThreadStart();
                    fn();
                    xbox_ParkThreadEnd();
                    xbox_GuestCpuLeaveCallback(took);
                }
                g_esp += 12;
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: main thread returned (g_eax=0x%08X)\n", g_eax);
                fflush(stderr);
            } else {
                /* Worker thread: a real one.
                 *
                 * This used to run the routine synchronously and restore the
                 * caller's registers afterwards, which is fine only for a
                 * worker that finishes. Halo's cache/file worker does not -- it
                 * blocks on an event waiting for requests, so CreateThread
                 * never returned and startup deadlocked before the main loop.
                 *
                 * Now that the register set is thread-local (RECOMP_TLS), a
                 * spawned thread gets its own, and the caller's is untouched by
                 * construction rather than by save/restore. */
                /* RECOMP_WORKERS=inline runs a title's worker routines on
                 * the calling thread instead of spawning one.
                 *
                 * Not a mode to ship a title in -- a worker that blocks
                 * waiting for requests never returns, and the caller never
                 * gets control back. It is a bisecting tool: when something
                 * only goes wrong with two guest threads running, this says so
                 * in one run, and the answer separates a concurrency bug from
                 * everything else it might have been. */
                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    fprintf(stderr, "  [KERNEL] RECOMP_WORKERS=inline: running "
                            "worker 0x%08X on this thread\n", start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    return;
                }

                stack_top = xbox_AllocThreadStack();

                if (!stack_top
                    || !bridge_spawn_guest_thread(fn, start_context1, start_context2,
                                                  stack_top, xbox_handle_ptr,
                                                  tls_size, create_suspended,
                                                  xbox_thread_id_ptr)) {
                    /* An error, as on the console when no kernel stack can
                     * be had (XBA-104). Running the worker inline instead
                     * deadlocked a worker that waits for its creator's
                     * requests. A host thread that fails to start gives its
                     * stack back: it never ran. */
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: no %s"
                            " for worker 0x%08X: STATUS_INSUFFICIENT_RESOURCES\n",
                            stack_top ? "host thread" : "stack", start_routine);
                    fflush(stderr);
                    xbox_FreeThreadStack(stack_top);
                    if (xbox_handle_ptr)
                        BRIDGE_MEM32(xbox_handle_ptr) = 0;
                    g_eax = 0xC000009Au;
                    return;
                } else {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawned "
                            "worker 0x%08X (ctx=0x%08X, stack top 0x%08X%s)\n",
                            start_routine, start_context1, stack_top,
                            create_suspended ? ", suspended" : "");
                    fflush(stderr);
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
            recomp_discovery_thread(start_routine, start_context1);
        }
    }

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtClose (ordinal 187) ───────────────────────────────
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */

/* Asynchronous-handle bookkeeping, defined with the file bridges below. */
static void bridge_note_async_handle(uint32_t token);
static void bridge_forget_async_handle(uint32_t token);
static int  bridge_handle_is_async(uint32_t token);

/* Handle-table helpers; defined further below. Xbox memory slots are 32-bit
 * but native HANDLEs are 64-bit pointers, so handles are kept in a table and
 * referenced by tagged 32-bit tokens. */
static void   bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_take_handle(uint32_t token);

static int ob_close_token(uint32_t token);   /* objects of ObCreateObject's */

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", raw_handle);
        fflush(stderr);
    }

    bridge_forget_async_handle(raw_handle);
    thobj_detach_token(raw_handle);
    ob_close_token(raw_handle);

    /* Close real handles but skip fake/synthetic ones */
    if (raw_handle && raw_handle != 0xDEAD0001u && raw_handle != 0xBEEF0010u) {
        HANDLE h = bridge_take_handle(raw_handle);
        if (h && h != INVALID_HANDLE_VALUE) {
            ks_port_forget(h);   /* an I/O completion object, or a file joined to one */
            xbox_NtClose(h);   /* settles a pending overwrite; drops a dir context */
        }   /* settles a pending overwrite; drops a dir context */
    }
    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── MmAllocateContiguousMemory (ordinal 165) ─────────────
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* From the contiguous window, not the general heap: the caller is
     * entitled to assume (VA & 0x0FFFFFFF) | 0x80000000 == VA, because that
     * is how a driver converts between the address it holds and the one it
     * hands the hardware. See xbox_ContiguousAlloc. */
    uint32_t xbox_va = xbox_ContiguousAlloc(size, 4096);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmAllocateSystemMemory (ordinal 167) ─────────────────
 * PVOID MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
 *
 * The kernel's own page allocator: whole pages, not the title's heap, and not
 * required to be physically contiguous. Titles use it for large buffers they
 * intend to keep -- Half-Life 2 takes one while loading a level, right after
 * it opens the .bsp.
 *
 * Unbridged this returned 0, and a NULL from an allocator is not a failure the
 * caller checks for here; it carried the pointer into graphics setup and hung
 * touching the NV2A aperture with it. Page-aligned out of the ordinary heap is
 * the right answer: the distinction the console draws between system memory
 * and the title heap is about which pool the pages come from, and there is one
 * pool here.
 *
 * Zeroed, because the console hands out zeroed pages and callers assume it.
 */
static void bridge_MmAllocateSystemMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t prot = STACK_ARG(1);
    uint32_t xbox_va;

    (void)prot;
    if (!size) {
        g_eax = 0;
        return;
    }
    xbox_va = xbox_HeapAlloc(size, 4096);
    if (xbox_va)
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);
    else
        fprintf(stderr, "  [KERNEL] MmAllocateSystemMemory: %u bytes REFUSED\n",
                size);
    g_eax = xbox_va;
}

/* ── MmAllocateContiguousMemoryEx (ordinal 166) ───────────
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
/* Contiguous memory is addressed through the physical-memory mirror: physical
 * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
 * physical addresses check the returned pointer against that, so the address
 * has to be honoured rather than satisfied from the general heap. */
#define XBOX_PHYSICAL_MIRROR_BASE 0x80000000u

static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);
    uint32_t xbox_va;

    (void)prot;

    /*
     * A caller that constrains the range to exactly one allocation's worth is
     * demanding a specific physical address, not expressing a preference.
     * Halo does this for its two big pools and asserts on the result
     * (physical_memory_map.c:46) - XPhysicalAlloc passes lowest = the address
     * it wants and highest = lowest + size - 1, then requires
     * 0x80000000 | lowest back. Satisfying that from the heap fails the assert
     * and leaves its whole memory map wrong.
     */
    if (low && high >= low && (high - low + 1) <= size + 0x1000) {
        xbox_va = XBOX_PHYSICAL_MIRROR_BASE + low;

        /* Inside the window, or refused (KRN-15): a low beyond it wrote
         * outside the mapping, a host fault inside the runtime with no
         * status for the guest. */
        if ((uint64_t)low + size > (uint64_t)XBOX_CONTIG_SIZE) {
            static int said;
            if (said++ < 8)
                fprintf(stderr, "  [CONTIG] refused: pinned phys 0x%08X size=%u is outside "
                                "the %u MB window (caller 0x%08X)\n",
                        low, size, (unsigned)XBOX_CONTIG_MB, g_xbox_kernel_caller);
            g_eax = 0;
            return;
        }

        /* The console hands out zeroed pages here, and titles rely on it:
         * pool headers and free-list roots are assumed clear, so whatever the
         * backing view happened to contain shows up later as structures that
         * are "allocated" but full of garbage. Only the pages no block owns
         * yet: the claim leaves owned pages to their owner, and zeroing them
         * first wiped a live texture or pushbuffer of the title. (Reading
         * the map before the claim leaves a window against a concurrent
         * allocation; zeroing inside the claim, under the arena lock, would
         * close it.) */
        {
            const volatile uint8_t *map = xbox_ContiguousPageMap();
            uint32_t end = low + size, p;
            for (p = low & ~0xFFFu; p < end; p += 0x1000u) {
                uint32_t from = p < low ? low : p;
                uint32_t to = p + 0x1000u < end ? p + 0x1000u : end;
                if (!map || !map[p >> 12])
                    memset((void *)((uintptr_t)(XBOX_PHYSICAL_MIRROR_BASE + from)
                                    + g_xbox_mem_offset), 0, to - from);
            }
        }
        /* Keep the arena from handing the same pages to someone else. */
        xbox_ContiguousClaim(xbox_va, size);

        if (KERNEL_LOG_ON_HALF()) {
            fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u "
                    "pinned phys 0x%08X -> Xbox VA 0x%08X (zeroed)\n",
                    size, low, xbox_va);
            fflush(stderr);
        }
        g_eax = xbox_va;
        return;
    }

    if (align < 4096) align = 4096;
    /* Lowest/highest are physical and inclusive; the window maps physical P
     * at XBOX_CONTIG_BASE + P, so the arena can honour them directly. */
    if (high < low) {
        g_eax = 0;
        return;
    }
    xbox_va = xbox_ContiguousAllocEx(size, align, low, high);

    /* A refusal is the title's problem only if it says who asked: the guest
     * return address and the stack above it (the XDK wrapper's callers). */
    if (!xbox_va) {
        static int n;
        if (n++ < 8) {
            int k;
            fprintf(stderr, "  [CONTIG] refused: caller 0x%08X size=%u low=0x%08X "
                    "high=0x%08X align=%u prot=0x%X stack:", g_xbox_kernel_caller,
                    size, low, high, align, prot);
            for (k = 5; k < 40; k++)
                fprintf(stderr, " %08X", STACK_ARG(k));
            fprintf(stderr, "\n");
        }
    }

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u → Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmFreeContiguousMemory (ordinal 171) ─────────────────
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Contiguous blocks come from their own arena, not the heap: passing
     * them to xbox_HeapFree found nothing, so none ever came back. */
    if (!xbox_ContiguousFree(addr))
        xbox_HeapFree(addr);
    g_eax = 0;
}

/* ── NtAllocateVirtualMemory (ordinal 184) ────────────────
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    /* RECOMP_EXT_VMA: a request for a specific address above the RAM mirrors
     * gets exactly that address, or a failure the caller can act on. The heap
     * path below never honours a base; it substitutes whatever its cursor is
     * at, which breaks any title that checks what it got against what it asked
     * for. Everything else (base = 0, or an address in mapped RAM) carries on
     * below. */
    if (base_ptr && size_ptr) {
        uint32_t vm_base = base_hint;
        uint32_t vm_size = size;
        uint32_t vm_status;

        if (guest_vmem_allocate(&vm_base, &vm_size, alloc_type, protect, &vm_status)) {
            if (KERNEL_LOG_ON()) {
                fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory (extended VMA): "
                                "base=0x%08X size=%u status=0x%08X\n",
                        vm_base, vm_size, vm_status);
                fflush(stderr);
            }
            if (vm_status == 0) {
                BRIDGE_MEM32(base_ptr) = vm_base;
                BRIDGE_MEM32(size_ptr) = vm_size;
            }
            g_eax = vm_status;
            return;
        }
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    /* A reservation at an address the title chose, above RAM: the address
     * it asked for, or STATUS_CONFLICTING_ADDRESSES, as on the console. The
     * heap path below never honours a base, and Intrinsic Alchemy's sbrk
     * reserves exactly the run VirtualQuery called free and gives up on any
     * other answer: its arena then fails every allocation (Yu-Gi-Oh! The Dawn
     * of Destiny). RECOMP_EXT_VMA's tracker, when on, already answered above. */
    if (base_hint && (alloc_type & 0x2000) && base_ptr && size_ptr) {
        uint32_t rb = base_hint, rs = size;
        int st = xbox_ReserveAt(&rb, &rs);

        if (st != -1) {         /* -1: not ours, carry on */
            if (KERNEL_LOG_ON()) {
                fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory (at the asked "
                                "address): base=0x%08X size=%u status=0x%08X\n",
                        rb, rs, (uint32_t)st);
                fflush(stderr);
            }
            if (st == 0) {
                BRIDGE_MEM32(base_ptr) = rb;
                BRIDGE_MEM32(size_ptr) = rs;
            }
            g_eax = (uint32_t)st;
            return;
        }
    }

    /* An address above physical RAM is not free memory -- it aliases.
     *
     * The runtime maps 64 MB and then mirrors it at 64 MB intervals,
     * because real Xbox RAM wraps on a 26-bit address bus. So a guest that
     * sub-allocates past the top of RAM does not get fresh pages, it gets
     * low memory that something else already owns, and the two quietly
     * share storage. Half-Life 2 put a CUtlRBTree element array at
     * 0x0CB80000, which aliases 0x00B80000; other regions it took land
     * inside the live heap (0x05B80000 -> 0x01B80000).
     *
     * Real hardware wraps *physical* addresses while translating virtual
     * ones, so this never happens there. Modelling every guest address as
     * physical is the gap, and that is a bigger change than a bridge fix.
     * Until then, say so: silent aliasing surfaces as corrupted data
     * structures far from here, which is the worst way to find it.
     */
    if (base_hint >= (g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram)
            && !xbox_ReserveHolds(base_hint, 1)) {
        static unsigned warned;
        if (warned++ < 8)
            fprintf(stderr,
                    "  [KERNEL] WARNING: allocation at 0x%08X is above "
                    "%u MB mapped; it aliases 0x%08X\n",
                    base_hint,
                    (unsigned)((g_xbox_map_size ? g_xbox_map_size
                                                : g_xbox_total_ram)
                               / (1024 * 1024)),
                    (uint32_t)(base_hint % (g_xbox_map_size
                                            ? g_xbox_map_size
                                            : g_xbox_total_ram)));
        fflush(stderr);
    }

    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        if (KERNEL_LOG_ON()) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT).
     *
     * A pure MEM_RESERVE costs no RAM on real hardware -- it takes address
     * space out of a 4 GB range, not pages out of the 64 MB of memory -- so
     * titles reserve far more than the console physically has and commit a
     * fraction of it. Our heap is a bump allocator that commits everything it
     * hands out, so a large reserve asks for RAM that does not exist.
     *
     * Half-Life 2's XBE header sets PeHeapReserve to 128 MB, and its CRT
     * reserves exactly that during RtlCreateHeap. Failing it returned
     * STATUS_NO_MEMORY, RtlCreateHeap returned 0, and CRT init aborted before
     * main -- on a console with 64 MB, asking for 128 MB is normal, not an
     * error.
     *
     * So a reserve that does not fit is clamped to what the heap can actually
     * back, and the caller is told the real size through the IN/OUT RegionSize
     * parameter, which is where the API already reports the rounded figure.
     *
     * ponytail: the honest fix is a reserve that costs nothing and a commit
     * that backs pages on demand, which needs the allocator to separate the
     * two. This clamp is enough for a title that reserves generously and
     * commits little, and it fails loudly and later rather than silently and
     * at startup if one does not. */
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        /* A pure reservation too big for the heap. Take it from the mapped
         * space above RAM, where it costs no heap and the pages are distinct.
         *
         * Clamping instead -- handing back a fraction of what was asked for --
         * is what broke Half-Life 2. It reserves 128 MB and then 200 MB, got
         * 32 MB and 12.5 MB, and then sub-allocated across the range it
         * believed it owned. That walks past the top of RAM, where the mirrors
         * alias low memory, so its containers quietly shared storage with the
         * live heap. Granting the full range is both more honest and less
         * damaging. Without a mapping larger than RAM (xbox_SetMapSize) the
         * range is a lazy reservation at the top of user space, backed only
         * where the title touches it (XBA-103); RECOMP_LAZY_RESERVE=0 brings
         * the clamp below back. */
        xbox_va = xbox_ReserveAlloc(size, 4096);
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u"
                            " granted at 0x%08X above RAM\n", size, xbox_va);
            fflush(stderr);
        }
    }
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        uint32_t want = size;
        while (want > 0x10000 && !xbox_va) {
            want /= 2;
            xbox_va = xbox_HeapAlloc(want, 4096);
        }
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u "
                            "clamped to %u (heap cannot back the full range)\n",
                    size, want);
            fflush(stderr);
            size = want;
        }
    }
    if (!xbox_va) {
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtFreeVirtualMemory (ordinal 199) ────────────────────
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
/* -- NtQueryVirtualMemory (ordinal 217, 2 args = 8 bytes) --------------
 *
 * Xbox takes two arguments, not NT's four:
 *
 *     NTSTATUS NtQueryVirtualMemory(PVOID BaseAddress,
 *                                   PMEMORY_BASIC_INFORMATION Info);
 *
 * This has to be a guest-side answer. The existing xbox_NtQueryVirtualMemory
 * in kernel_memory.c calls the host VirtualQuery and memcpy's a host
 * MEMORY_BASIC_INFORMATION into guest memory, whose pointer fields are 64-bit
 * on an x64 build -- so every field after BaseAddress lands in the wrong place.
 *
 * It also has to exist at all. Without a bridge entry the thunk is left
 * unbridged, and a title's CRT heap creation calls this to probe its heap
 * region: RtlCreateHeap does
 *
 *     call NtQueryVirtualMemory ; test eax,eax ; jl fail
 *     cmp  mbi.BaseAddress, requested ; jne fail
 *     cmp  mbi.State, MEM_FREE        ; je  fail
 *
 * and returns 0 on any of those. On Half-Life 2 that null heap propagated
 * silently through the rest of CRT init.
 *
 * Guest MEMORY_BASIC_INFORMATION, 32-bit, 28 bytes:
 *     +0x00 BaseAddress   +0x04 AllocationBase  +0x08 AllocationProtect
 *     +0x0C RegionSize    +0x10 State           +0x14 Protect
 *     +0x18 Type
 *
 * The guest is one flat committed mapping, so that is what we report: any
 * address inside it is MEM_COMMIT / PAGE_READWRITE / MEM_PRIVATE, and anything
 * outside is MEM_FREE rather than an error, which is the honest answer and the
 * one that lets a caller distinguish the two.
 */
static void bridge_NtQueryVirtualMemory(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t info_va = STACK_ARG(1);
    uint32_t page_base = base_va & ~0xFFFu;

    if (!info_va) {
        g_eax = 0xC000000Du;               /* STATUS_INVALID_PARAMETER */
        return;
    }

    /* Anything the extended-VMA tracker owns is answered from its own records.
     * The generic answer below calls everything above RAM free, so a caller
     * walking its address space for a free range would be handed one that is
     * live. */
    {
        uint32_t vm_info[7];

        if (guest_vmem_query(base_va, vm_info)) {
            BRIDGE_MEM32(info_va + 0x00) = vm_info[0]; /* BaseAddress */
            BRIDGE_MEM32(info_va + 0x04) = vm_info[1]; /* AllocationBase */
            BRIDGE_MEM32(info_va + 0x08) = vm_info[2]; /* AllocationProtect */
            BRIDGE_MEM32(info_va + 0x0C) = vm_info[3]; /* RegionSize */
            BRIDGE_MEM32(info_va + 0x10) = vm_info[4]; /* State */
            BRIDGE_MEM32(info_va + 0x14) = vm_info[5]; /* Protect */
            BRIDGE_MEM32(info_va + 0x18) = vm_info[6]; /* Type */
            g_eax = 0;
            return;
        }
    }

    /* Past the highest VAD address there is no user memory to describe, and
     * the console says so: STATUS_INVALID_PARAMETER. Scanners rely on it to
     * stop -- `for (a = 0; NtQueryVirtualMemory(a, &mbi) >= 0; a += size)`.
     * Answering MEM_FREE up there instead kept such a loop wrapping round the
     * 4 GB space for ever (Yu-Gi-Oh! The Dawn of Destiny, black screen and
     * millions of queries a second). */
    if (base_va >= GUEST_VMEM_TOP) {
        g_eax = 0xC000000Du;               /* STATUS_INVALID_PARAMETER */
        return;
    }

    /* Above the RAM view, the lazy reservations answer: theirs read as
     * committed, the rest as one free run up to the next reservation. */
    {
        uint32_t lz[7];

        if (xbox_ReserveQuery(base_va, lz)) {
            int k;
            for (k = 0; k < 7; k++)
                BRIDGE_MEM32(info_va + 4u * (uint32_t)k) = lz[k];
            g_eax = 0;
            return;
        }
    }

    BRIDGE_MEM32(info_va + 0x00) = page_base;          /* BaseAddress */
    BRIDGE_MEM32(info_va + 0x04) = page_base;          /* AllocationBase */
    BRIDGE_MEM32(info_va + 0x08) = 0x04;               /* PAGE_READWRITE */
    BRIDGE_MEM32(info_va + 0x14) = 0x04;               /* Protect */
    BRIDGE_MEM32(info_va + 0x18) = 0x20000;            /* MEM_PRIVATE */

    if (page_base >= g_xbox_code_lo && page_base < XBOX_TOTAL_RAM) {
        BRIDGE_MEM32(info_va + 0x0C) = XBOX_TOTAL_RAM - page_base; /* RegionSize */
        BRIDGE_MEM32(info_va + 0x10) = 0x1000;         /* MEM_COMMIT */
    } else {
        /* Free to the next boundary, not one page. A caller enumerating free
         * memory then steps across the range instead of crawling it 4 KB at a
         * time; past the user range that is 0x80000 queries per lap, and on the
         * title this came from the walk wrapped at 0xFFFFFFFF and never ended.
         * Only with RECOMP_EXT_VMA, since it changes what every query above
         * the image answers. */
        uint32_t region = 0x1000;

        if (guest_vmem_active()) {
            /* Only where nothing else owns the answer: below the image, and
             * above the tracker's range. */
            uint32_t end = 0;

            if (page_base < g_xbox_code_lo)
                end = g_xbox_code_lo;
            if (end > page_base)
                region = end - page_base;
        }
        BRIDGE_MEM32(info_va + 0x0C) = region;
        BRIDGE_MEM32(info_va + 0x10) = 0x10000;        /* MEM_FREE */
        BRIDGE_MEM32(info_va + 0x08) = 0;
        BRIDGE_MEM32(info_va + 0x18) = 0;
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtQueryVirtualMemory: base=0x%08X -> "
                        "state=0x%X size=%u\n", base_va,
                        BRIDGE_MEM32(info_va + 0x10),
                        BRIDGE_MEM32(info_va + 0x0C));
        fflush(stderr);
    }
    g_eax = 0;                                          /* STATUS_SUCCESS */
}

/* NtFreeVirtualMemory without RECOMP_HEAP_RECLAIM, answered in guest terms.
 *
 * Both pointer arguments are guest addresses of guest values: *BaseAddress is
 * a 32-bit guest VA and *FreeSize a 32-bit byte count. Handing
 * XBOX_TO_NATIVE(base_ptr) to xbox_NtFreeVirtualMemory as a host PVOID * read
 * 8 bytes there -- the guest VA plus the next stack word -- and called the
 * host VirtualFree on the result. That failed every time (one title logged
 * tens of thousands of "NtFreeVirtualMemory failed ... err=487" in minutes),
 * and even a correct read would have released a slice of the guest RAM view
 * out from under the title.
 *
 * Guest memory is one committed mapping, and the regions a title frees here
 * came from bridge_NtAllocateVirtualMemory, i.e. from xbox_HeapAlloc (or
 * xbox_ReserveAlloc above RAM). So:
 *
 *   MEM_DECOMMIT  the pages stay mapped; they are zeroed, which is what the
 *                 title sees on hardware when it commits them again (a
 *                 recommit is a no-op in the allocate bridge).
 *   MEM_RELEASE   the heap block goes back to xbox_HeapFree (reused only
 *                 under RECOMP_HEAP_RECLAIM). A reservation above RAM comes
 *                 from a bump allocator and is not reused.
 *
 * NT rounds the range out to whole pages and writes the rounded base and
 * size back, and so does this. */
#define BRIDGE_MEM_DECOMMIT 0x4000u
#define BRIDGE_MEM_RELEASE  0x8000u
#define BRIDGE_FREE_VM_NOT_AT_BASE 0xC000009Fu

/* True when va lies inside a heap block but not at its start; left is
 * xbox_HeapBlockSize(va). The byte before an interior address is in the same
 * block, one byte further from its end. NT refuses a release there. */
static int bridge_heap_interior(uint32_t va, uint32_t left)
{
    return left && va && xbox_HeapBlockSize(va - 1) == left + 1;
}

static void bridge_free_guest_pages(uint32_t base_ptr, uint32_t size_ptr,
                                    uint32_t free_type)
{
    static unsigned long decommits, releases, refused;
    static uint64_t decommit_bytes;
    uint32_t base, size, page_base, end, left;
    uint32_t mapped = (uint32_t)(g_xbox_map_size ? g_xbox_map_size
                                                 : g_xbox_total_ram);

    if (!base_ptr || !size_ptr || !bridge_va_mapped(base_ptr, 4)
            || !bridge_va_mapped(size_ptr, 4)) {
        g_eax = 0xC0000005u;                    /* STATUS_ACCESS_VIOLATION */
        return;
    }
    base = BRIDGE_MEM32(base_ptr);
    size = BRIDGE_MEM32(size_ptr);

    if ((free_type != BRIDGE_MEM_DECOMMIT && free_type != BRIDGE_MEM_RELEASE)
            || (free_type == BRIDGE_MEM_RELEASE && size != 0)) {
        g_eax = 0xC00000F0u;           /* STATUS_INVALID_PARAMETER_2..3 */
        if (refused++ < 8) {
            fprintf(stderr, "  [KERNEL] NtFreeVirtualMemory: base=0x%08X "
                    "size=%u type=0x%X refused (bad type/size)\n",
                    base, size, free_type);
            fflush(stderr);
        }
        return;
    }

    /* A lazy reservation (xbox_ReserveAlloc, XBA-103): its pages go back to
     * the host and read as zero afterwards. */
    {
        uint32_t lb = 0, ls = 0;
        int r = xbox_ReserveFree(base, size, free_type == BRIDGE_MEM_RELEASE,
                                 &lb, &ls);
        if (r < 0) {
            g_eax = BRIDGE_FREE_VM_NOT_AT_BASE;
            return;
        }
        if (r > 0) {
            BRIDGE_MEM32(base_ptr) = lb;
            BRIDGE_MEM32(size_ptr) = ls;
            g_eax = 0;
            if (free_type == BRIDGE_MEM_RELEASE)
                releases++;
            else {
                decommits++;
                decommit_bytes += ls;
            }
            return;
        }
    }

    /* Only the dynamic heap and the reservations above RAM are ever handed
     * out by the allocate bridge. Anything lower is the image or the stacks,
     * and zeroing it on a stray request would be far worse than refusing. */
    page_base = base & ~0xFFFu;
    if (page_base < XBOX_HEAP_BASE || page_base >= mapped) {
        g_eax = 0xC00000A0u;                /* STATUS_MEMORY_NOT_ALLOCATED */
        if (refused++ < 8) {
            fprintf(stderr, "  [KERNEL] NtFreeVirtualMemory: base=0x%08X "
                    "type=0x%X outside guest memory\n", base, free_type);
            fflush(stderr);
        }
        return;
    }

    left = xbox_HeapBlockSize(page_base);
    if (free_type == BRIDGE_MEM_RELEASE) {
        if (base != page_base || bridge_heap_interior(page_base, left)) {
            g_eax = BRIDGE_FREE_VM_NOT_AT_BASE;
            if (refused++ < 8) {
                fprintf(stderr, "  [KERNEL] NtFreeVirtualMemory: base=0x%08X "
                        "release inside a block refused\n", base);
                fflush(stderr);
            }
            return;
        }
        size = left;
        xbox_HeapFree(page_base);          /* ignores what it never handed out */
        releases++;
    } else {
        /* FreeSize 0 means "the whole region from here". */
        if (!size)
            size = left;
        end = (uint32_t)(((uint64_t)base + size + 0xFFFu) & ~(uint64_t)0xFFFu);
        if (end > mapped || end < page_base)
            end = mapped;
        /* Never past the block: its neighbour belongs to someone else. */
        if (left && end - page_base > left)
            end = page_base + left;
        size = end - page_base;
        if (size)
            memset(XBOX_TO_NATIVE(page_base), 0, size);
        decommits++;
        decommit_bytes += size;
    }

    BRIDGE_MEM32(base_ptr) = page_base;
    BRIDGE_MEM32(size_ptr) = size;
    g_eax = 0;                                          /* STATUS_SUCCESS */

    /* Hot on a title with a growable heap (hundreds of decommits a second):
     * a running summary, not a line per call. */
    if (KERNEL_LOG_ON() || decommits + releases <= 4
            || ((decommits + releases) & 0x3FFFu) == 0) {
        fprintf(stderr, "  [KERNEL] NtFreeVirtualMemory: base=0x%08X size=%u "
                "type=0x%X (decommits=%lu/%llu KB releases=%lu)\n",
                page_base, size, free_type, decommits,
                (unsigned long long)(decommit_bytes / 1024), releases);
        fflush(stderr);
    }
}

static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);

    if (base_ptr && size_ptr) {
        uint32_t vm_base = BRIDGE_MEM32(base_ptr);
        uint32_t vm_size = BRIDGE_MEM32(size_ptr);
        uint32_t vm_status;

        if (guest_vmem_free(&vm_base, &vm_size, free_type, &vm_status)) {
            if (vm_status == 0) {
                BRIDGE_MEM32(base_ptr) = vm_base;
                BRIDGE_MEM32(size_ptr) = vm_size;
            }
            g_eax = vm_status;
            return;
        }
    }

    /* Memory NtAllocateVirtualMemory took from the guest heap. The call below
     * reads the 32-bit guest slots as host pointers and hands them to
     * VirtualFree, which fails, so none of it ever came back. Under
     * RECOMP_HEAP_RECLAIM a release returns the block to the heap and a
     * decommit zeroes the pages it names, since heap memory stays committed. */
    if (xbox_HeapReclaimEnabled() && base_ptr && size_ptr) {
        uint32_t vm_base = BRIDGE_MEM32(base_ptr);
        uint32_t left = xbox_HeapBlockSize(vm_base);

        if (left) {
            if (free_type & 0x8000) {              /* MEM_RELEASE */
                if (bridge_heap_interior(vm_base, left)) {
                    g_eax = BRIDGE_FREE_VM_NOT_AT_BASE;
                    return;
                }
                xbox_HeapFree(vm_base);
                /* NT writes back what it released: the base and the size. */
                BRIDGE_MEM32(size_ptr) = left;
            } else {
                /* MEM_DECOMMIT keeps the block, but on the console the pages
                 * are gone and a later MEM_COMMIT (a no-op here) brings them
                 * back zeroed. Zero them now so it does. */
                uint32_t vm_size = BRIDGE_MEM32(size_ptr);
                uint32_t n = (vm_size && vm_size < left) ? vm_size : left;
                uint32_t page_base = vm_base & ~0xFFFu;
                uint64_t end = ((uint64_t)vm_base + n + 0xFFFu) & ~(uint64_t)0xFFFu;
                /* NT decommits whole pages and writes back the rounded range;
                 * both stay inside the block (a block need not start on a
                 * page, and then its first page is shared). */
                if (end > (uint64_t)vm_base + left)
                    end = (uint64_t)vm_base + left;
                if (xbox_HeapBlockSize(page_base) != left + (vm_base - page_base))
                    page_base = vm_base;
                memset(XBOX_TO_NATIVE(page_base), 0, (size_t)(end - page_base));
                BRIDGE_MEM32(base_ptr) = page_base;
                BRIDGE_MEM32(size_ptr) = (uint32_t)(end - page_base);
            }
            g_eax = 0;
            return;
        }
    }

    bridge_free_guest_pages(base_ptr, size_ptr, free_type);
}

/* ── ExAllocatePool / ExAllocatePoolWithTag (ordinals 15, 16) ─
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
/* ExQueryNonVolatileSetting(ValueIndex, Type, Value, ValueLength, ResultLength)
 *
 * Titles read region, language and AV settings from EEPROM through this very
 * early in boot. Ordinal 24 was previously routed to bridge_ExQueryPoolBlockSize,
 * so the call returned a pool size where the game expected a settings blob. */
/* ── FscGetCacheSize (35) / FscSetCacheSize (37) ───────────
 *
 * The Xbox filesystem cache, sized in 4 KB pages. A title that streams from a
 * pack file resizes it around the work: Half-Life 2's pack scanner
 * (sub_0041E650) reads the current size, sets 1 MB for the scan, and puts the
 * old value back when it is done.
 *
 * There is no cache here -- reads go to the host filesystem, which has its
 * own -- so the size is only ever a number the title stores and restores.
 * Keeping it is still worth doing: unbridged, the getter returned 0, and a
 * title that saves that and restores it later is restoring a cache size of
 * zero pages. 64 KB is the console's own default.
 */
#define XBOX_FSCACHE_DEFAULT_PAGES 16u      /* 64 KB in 4 KB pages */

static uint32_t g_fscache_pages = XBOX_FSCACHE_DEFAULT_PAGES;

static void bridge_FscGetCacheSize(void)
{
    g_eax = g_fscache_pages;
}

static void bridge_FscSetCacheSize(void)
{
    uint32_t pages = STACK_ARG(0);

    /* The kernel rejects a request it cannot satisfy and leaves the current
     * size alone; the caller checks for a negative status. Nothing here can
     * fail, so accept it and remember what was asked for. */
    g_fscache_pages = pages;
    g_eax = STATUS_SUCCESS;
}

/* ── RtlCompareMemory (268) / RtlCompareMemoryUlong (269) ──
 *
 * Both answer "how far do these match", counted in bytes from the start, and
 * both are used to decide whether a buffer needs work rather than to do it.
 * Unbridged they returned 0, which reads as "differs at the first byte" -- the
 * safe-looking answer that is wrong whenever the caller is checking for a
 * region it can skip.
 *
 * RtlCompareMemoryUlong compares against a repeating ULONG and only ever
 * examines whole ULONGs, so a length that is not a multiple of four leaves the
 * remainder uncompared; the count it returns is still in bytes.
 */
static void bridge_RtlCompareMemory(void)
{
    uint32_t a_va   = STACK_ARG(0);
    uint32_t b_va   = STACK_ARG(1);
    uint32_t length = STACK_ARG(2);
    const uint8_t *a, *b;
    uint32_t i;

    if (!a_va || !b_va || !length) {
        g_eax = 0;
        return;
    }
    a = (const uint8_t *)XBOX_TO_NATIVE(a_va);
    b = (const uint8_t *)XBOX_TO_NATIVE(b_va);
    for (i = 0; i < length; i++) {
        if (a[i] != b[i])
            break;
    }
    g_eax = i;
}

static void bridge_RtlCompareMemoryUlong(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t length  = STACK_ARG(1);
    uint32_t pattern = STACK_ARG(2);
    uint32_t i;

    if (!base_va) {
        g_eax = 0;
        return;
    }
    length &= ~3u;                      /* whole ULONGs only */
    for (i = 0; i < length; i += 4) {
        if (BRIDGE_MEM32(base_va + i) != pattern)
            break;
    }
    g_eax = i;
}

static void bridge_ExQueryNonVolatileSetting(void)
{
    uint32_t value_index  = STACK_ARG(0);
    uint32_t type_va      = STACK_ARG(1);
    uint32_t value_va     = STACK_ARG(2);
    uint32_t value_length = STACK_ARG(3);
    uint32_t result_va    = STACK_ARG(4);

    NTSTATUS st = xbox_ExQueryNonVolatileSetting(
        value_index,
        type_va   ? (PULONG)&BRIDGE_MEM32(type_va)   : NULL,
        value_va  ? (PVOID)((uintptr_t)value_va + g_xbox_mem_offset) : NULL,
        value_length,
        result_va ? (PULONG)&BRIDGE_MEM32(result_va) : NULL);

    g_eax = (uint32_t)st;
}

/* HalReturnToFirmware(Routine) - the title asking to reboot or quit.
 *
 * It never returns on hardware. Returning here would let the game run on past
 * a decision to quit, which reads as a hang rather than an exit. */
static void bridge_HalReturnToFirmware(void)
{
    uint32_t routine = STACK_ARG(0);

    /* Routine 2 is a quick reboot, which on Xbox is how a title hands off to
     * another image: XLaunchNewImage fills the launch data page and reboots.
     * So "the title is exiting" and "the title is launching something" look
     * identical here, and the launch page is what tells them apart. */
    {
        uint32_t page = BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);

        if (page) {
            char path[520];
            uint32_t i;

            for (i = 0; i < sizeof(path) - 1; i++) {
                uint8_t c = BRIDGE_MEM8(page + 8 + i);
                if (!c) break;
                path[i] = (char)c;
            }
            path[i] = 0;
            fprintf(stderr, "  [KERNEL] launch data page 0x%08X:"
                            " type=%u titleid=0x%08X path='%s'\n",
                    page, BRIDGE_MEM32(page), BRIDGE_MEM32(page + 4), path);
            /* XapiBootToDash packs its reason and two parameters into the
             * front of the launch data, so this says why the title asked to
             * leave rather than merely that it did. */
            fprintf(stderr, "  [KERNEL]   launch data:");
            for (i = 0; i < 8; i++)
                fprintf(stderr, " %08X", BRIDGE_MEM32(page + 1024 + i * 4));
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  [KERNEL] no launch data page set\n");
        }
    }

    /* Who asked to quit.
     *
     * A title exiting looks identical whether it finished cleanly, hit an
     * error path, or was told to reboot -- and the routine number does not say
     * which. The guest call chain does. Same GS format tools/stackwalk.py
     * reads. */
    {
        const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
        uint32_t i;

        fprintf(stderr, "  [KERNEL] exit requested, guest esp=0x%08X:\n", g_esp);
        for (i = 0; i < 200; i++) {
            uint32_t a = g_esp + i * 4;
            if (a < 0x00010000u || a >= 0x04000000u) break;
            fprintf(stderr, "    GS %08X %08X\n", a,
                    *(const uint32_t *)(mem + a));
        }
        fflush(stderr);
    }

    xbox_PeekSample("exit peek");
    fprintf(stderr, "  [KERNEL] HalReturnToFirmware: routine=%u - title is leaving\n",
            routine);
    fflush(stderr);
    bridge_run_shutdown_notifications();   /* HalRegisterShutdownNotification (XBA-114) */

    /* Write the indirect-branch targets before the process goes away. This
     * path ends in ExitProcess, which does not run atexit handlers, so the
     * host's registered dump never fires -- and a title that gives up during
     * boot is exactly the one whose targets are worth having. No-op unless
     * RECOMP_ICALL_FEEDBACK is on. */
    RECOMP_ICALL_FEEDBACK_DUMP();

    /* The missing-file summary, for the same reason. On POSIX ExitProcess is
     * exit() and atexit runs after all; the summary prints once either way. */
    xbox_missing_summary("firmware");

    /* Let a host-played FMV finish before the process goes away.
     *
     * The title is not the one presenting it, so it has no reason to wait --
     * it opens the file, carries on, and quits, which would kill the video
     * thread part-way through a five-second clip. Waiting here is what makes
     * the clip actually watchable, and it costs nothing when no video is
     * playing. Bounded, so a stuck player cannot stop the process exiting. */
    {
        extern int xbox_VideoIsPlaying(void);
        int waited = 0;

        while (xbox_VideoIsPlaying() && waited < 60000) {
            Sleep(50);
            waited += 50;
        }
        if (waited)
            fprintf(stderr, "  [KERNEL] waited %dms for the video to finish\n",
                    waited);
    }

    /* Routine 2 with a page naming this title's own image: a restart, not an
     * exit (kernel_reboot.c). Fable's Pause -> Quit -> Confirm goes back to
     * its main menu this way. Another image of the disc is handed over to
     * when the build has it (XBA-82). An empty path is XapiBootToDash: the
     * dashboard, nothing to hand over to. Whatever is left ends the run, with
     * a reason the Android launcher shows (xbox_title_exit). */
    {
        uint32_t page = BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);
        char path[520] = "";
        char why[640];

        if (page) {
            uint32_t i;

            for (i = 0; i < sizeof(path) - 1; i++) {
                path[i] = (char)BRIDGE_MEM8(page + 8 + i);
                if (!path[i]) break;
            }
            path[i] = 0;
        }
        if (routine == 2 && page && path[0]) {
            if (xbox_launch_path_is_image(path, xbox_XeImageFileName.Buffer))
                xbox_quick_reboot((const uint8_t *)((uintptr_t)page +
                                                    g_xbox_mem_offset));
            else if (xbox_launch_image(path, (const uint8_t *)((uintptr_t)page + g_xbox_mem_offset)) != 0) {
                fprintf(stderr, "  [KERNEL] launch of '%s' is not this title"
                                " (%s): exiting\n", path,
                        xbox_XeImageFileName.Buffer);
                /* A disc that hands over to another image needs XBA-82; the
                 * compat runner names it from this (XBA-62). */
                recomp_discovery_launch(path);
            }
        }
        /* The reason. XapiBootToDash's own reason (the first launch-data
         * dword) only with an empty path: anything else's launch data is the
         * title's. A path matters only for routine 2 (a launch), and a
         * refused or failed restart into this image reads as the dashboard:
         * there is nothing better to say. */
        if (!page)
            xbox_exit_reason(why, sizeof why, 0, routine, NULL, 0);
        else if (!path[0])
            xbox_exit_reason(why, sizeof why, 0, routine, "",
                             BRIDGE_MEM32(page + 1024));
        else if (routine == 2 &&
                 !xbox_launch_path_is_image(path, xbox_XeImageFileName.Buffer))
            xbox_exit_reason(why, sizeof why, 0, routine, path, 0);
        else
            xbox_exit_reason(why, sizeof why, 0, routine, NULL, 0);
        xbox_HalReturnToFirmwareWhy(routine, why);
    }
}

static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── KfRaiseIrql / KfLowerIrql (ordinals 160, 161) ────── */
static void bridge_KfRaiseIrql(void)
{
    uint32_t new_irql = g_ecx; /* fastcall: KIRQL is passed in CL */
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)new_irql);
}

static void bridge_KfLowerIrql(void)
{
    uint32_t new_irql = g_ecx; /* fastcall: KIRQL is passed in CL */
    xbox_KfLowerIrql((UCHAR)new_irql);
    g_eax = 0;
}

/* ── KeRaiseIrqlToDpcLevel (ordinal 129) ─────────────────── */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

/* ── RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) ─ */
static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* ── KeQueryPerformanceCounter / Frequency (ordinals 126, 127) ─ */
static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

/* ── KeQuerySystemTime (ordinal 128) ─────────────────────── */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

/* ── MmQueryStatistics (ordinal 181) ─────────────────────── */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

/* ── NtCreateEvent (ordinal 189) ─────────────────────────── */
static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    /* Use local HANDLE to avoid 8-byte write to 4-byte Xbox memory slot.
     * On x64, HANDLE is 8 bytes but Xbox expects 4-byte handles. */
    HANDLE local_handle = NULL;
    NTSTATUS status = xbox_NtCreateEvent(
        &local_handle,
        XBOX_TO_NATIVE(obj_attr_ptr),
        event_type, initial_state);

    if (handle_ptr) {
        bridge_write_handle(handle_ptr, local_handle);
    }

    fprintf(stderr, "  [BRIDGE] NtCreateEvent: handle_ptr=0x%08X type=%u init=%u → status=0x%08X handle=0x%08X\n",
            handle_ptr, event_type, initial_state, (uint32_t)status,
            (uint32_t)(uintptr_t)local_handle);

    g_eax = (uint32_t)status;
}

static HANDLE ke_shadow_lookup(uint32_t guest_va);
static void ke_shadow_insert(uint32_t guest_va, HANDLE host);
static void ke_shadow_note_header(uint32_t guest_va);
static HANDLE bridge_resolve_handle(uint32_t token);
static HANDLE ke_guest_event(uint32_t guest_va, int *type);
static int ke_promote(uint32_t guest_va);
static void ke_note_set_attempt(uint32_t guest_va);
static int ke_set_seen(uint32_t guest_va);
static int ke_is_promoted(uint32_t guest_va);
static int ke_title_kevents_on(void);
static void ke_shadow_remove(uint32_t guest_va);

/* Wait accounting (kernel_wait_stats.h): a zero timeout is a poll, not timed. */
static int bridge_timeout_is_poll(uint32_t timeout_ptr)
{
    return timeout_ptr && !(BRIDGE_MEM32(timeout_ptr) | BRIDGE_MEM32(timeout_ptr + 4));
}

/* Bring a header-owned host event in line with guest SignalState before a
 * wait. Explicitly initialized shadows keep their existing host-state policy. */
static HANDLE ke_guest_event_prepare(uint32_t guest_va, int *type)
{
    HANDLE h = ke_guest_event(guest_va, type);
    if (h) {
        if (BRIDGE_MEM32(guest_va + 4) == 0) {
            ResetEvent(h);
            if (BRIDGE_MEM32(guest_va + 4) != 0) /* set meanwhile */
                SetEvent(h);
        } else {
            SetEvent(h);
        }
    }
    return h;
}

/* A set or reset on an event the title built itself that is not a guest event
 * (RECOMP_TITLE_KEVENTS=0, not promoted yet): no host event to signal, but
 * SignalState in guest memory is written as the console's KeSetEvent /
 * KeResetEvent would. Otherwise a set that came before the wait-spin detector
 * promoted the object (XBA-93) was lost: the promoted wait then blocked for
 * good (Fable's fence event, method 5 arriving while BlockOnTime spun). The
 * host event a promotion creates starts from SignalState, and
 * ke_guest_event_prepare re-reads it before each wait. Only for a readable
 * event header with no KeInitializeEvent shadow. */
static void ke_lost_signal(uint32_t guest_va, uint32_t state)
{
    uint32_t hdr;
    if (guest_va < 0x10000u || (guest_va & 3u))
        return;
    if (guest_va >= 0x04000000u && (guest_va < 0x80000000u || guest_va >= 0x84000000u))
        return;
    hdr = BRIDGE_MEM32(guest_va);
    if (((hdr & 0xFFu) != 0 && (hdr & 0xFFu) != 1) || ((hdr >> 16) & 0xFFu) != 4)
        return;
    if (ke_shadow_lookup(guest_va))
        return;
    BRIDGE_MEM32(guest_va + 4) = state;
}

/* Set a KEVENT the title built itself, from a host thread: a device model
 * standing in for the interrupt that calls KeSetEvent on the console (D3D's
 * software method 5, for one). Same effect as the KeSetEvent bridge on such an
 * event. Returns 0 when the object is not a guest event (RECOMP_TITLE_KEVENTS=0
 * and not promoted or adopted, a KeInitializeEvent shadow, a bad header); an
 * event header with no shadow still gets SignalState 1 (ke_lost_signal). */
int xbox_KeSetGuestEvent(uint32_t guest_va)
{
    int type;
    HANDLE ge = ke_guest_event(guest_va, &type);

    if (!ge) {
        ke_note_set_attempt(guest_va);   /* a wait spin on it may be promoted (XBA-93) */
        ke_lost_signal(guest_va, 1);
        return 0;
    }
    xbox_wait_stats_set(guest_va);
    BRIDGE_MEM32(guest_va + 4) = 1;
    SetEvent(ge);
    return 1;
}

/* Treat one KEVENT the title builds itself as a guest event from now on, as
 * RECOMP_TITLE_KEVENTS does for every object and the wait-spin detector does
 * for one it caught spinning (XBA-93): for a device model that knows the
 * event and sets it (the XDK D3D layer's fence event, once it has found the
 * device at run time). The header is checked at each use, as for any guest
 * event. Returns 1 when adopted (or already), 0 when the table is full. */
int xbox_KeAdoptGuestEvent(uint32_t guest_va)
{
    if (ke_is_promoted(guest_va))
        return 1;
    if (!ke_promote(guest_va))
        return 0;
    fprintf(stderr, "  [KERNEL] event 0x%08X adopted as a guest event (its setter is modelled)\n", guest_va);
    fflush(stderr);
    return 1;
}

/* ── KeSetEvent (ordinal 145) ────────────────────────────── */
static void bridge_KeSetEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    HANDLE h;

    (void)increment;
    (void)wait;

    xbox_wait_stats_set(guest_va);
    {   /* An event the title built itself (see ke_guest_event). */
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            g_eax = BRIDGE_MEM32(guest_va + 4) != 0;   /* previous state */
            BRIDGE_MEM32(guest_va + 4) = 1;
            SetEvent(ge);
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (!h) {
        ke_note_set_attempt(guest_va);   /* no host event: a wait spin on it may be promoted (XBA-93) */
        ke_lost_signal(guest_va, 1);     /* kept for that promotion */
    }
    if (!h)
        h = bridge_resolve_handle(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        g_eax = (uint32_t)SetEvent(h);
    else
        g_eax = 0;
}

/* ── KeWaitForSingleObject (ordinal 159) ─────────────────── */
/* ── Wait spins (XBA-93) ─────────────────────────────────────
 *
 * A thread that waits on the same object thousands of times a second is
 * stuck: the wait either fails at once or times out at once, and nothing ever
 * signals the object. Black's worker did it ~500,000 times a second. Seen
 * causes: an event the title built by writing its header (no
 * KeInitializeEvent), which without RECOMP_TITLE_KEVENTS reaches the host as a
 * bad handle and fails immediately; and an object only the hardware would
 * signal (a GPU interrupt, a DPC) that the runtime does not deliver.
 *
 * After WAIT_SPIN_CALLS consecutive waits on one object within
 * WAIT_SPIN_MS, it is reported once: the object, its dispatcher header, the
 * wait's result, the caller's return address and a frame walk, in the log and
 * in the discovery file ({"t":"wait_spin"}). An event header whose waits
 * failed (not timed out) is then promoted to a guest-built event, which is
 * what RECOMP_TITLE_KEVENTS does for every object, but only once something
 * has tried to set it (KeSetEvent, a software method 5): promoted with no
 * setter, a wait that used to fail and re-poll would block for ever (Fable's
 * fence wait, whose method 5 its profile does not service, would hang).
 * The check runs at every spin window, so a setter seen later still counts.
 * RECOMP_WAIT_SPIN=0 turns the detector off; =promote promotes without the
 * evidence (bring-up). */
#define WAIT_SPIN_CALLS 2000u
#define WAIT_SPIN_MS    1000u

typedef struct {
    uint32_t object;
    uint32_t calls;
    DWORD    t0;
    uint32_t reported;          /* last object reported by this thread */
} WaitSpin;
static XBOX_THREAD_LOCAL WaitSpin t_wait_spin;

static const char *dispatcher_type_name(uint32_t type)
{
    switch (type) {
    case 0: return "notification event";
    case 1: return "synchronization event";
    case 2: return "mutant";
    case 3: return "process";
    case 4: return "queue";
    case 5: return "semaphore";
    case 6: return "thread";
    case 8: return "notification timer";
    case 9: return "synchronization timer";
    default: return "unknown";
    }
}

static void bridge_wait_spin(uint32_t object, uint32_t status, uint32_t timeout_ptr, uint32_t ret_va)
{
    static int enabled = -1, promote_always;
    WaitSpin *w = &t_wait_spin;
    DWORD now;

    if (enabled < 0) {
        const char *e = getenv("RECOMP_WAIT_SPIN");
        enabled = !(e && *e == '0');
        promote_always = e && !strcmp(e, "promote");
    }
    if (!enabled)
        return;
    now = GetTickCount();
    if (object != w->object || now - w->t0 > WAIT_SPIN_MS) {
        w->object = object;
        w->calls = 0;
        w->t0 = now;
    }
    if (++w->calls != WAIT_SPIN_CALLS)
        return;
    w->calls = 0;               /* a new window: the setter check runs again */
    w->t0 = now;
    {
        int readable = object >= 0x10000u && !(object & 3u)
                    && (object < 0x04000000u || (object >= 0x80000000u && object < 0x84000000u));
        uint32_t hdr = readable ? BRIDGE_MEM32(object) : 0;
        uint32_t type = hdr & 0xFFu, size = (hdr >> 16) & 0xFFu;
        uint32_t signal = readable ? BRIDGE_MEM32(object + 4) : 0;
        uint32_t frames[4] = { 0, 0, 0, 0 }, ebp = g_ebp;
        int k, failed = status != 0 && status != (uint32_t)STATUS_TIMEOUT
                     && status != 0x101u && status != 0xC0u;
        const char *timeout = !timeout_ptr ? "none"
                            : (BRIDGE_MEM32(timeout_ptr) | BRIDGE_MEM32(timeout_ptr + 4)) ? "set" : "zero";
        int promotable = failed && readable && (type == 0 || type == 1) && size == 4;
        if (w->reported == object) {
            /* Reported already: only a setter seen since can change things. */
            if (promotable && !ke_is_promoted(object) && (promote_always || ke_set_seen(object))
                    && ke_promote(object)) {
                fprintf(stderr, "  [KERNEL] wait spin: 0x%08X now has a setter: promoted to a guest event\n",
                        object);
                fflush(stderr);
            }
            return;
        }
        w->reported = object;
        for (k = 0; k < 4 && ebp >= 0x10000u && ebp < 0x04000000u && !(ebp & 3u); k++) {
            frames[k] = BRIDGE_MEM32(ebp + 4);
            ebp = BRIDGE_MEM32(ebp);
        }
        fprintf(stderr, "  [KERNEL] wait spin: %u waits in under %u ms on 0x%08X (%s, header 0x%08X, size %u, "
                        "signal %u), status 0x%08X, timeout %s, from 0x%08X; frames 0x%08X 0x%08X 0x%08X 0x%08X\n",
                WAIT_SPIN_CALLS, WAIT_SPIN_MS, object, readable ? dispatcher_type_name(type) : "not guest memory",
                hdr, size, signal, status, timeout, ret_va, frames[0], frames[1], frames[2], frames[3]);
        recomp_discovery_wait_spin(object, ret_va, hdr, status);
        if (promotable && (promote_always || ke_set_seen(object)) && ke_promote(object))
            fprintf(stderr, "  [KERNEL] wait spin: 0x%08X looks like an event the title built itself and "
                            "something sets it: promoted to a guest event (RECOMP_TITLE_KEVENTS for this "
                            "object)\n", object);
        else if (promotable)
            fprintf(stderr, "  [KERNEL] wait spin: 0x%08X looks like an event the title built itself, but "
                            "nothing has tried to set it yet: left as it is\n", object);
        fflush(stderr);
    }
}

/* ── A timed wait served before it times out (WP-2, kernel.h) ──
 *
 * A timed wait that runs out gives the device models one pass first
 * (xbox_IrqServiceTimedWait: catch-up hook, posted interrupts, DPCs, on this
 * thread), then the object is tested again with a zero timeout. On the
 * console a USB control transfer retires within a frame; here the
 * controller thread can be starved by the host or held by a save state's
 * capture past XAPI's 50 ms, and a timeout that is the host's fault made
 * KOTOR cancel the transfer and drop its pad. Only a non-zero timeout:
 * a poll must stay a poll, and an untimed wait never times out. */
static volatile LONG s_wait_served;

static int wait_timeout_is_timed(PLARGE_INTEGER timeout)
{
    return timeout && timeout->QuadPart != 0;
}

static void wait_served_note(uint32_t object, uint32_t ret_va)
{
    LONG n = InterlockedIncrement(&s_wait_served);
    if (n <= 3 || (n & (n - 1)) == 0) {
        fprintf(stderr, "  [KERNEL] timed wait on 0x%08X from 0x%08X timed out; "
                        "signalled after a device service pass (%ld so far)\n",
                object, ret_va, (long)n);
        fflush(stderr);
    }
}

NTSTATUS xbox_KeWaitServiced(PVOID Object, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
{
    NTSTATUS st = xbox_KeWaitForSingleObject(Object, 0, 0, Alertable, Timeout);
    if (st == STATUS_TIMEOUT && wait_timeout_is_timed(Timeout) && xbox_IrqServiceTimedWait()) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        st = xbox_KeWaitForSingleObject(Object, 0, 0, Alertable, &zero);
    }
    return st;
}

/* One wait on a guest object, as the bridge resolves it. Returns the status;
 * a successful wait consumes a guest-built synchronization event. */
static NTSTATUS bridge_ke_wait_one(uint32_t object, uint32_t wait_reason, uint32_t wait_mode,
                                   uint32_t alertable, PLARGE_INTEGER timeout)
{
    NTSTATUS st;
    int type;
    HANDLE h = ke_guest_event_prepare(object, &type);

    if (h) {    /* An event the title built itself (see ke_guest_event): its
                 * SignalState in guest memory is the truth -- XDK code resets
                 * it by writing 0 there -- so the host event was brought in
                 * line first. */
        st = xbox_KeWaitForSingleObject(h, wait_reason, wait_mode, (BOOLEAN)alertable, timeout);
        if (st == 0 && type == 1)               /* synchronization: consumed */
            BRIDGE_MEM32(object + 4) = 0;
        return st;
    }
    h = ke_shadow_lookup(object);
    if (!h)
        h = bridge_resolve_handle(object);
    if (!h)
        h = XBOX_TO_NATIVE(object);
    return xbox_KeWaitForSingleObject(h, wait_reason, wait_mode, (BOOLEAN)alertable, timeout);
}

static void bridge_KeWaitForSingleObject(void)
{
    uint32_t object = STACK_ARG(0);
    uint32_t wait_reason = STACK_ARG(1);
    uint32_t wait_mode = STACK_ARG(2);
    uint32_t alertable = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);
    uint32_t ret_va = BRIDGE_MEM32(g_esp - 4);   /* the call's return address */
    uint64_t t0 = xbox_wait_stats_begin(bridge_timeout_is_poll(timeout_ptr));
    PLARGE_INTEGER timeout = XBOX_TO_NATIVE(timeout_ptr);
    NTSTATUS st;

    if (bridge_run_io_apcs(alertable, wait_mode))
        return;
    st = bridge_ke_wait_one(object, wait_reason, wait_mode, alertable, timeout);
    if (st == STATUS_TIMEOUT && wait_timeout_is_timed(timeout) && xbox_IrqServiceTimedWait()) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        st = bridge_ke_wait_one(object, wait_reason, wait_mode, alertable, &zero);
        if (st == 0)
            wait_served_note(object, ret_va);
    }
    g_eax = (uint32_t)st;
    bridge_wait_done(alertable, wait_mode);
    xbox_wait_stats_wait(object, ret_va, g_eax, t0);
    if (g_eax != 0)
        bridge_wait_spin(object, g_eax, timeout_ptr, ret_va);
}

/* ── NtWaitForSingleObject (ordinal 233) ─────────────────── */
/*
 * The synchronous sibling of ...Ex. Halo's synchronous ReadFile issues the read
 * and then waits on its completion event through this; unbridged it fell to the
 * "return 0" default (STATUS_SUCCESS = "already signalled"), so the read handshake
 * completed before the data arrived and the UI-map precache never made progress.
 */
static void bridge_NtWaitForSingleObject(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t alertable   = STACK_ARG(1);
    uint32_t timeout_ptr = STACK_ARG(2);
    uint32_t token       = STACK_ARG(0);
    uint32_t site        = g_xbox_kernel_caller;
    uint64_t t0;

    if (bridge_run_io_apcs(alertable, 1))
        return;
    t0 = xbox_wait_stats_begin(bridge_timeout_is_poll(timeout_ptr));
    g_eax = (uint32_t)xbox_NtWaitForSingleObject(
        handle, (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
    bridge_wait_done(alertable, 1);
    xbox_wait_stats_wait(token, site, g_eax, t0);
}

/* ── NtClearEvent (ordinal 186) ──────────────────────────── */
/* Resets an event to non-signalled. Halo clears the read-completion event
 * before each async map read; a no-op here left the event stuck signalled. */
static void bridge_NtClearEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    g_eax = (uint32_t)xbox_NtClearEvent(handle);
}

/* ── NtSetEvent (ordinal 225) ────────────────────────────── */
/* Signals an event and optionally returns its previous state. Unbridged it
 * no-op'd, so a producer's "work ready" signal never landed -- Halo's map-copy
 * worker thread then slept forever in WaitForSingleObject on the decompress
 * context's go-event and only the first 14 KB of the map ever loaded. */
static void bridge_NtSetEvent(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t prev   = STACK_ARG(1);
    xbox_wait_stats_set(STACK_ARG(0));
    g_eax = (uint32_t)xbox_NtSetEvent(handle, XBOX_TO_NATIVE(prev));
}

/* ── NtPulseEvent (ordinal 205) ──────────────────────────── */
/* Signal-then-reset: releases threads currently waiting, then leaves the event
 * non-signalled. Same unbridged-no-op hazard as NtSetEvent in the map-load
 * handoff chain. PulseEvent carries the (deprecated, lossy) Xbox semantics
 * faithfully -- a waiter not yet blocked misses it, exactly as on hardware. */
static void bridge_NtPulseEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    xbox_wait_stats_set(STACK_ARG(0));
    if (handle) PulseEvent(handle);
    g_eax = 0;
}

/* ── NtWaitForSingleObjectEx (ordinal 234) ───────────────── */
/*
 * Unbridged, this fell through to the "no bridge, returning 0" default -- and 0
 * is STATUS_SUCCESS, so every wait returned instantly as though the object were
 * already signalled. Halo's main loop then spun: 91 million calls in 100
 * seconds, no blocking, no progress. A wait that always succeeds is worse than
 * one that always fails, because it looks like the game is running.
 */
static HANDLE bridge_resolve_handle(uint32_t token);

static void bridge_NtWaitForSingleObjectEx(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t wait_mode   = STACK_ARG(1);
    uint32_t alertable   = STACK_ARG(2);
    uint32_t timeout_ptr = STACK_ARG(3);

    static int logged = 0;
    if (logged++ < 20) {
        fprintf(stderr, "  [KERNEL] NtWaitForSingleObjectEx: token=0x%08X "
                "handle=%p timeout=%s\n",
                STACK_ARG(0), handle, timeout_ptr ? "finite" : "INFINITE");
        fflush(stderr);
    }

    if (bridge_run_io_apcs(alertable, wait_mode))
        return;
    {
        uint32_t token = STACK_ARG(0), site = g_xbox_kernel_caller;
        uint64_t t0 = xbox_wait_stats_begin(bridge_timeout_is_poll(timeout_ptr));
        g_eax = (uint32_t)xbox_NtWaitForSingleObjectEx(
            handle, (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
            XBOX_TO_NATIVE(timeout_ptr));
        bridge_wait_done(alertable, wait_mode);
        xbox_wait_stats_wait(token, site, g_eax, t0);
    }
}

/* ── MmQueryAddressProtect (ordinal 179) ─────────────────── */
/* NtWaitForMultipleObjectsEx (ordinal 235, 6 args = 24 bytes)
 *
 * NTSTATUS NtWaitForMultipleObjectsEx(ULONG Count, HANDLE *Handles,
 *                                     WAIT_TYPE WaitType,
 *                                     KPROCESSOR_MODE WaitMode,
 *                                     BOOLEAN Alertable,
 *                                     PLARGE_INTEGER Timeout);
 *
 * Six, not five: WaitMode sits between WaitType and Alertable. Half-Life 2's
 * own call site settles it -- sub_0059BE0F pushes six dwords before the
 * thunk (esi, eax, ebx, 1, [ebp+0x18], edi) and the callee is expected to pop
 * them all.
 *
 * Getting it wrong cost 4 bytes of guest stack per call and shifted every
 * argument after WaitType, so the wait read Alertable as its timeout pointer
 * and reported INFINITE for every wait. The leak was invisible to the esp
 * invariant because sub_0059BE0F restores esp with `leave`: its own frame
 * came back correct while `pop edi; pop esi; pop ebx` took their values one
 * slot out, so the caller's `this` -- kept in ebx by sub_005ACDC0 -- came
 * back holding what esi had, and the next [ebx+0x94] read a string as a
 * pointer.
 *
 * xbox_NtWaitForMultipleObjectsEx has been in kernel_sync.c all along; only
 * the bridge wrapper was missing, so the thunk fell through to the fallback
 * and returned 0 -- STATUS_SUCCESS, meaning 'object 0 is signalled'. A wait
 * that always reports signalled turns a blocking wait into a busy loop, which
 * is exactly what Half-Life 2 does after spawning its first worker: the main
 * thread spins in a CUtlLinkedList walk making no indirect calls at all.
 *
 * Handles is a guest array of tokens, so each has to be resolved
 * individually -- the array cannot just be pointed at. Bounded because a
 * bogus Count would otherwise read arbitrary guest memory onto the stack;
 * MAXIMUM_WAIT_OBJECTS is the real kernel's own limit.
 */
static void bridge_NtWaitForMultipleObjectsEx(void)
{
    uint32_t count       = STACK_ARG(0);
    uint32_t handles_va  = STACK_ARG(1);
    uint32_t wait_type   = STACK_ARG(2);
    uint32_t wait_mode   = STACK_ARG(3);   /* KernelMode / UserMode */
    uint32_t alertable   = STACK_ARG(4);
    uint32_t timeout_ptr = STACK_ARG(5);

    HANDLE   handles[MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || !handles_va) {
        g_eax = 0xC000000Du;             /* STATUS_INVALID_PARAMETER */
        return;
    }
    for (i = 0; i < count; i++)
        handles[i] = bridge_resolve_handle(BRIDGE_MEM32(handles_va + i * 4));

    {
        static int logged;
        if (logged++ < 20) {
            fprintf(stderr, "  [KERNEL] NtWaitForMultipleObjectsEx: count=%u type=%u timeout=%s\n",
                    count, wait_type, timeout_ptr ? "finite" : "INFINITE");
            for (i = 0; i < count; i++)
                fprintf(stderr, "      [%u] token=0x%08X host=%p\n", i,
                        BRIDGE_MEM32(handles_va + i * 4), handles[i]);
            fflush(stderr);
        }
    }

    if (bridge_run_io_apcs(alertable, wait_mode))
        return;
    {   /* Filed under the first object (kernel_wait_stats.h). */
        uint32_t site = g_xbox_kernel_caller;
        uint64_t t0 = xbox_wait_stats_begin(bridge_timeout_is_poll(timeout_ptr));
        g_eax = (uint32_t)xbox_NtWaitForMultipleObjectsEx(
            count, handles, wait_type, (BOOLEAN)alertable,
            XBOX_TO_NATIVE(timeout_ptr));
        bridge_wait_done(alertable, wait_mode);
        xbox_wait_stats_wait_multi(BRIDGE_MEM32(handles_va), site, g_eax, t0, count, wait_type == 0);
    }
}

/*
 * Takes an Xbox VA, so the native pointer has to be formed before the query --
 * an unbridged 0 return reads as PAGE_NOACCESS. Halo walks all 22 MB of its
 * physical memory map asserting every page is PAGE_READWRITE
 * (physical_memory_map.c:77), so a zero here stops startup on the first page.
 */
static void bridge_MmQueryAddressProtect(void)
{
    uint32_t address = STACK_ARG(0);

    g_eax = address ? (uint32_t)xbox_MmQueryAddressProtect(XBOX_TO_NATIVE(address))
                    : 0;
}

/* ── NtUserIoApcDispatcher (ordinal 232) ─────────────────── */
/*
 * The kernel side of XAPI's ReadFileEx/WriteFileEx. XAPI passes *this* as the
 * ApcRoutine to NtReadFile and puts the title's completion routine in
 * ApcContext, so the dispatcher's only job is to call it with Win32 argument
 * shape:
 *
 *   VOID CALLBACK Completion(DWORD dwErrorCode,
 *                            DWORD dwNumberOfBytesTransfered,
 *                            LPOVERLAPPED lpOverlapped)   // __stdcall, ret 12
 *
 * lpOverlapped is the IO_STATUS_BLOCK pointer: an NT OVERLAPPED begins with
 * Internal/InternalHigh, which is exactly a IO_STATUS_BLOCK, so the title's
 * OVERLAPPED and the block it handed to NtReadFile are the same address.
 * Halo's cache_files_windows completion relies on that -- it reads its own
 * field at lpOverlapped+0x10 and sets the flag the setup loop polls.
 */
/* Guest code a kernel call runs on the caller's own thread (an APC, an
 * inline DPC, a synchronize routine) writes the register globals the guest
 * function that made the call is still using. A callee that keeps the ABI
 * leaves ebx/esi/edi alone and pops its own arguments, but nothing checks
 * that, and the failure is intermittent: Darkwatch's loader resumed with an
 * APC's RDTSC value in esi and faulted (@ranzo312-cyber, #127). So each
 * such site puts them back, eax too unless the routine's result is the
 * call's own. */
typedef struct { uint32_t eax, ecx, edx, ebx, esi, edi, esp; } CallbackRegs;

static void callback_regs_save(CallbackRegs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx;
    r->ebx = g_ebx; r->esi = g_esi; r->edi = g_edi; r->esp = g_esp;
}

static void callback_regs_restore(const CallbackRegs *r, int keep_eax)
{
    if (!keep_eax)
        g_eax = r->eax;
    g_ecx = r->ecx; g_edx = r->edx;
    g_ebx = r->ebx; g_esi = r->esi; g_edi = r->edi; g_esp = r->esp;
}

static void bridge_NtUserIoApcDispatcher(void)
{
    uint32_t apc_context = STACK_ARG(0);
    uint32_t iostatus    = STACK_ARG(1);
    uint32_t status      = iostatus ? BRIDGE_MEM32(iostatus) : 0;
    uint32_t information = iostatus ? BRIDGE_MEM32(iostatus + 4) : 0;
    recomp_func_t fn;

    fn = recomp_lookup(apc_context);
    if (!fn) fn = recomp_lookup_manual(apc_context);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] NtUserIoApcDispatcher: completion routine "
                "0x%08X not in dispatch\n", apc_context);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* __stdcall, right-to-left. The callee's `ret 12` consumes the dummy
     * return address and all three arguments, so g_esp needs no fixup here. */
    CallbackRegs saved;
    callback_regs_save(&saved);
    g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = information;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = (status == 0) ? 0 : status;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    {
        /* An APC runs in its thread's context: guest code on a guest thread,
         * so it holds the guest CPU this call let go of. */
        int took = xbox_GuestCpuEnterCallback();
        fn();
        xbox_GuestCpuLeaveCallback(took);
    }
    callback_regs_restore(&saved, 0);

    g_eax = 0;
}

/* ── KeDelayExecutionThread (ordinal 99) ─────────────────── */

/* Sleeps a title profile shortens (xbox_SetDelayOverride).
 *
 * A title whose loop is `draw; Sleep(50)` caps itself at 20 fps by design.
 * On the console the draw is a few milliseconds; here it can be most of a
 * frame, and the fixed sleep comes on top of it. A profile can name the
 * call: the return address the title's `call Sleep` pushes, and the
 * milliseconds it passes. The XAPI Sleep(ms) -> SleepEx -> this thunk chain
 * leaves both on the guest stack, the return address right below the pushed
 * argument, a few frames above the thunk's own arguments; so a matching
 * pair within the top 64 words of the stack, and an interval of exactly
 * that many milliseconds, identify the call. Nothing else is touched, and
 * with no override registered the scan never runs. */
#define DELAY_OVERRIDES_MAX 8
static struct { uint32_t ret_va, from_ms, to_ms; } s_delay_ovr[DELAY_OVERRIDES_MAX];
static int s_delay_novr;

void xbox_SetDelayOverride(uint32_t ret_va, uint32_t from_ms, uint32_t to_ms)
{
    if (s_delay_novr < DELAY_OVERRIDES_MAX) {
        s_delay_ovr[s_delay_novr].ret_va = ret_va;
        s_delay_ovr[s_delay_novr].from_ms = from_ms;
        s_delay_ovr[s_delay_novr].to_ms = to_ms;
        s_delay_novr++;
    }
}

/* The override for this delay, in ms, or -1. */
static int delay_override(uint32_t interval_ptr)
{
    int64_t interval;
    uint32_t w;
    int i;

    if (!s_delay_novr || !interval_ptr)
        return -1;
    memcpy(&interval, XBOX_TO_NATIVE(interval_ptr), sizeof interval);
    for (i = 0; i < s_delay_novr; i++) {
        if (interval != -(int64_t)s_delay_ovr[i].from_ms * 10000)
            continue;
        for (w = 0; w < 64; w++)
            if (BRIDGE_MEM32(g_esp + w * 4) == s_delay_ovr[i].ret_va
                && BRIDGE_MEM32(g_esp + w * 4 + 4) == s_delay_ovr[i].from_ms) {
                static uint32_t told;           /* one line per override */
                if (!(told & (1u << i))) {
                    told |= 1u << i;
                    fprintf(stderr, "  [KERNEL] Sleep(%u) returning to 0x%08X"
                            " sleeps %u ms (title profile)\n",
                            s_delay_ovr[i].from_ms, s_delay_ovr[i].ret_va,
                            s_delay_ovr[i].to_ms);
                }
                return (int)s_delay_ovr[i].to_ms;
            }
    }
    return -1;
}

/* Unbridged this returned instantly, turning every "sleep and retry" in the
 * title into a hot spin. Halo's cache-partition setup retries this way. */
static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode    = STACK_ARG(0);
    uint32_t alertable    = STACK_ARG(1);
    uint32_t interval_ptr = STACK_ARG(2);
    int ovr;


    if (bridge_run_io_apcs(alertable, wait_mode))
        return;
    if ((ovr = delay_override(interval_ptr)) >= 0) {
        LARGE_INTEGER shorter;
        shorter.QuadPart = -(LONGLONG)ovr * 10000;
        g_eax = (uint32_t)xbox_KeDelayExecutionThread(
            (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable, &shorter);
        bridge_wait_done(alertable, wait_mode);
        return;
    }
    g_eax = (uint32_t)xbox_KeDelayExecutionThread(
        (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(interval_ptr));
    bridge_wait_done(alertable, wait_mode);
}

/* ── KeBugCheck (ordinal 95) / KeBugCheckEx (96) ─────────── */
/*
 * The title asking the kernel to die; its CRT's abort() is KeBugCheck(0).
 * Neither returns on the console, and neither does it here: returning ran the
 * code the bug check was there to stop (Yu-Gi-Oh: 2.9 M bug checks in 40 s,
 * then a SIGILL deep in the image). xbox_BugCheckStop names the code, the
 * parameters and the call site, and ends the run like a title that quits.
 */
static void bridge_KeBugCheck(void)
{
    xbox_BugCheckStop(0, g_xbox_kernel_caller, STACK_ARG(0), 0, 0, 0, 0);
}

static void bridge_KeBugCheckEx(void)
{
    xbox_BugCheckStop(1, g_xbox_kernel_caller, STACK_ARG(0), STACK_ARG(1),
                      STACK_ARG(2), STACK_ARG(3), STACK_ARG(4));
}

/* ── NtYieldExecution (ordinal 238) ──────────────────────── */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* ── MmGetPhysicalAddress (ordinal 173) ──────────────────── */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Calls the same xbox_* the thunk table exposes, so there is one
     * implementation rather than two that have to be kept in agreement.
     * Note this bridge itself is not covered by any test: bridge functions
     * are static and driven by guest CPU state, and nothing in tests/ can
     * reach them. */
    g_eax = (uint32_t)xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)addr);
}

/* ── MmSetAddressProtect (ordinal 182) ───────────────────── */
static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* ── AvSetDisplayMode (ordinal 3) ────────────────────────── */
static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    /* The framebuffer the display is meant to scan out, and the format it is
     * in. This is the only place the address is stated: the title never writes
     * PCRTC_START itself, so without this there is nothing that says where the
     * guest believes its picture is. */
    fprintf(stderr, "  [AV] SetDisplayMode mode=0x%08X format=0x%08X"
                    " pitch=%u fb=0x%08X\n", mode, format, pitch, fb);
    fflush(stderr);

    {
        /* Point the framebuffer window at whatever the title just set, and
         * start it on the first display mode -- before that there is nothing
         * to show and no pitch to interpret it with. */
        extern void xbox_FramebufferWindowSet(uint32_t, uint32_t);
        extern void xbox_FramebufferWindowStart(void);
        uint32_t fb_va = fb;

        /* AvSetDisplayMode reports the scanout address the way the CRTC wants
         * it -- a physical address. The window has to read it the way the CPU
         * sees it. Where the title allocated says which: a framebuffer from
         * MmAllocateContiguousMemory lives in the window at XBOX_CONTIG_BASE,
         * so physical P is visible at XBOX_CONTIG_BASE + P. Reading P
         * directly lands in the loaded image instead, which is why the window
         * showed black while the executor was clearing and rasterising
         * correctly a few megabytes away. */
        if (fb_va && fb_va < XBOX_CONTIG_SIZE)
            fb_va = XBOX_CONTIG_BASE + fb_va;

        xbox_FramebufferWindowSet(fb_va, pitch);
        xbox_FramebufferWindowStart();

        /* Record the resolved address, not the physical one the title passed.
         * Every reader of this wants to read guest memory with it, and the
         * physical form lands in the loaded image -- so the checksum probe
         * reported an unchanging zero while the title was drawing correctly a
         * few megabytes away, and so did every dump that asked for "the"
         * framebuffer. Resolving once here is the fix; resolving in each
         * caller is how there came to be two of them disagreeing. */
        xbox_SetDisplayFramebuffer(fb_va, pitch);
    }
    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

/* ── PsTerminateSystemThread (ordinal 258) ───────────────
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * In our recompiled version, threads run synchronously, so we just
 * return. The caller (sub_001D1818) handles this gracefully.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X%s\n",
            exit_status, g_is_spawned_thread ? " (worker)" : " (main)");
    fflush(stderr);

    g_eax = exit_status;

    /*
     * This does not return on hardware. Returning was survivable while every
     * thread ran on the host's main thread, but a spawned worker that returns
     * here falls off the end of its start routine and into whatever bytes
     * follow -- Halo's input worker landed on an int 3, and the resulting
     * breakpoint took down the whole process while the main thread was still
     * inside input_initialize.
     *
     * The main thread still returns: it is the host's thread and unwinding
     * back to main() is how the process shuts down cleanly.
     */
    if (g_is_spawned_thread) {
        /* The normal exit for a worker, and therefore the one that has to
         * return the stack -- ExitThread never comes back to bridge_thread_main
         * to do it. */
        thread_notify_run(g_thread_obj_va, 0);
        xbox_FreeThreadStack(g_thread_stack_top);
        g_thread_stack_top = 0;
        thalert_forget(g_thread_obj_va);
        ks_thread_exit();
        thobj_exit(g_thread_obj_va, exit_status);
        g_thread_obj_va = 0;
        bridge_free_thread_tib();
        ExitThread(exit_status);
    }
}

/* ── HalReadSMCTrayState (ordinal 47) ─────────────────────
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;  /* No disc */
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* -- KeInsertQueueDpc (ordinal 119) ------------------------
 * BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1,
 *                          PVOID SystemArgument2)
 *
 * Unbridged this returned 0, which reads to a driver as "already queued" and
 * means the deferred routine never runs. That is not a small loss: an
 * interrupt service routine is expected to do almost nothing except mask the
 * source and queue a DPC, so with this missing every driver that follows the
 * normal pattern acknowledges its interrupt and then does none of the work.
 * Half-Life 2's USB stack does exactly that -- its ISR masks the master
 * interrupt enable and queues here, and the enumeration it should have started
 * lives entirely in the deferred routine.
 *
 * ponytail: runs the routine inline rather than queueing it. A real DPC runs
 * at DISPATCH_LEVEL shortly after the ISR returns, and this runs it before the
 * ISR returns, on whichever thread queued it. That ordering difference has not
 * mattered for anything here yet; when it does, the upgrade is a real queue
 * drained by the thread that lowered IRQL, not a second call site.
 */
/* Run a DPC's deferred routine on the calling thread.
 *
 * VOID DeferredRoutine(PKDPC, PVOID Context, PVOID Arg1, PVOID Arg2),
 * __stdcall: its `ret 16` consumes the dummy return address and all four
 * arguments, so g_esp needs no fixup afterwards. The caller must already have
 * a guest stack -- true on a guest thread, and true on a host thread that has
 * taken a worker slice.
 *
 * Returns 1 if the routine was found and called.
 */
/* The DPC routine running now and since when (GetTickCount), for the device
 * models' stall warnings: a deferred routine that never returns stops every
 * other DPC, and the model that notices (its driver's ISR masked it and the
 * DPC never unmasked it) can say which routine is stuck. One slot: with the
 * gate on DPCs never run two at a time; with it off it names the latest. */
static volatile uint32_t s_dpc_now_routine;
static volatile LONG s_dpc_now_since;
static volatile LONG s_dpc_now_tid;
/* Gate off: DPCs that started while another thread was at DISPATCH_LEVEL.
 * A driver that locks by raising IRQL (XAPI's USB stack does) is racing its
 * own DPC every time this moves. */
static volatile LONG s_dpc_overlaps;

int xbox_DpcRunning(uint32_t *routine, unsigned long *ms)
{
    uint32_t r = s_dpc_now_routine;
    LONG since = InterlockedCompareExchange(&s_dpc_now_since, 0, 0);
    if (!r || r != s_dpc_now_routine)
        return 0;
    if (routine) *routine = r;
    if (ms) *ms = (unsigned long)GetTickCount() - (unsigned long)since;
    return 1;
}

long xbox_DpcOverlapCount(void)
{
    return (long)InterlockedCompareExchange(&s_dpc_overlaps, 0, 0);
}

/* A KDPC the kernel makes for xbox_DpcQueueGuestCall (Type byte; a real
 * KDPC is type 19). */
#define GUEST_CALL_DPC_TYPE 0xC0u

static int kernel_run_dpc(uint32_t dpc_va, uint32_t arg1, uint32_t arg2)
{
    uint32_t routine, context;
    recomp_func_t fn;
    uint32_t prev_routine;
    LONG prev_since, prev_tid, tid;

    if (!dpc_va)
        return 0;
    routine = BRIDGE_MEM32(dpc_va + 12);
    context = BRIDGE_MEM32(dpc_va + 16);
    if (!routine)
        return 0;

    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] DPC routine 0x%08X not in dispatch\n",
                routine);
        fflush(stderr);
        return 0;
    }

    BRIDGE_MEM32(dpc_va + 20) = arg1;
    BRIDGE_MEM32(dpc_va + 24) = arg2;

    {
        CallbackRegs saved;
        callback_regs_save(&saved);
        if (BRIDGE_MEM8(dpc_va) == GUEST_CALL_DPC_TYPE) {
            /* xbox_DpcQueueGuestCall: routine(context), cdecl, as D3D's
             * interrupt handler calls an InsertCallback routine. The
             * registers (esp too) are put back after, whatever it pops. */
            g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
        } else {
            g_esp -= 4; BRIDGE_MEM32(g_esp) = arg2;
            g_esp -= 4; BRIDGE_MEM32(g_esp) = arg1;
            g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
            g_esp -= 4; BRIDGE_MEM32(g_esp) = dpc_va;
        }
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        if (!xbox_DispatchGateOn()
            && xbox_IrqlRaisedCount() > (xbox_IrqlCurrent() >= 2 ? 1 : 0))
            InterlockedIncrement(&s_dpc_overlaps);
        /* Nested (a DPC run from inside one) puts the outer one back. */
        prev_routine = s_dpc_now_routine;
        prev_since = s_dpc_now_since;
        prev_tid = s_dpc_now_tid;
        tid = (LONG)GetCurrentThreadId();
        InterlockedExchange(&s_dpc_now_since, (LONG)GetTickCount());
        InterlockedExchange(&s_dpc_now_tid, tid);
        s_dpc_now_routine = routine;
        { int _irql = xbox_IrqlEnterInterrupt(2); fn(); xbox_IrqlLeaveInterrupt(_irql); }
        if (InterlockedCompareExchange(&s_dpc_now_tid, 0, 0) == tid) {
            s_dpc_now_routine = prev_routine;
            InterlockedExchange(&s_dpc_now_since, prev_since);
            InterlockedExchange(&s_dpc_now_tid, prev_tid);
        }
        callback_regs_restore(&saved, 0);
    }
    return 1;
}

/* -- KeSynchronizeExecution (ordinal 153) ------------------
 * BOOLEAN KeSynchronizeExecution(PKINTERRUPT Interrupt,
 *                                PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
 *                                PVOID SynchronizeContext)
 *
 * Runs a routine while holding the interrupt's spinlock at the ISR's IRQL,
 * which is how a driver touches its hardware from anywhere that is not the
 * ISR without racing the ISR. Unbridged it returned 0 and the routine never
 * ran -- the same failure as an unqueued DPC, and just as quiet: the driver
 * asks for exclusive access, is told it did not get it, and skips the work.
 *
 * The lock is the one-CPU gate the device models deliver ISRs under (one
 * for all interrupts, not the interrupt object's own).
 */
static void bridge_KeSynchronizeExecution(void)
{
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);
    recomp_func_t fn;

    fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn && routine) fn = recomp_lookup_manual(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] KeSynchronizeExecution: routine 0x%08X "
                        "not in dispatch\n", routine);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* BOOLEAN SynchronizeRoutine(PVOID Context), __stdcall. Its `ret 4` takes
     * the dummy return address and the argument, so g_esp needs no fixup. */
    CallbackRegs saved;
    callback_regs_save(&saved);
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    /* The ISR's exclusion: the APU and OHCI models and the vblank deliver
     * their ISRs holding the one-CPU gate when it is free and post them to
     * its holder when it is not (xbox_IrqPost), so holding it here keeps the
     * routine and those ISRs apart, as the interrupt's spinlock at DIRQL
     * does. The leave is a safe point: what was posted meanwhile runs there,
     * with the routine's result in g_eax saved around it. */
    /* The routine is the title's code on its own thread, so it holds the
     * guest CPU too (kernel_guest_cpu.c), taken after the gate: the order
     * everywhere is gate, then CPU, never the reverse. */
    xbox_DispatchGateEnter();
    {
        int took = xbox_GuestCpuEnterCallback();
        fn();
        xbox_GuestCpuLeaveCallback(took);
    }
    xbox_DispatchGateLeave();
    /* g_eax is whatever the routine returned, which is this call's result. */
    callback_regs_restore(&saved, 1);
}

/* -- KeRemoveQueueDpc (ordinal 137) ------------------------
 * BOOLEAN KeRemoveQueueDpc(PKDPC Dpc)
 *
 * Cancels a queued DPC, returning whether it was still in the queue. DPCs run
 * inline here (see KeInsertQueueDpc), so by the time anyone can call this the
 * routine has already run and there is nothing to cancel. FALSE is both the
 * honest answer and the one that keeps a caller's bookkeeping right.
 */
static void bridge_KeRemoveQueueDpc(void);   /* defined with the queue */

/* The pending DPC queue.
 *
 * These used to run inline, on whichever thread queued them, before the caller
 * returned. That is not what a DPC is: an interrupt service routine masks its
 * source and queues, and the deferred routine runs afterwards at DISPATCH_LEVEL
 * -- after the ISR has returned. Running it inline inverts that, so a routine
 * that re-enables interrupts does it while the ISR that masked them is still on
 * the stack, and a driver's state machine re-enters itself from inside its own
 * interrupt.
 *
 * Queued properly now, and drained by the timer thread, which is the one thread
 * here that already has a guest stack and a TIB and runs nothing else urgent.
 *
 * ponytail: one queue, no IRQL, no per-processor list, and a DPC queued from a
 * DPC runs on the next drain rather than immediately. Nothing here depends on
 * DPC ordering beyond "after the ISR".
 */
#define XBOX_MAX_PENDING_DPC 64
typedef struct { uint32_t dpc, arg1, arg2; } PendingDpc;
static PendingDpc g_dpc_queue[XBOX_MAX_PENDING_DPC];
static volatile LONG g_dpc_head, g_dpc_tail;

/* The queue is fed from several host threads at once -- the USB and APU
 * controller threads raise interrupts whose ISRs queue DPCs, and the title
 * queues its own -- and was an unlocked ring: two inserts could take the same
 * slot and one DPC was lost. A lost USB DPC is a done queue the driver never
 * acknowledges; the controller then completes nothing more and the pad goes
 * dead mid-game, which is what it did.
 *
 * And a DPC already queued is not queued twice: the kernel keeps an Inserted
 * flag in the KDPC (+2) and KeInsertQueueDpc returns FALSE while it is set.
 * Running a driver's DPC twice for one interrupt makes it walk a done list it
 * has already consumed. */
static CRITICAL_SECTION g_dpc_lock;
static INIT_ONCE g_dpc_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK dpc_lock_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_dpc_lock);
    return TRUE;
}

static void dpc_lock(void)
{
    InitOnceExecuteOnce(&g_dpc_lock_once, dpc_lock_init, NULL, NULL);
    EnterCriticalSection(&g_dpc_lock);
}

int xbox_DpcQueue(uint32_t dpc, uint32_t arg1, uint32_t arg2)
{
    LONG tail, next;

    if (!dpc)
        return 0;

    dpc_lock();
    if (BRIDGE_MEM8(dpc + 2)) {                 /* already queued */
        LeaveCriticalSection(&g_dpc_lock);
        return 0;
    }
    tail = g_dpc_tail;
    next = (tail + 1) % XBOX_MAX_PENDING_DPC;
    if (next == g_dpc_head) {
        LeaveCriticalSection(&g_dpc_lock);
        fprintf(stderr, "  [KERNEL] DPC queue full, dropping 0x%08X\n", dpc);
        fflush(stderr);
        return 0;
    }
    g_dpc_queue[tail].dpc  = dpc;
    g_dpc_queue[tail].arg1 = arg1;
    g_dpc_queue[tail].arg2 = arg2;
    BRIDGE_MEM8(dpc + 2) = 1;
    g_dpc_tail = next;
    LeaveCriticalSection(&g_dpc_lock);
    /* The timer thread may be asleep to its next vblank edge, 10 ms away:
     * a DPC that no guest thread picks up at a raise would wait it out. */
    if (xbox_DpcOnRaise())
        xbox_HostTimerWake();
    return 1;
}

/* Run routine(context) on the DPC path, at DISPATCH_LEVEL, as an interrupt's
 * DPC would (XBA-94: D3D's InsertCallback, software methods 6 and 7, whose
 * handler calls a guest routine). Takes a free KDPC from a small pool in
 * guest memory, typed GUEST_CALL_DPC_TYPE so kernel_run_dpc calls the
 * routine with the one argument. Returns 0 when the routine is not code or
 * the pool is busy. Any thread. */
#define GUEST_CALL_POOL 16
int xbox_DpcQueueGuestCall(uint32_t routine, uint32_t context)
{
    static uint32_t pool;
    int i;

    if (!routine || (!recomp_lookup(routine) && !recomp_lookup_manual(routine)))
        return 0;
    if (!pool) {
        uint32_t p = xbox_HeapAlloc(GUEST_CALL_POOL * 32u, 16);
        if (!p)
            return 0;
        memset((void *)((uintptr_t)p + g_xbox_mem_offset), 0, GUEST_CALL_POOL * 32u);
        if (InterlockedCompareExchange((volatile LONG *)&pool, (LONG)p, 0) != 0)
            ;                           /* another thread made one first; this one stays unused */
    }
    for (i = 0; i < GUEST_CALL_POOL; i++) {
        uint32_t d = pool + (uint32_t)i * 32u;
        if (BRIDGE_MEM8(d + 2))
            continue;                   /* queued, not run yet */
        BRIDGE_MEM8(d) = (uint8_t)GUEST_CALL_DPC_TYPE;
        BRIDGE_MEM32(d + 12) = routine;
        BRIDGE_MEM32(d + 16) = context;
        if (xbox_DpcQueue(d, 0, 0))
            return 1;
    }
    return 0;
}

int xbox_DpcPending(void)
{
    return g_dpc_head != g_dpc_tail;
}

static void bridge_KeInsertQueueDpc(void)
{
    g_eax = (uint32_t)xbox_DpcQueue(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2));
}

/* Cancel a queued DPC: take it out of the queue if it is still there. */
static void bridge_KeRemoveQueueDpc(void)
{
    uint32_t dpc = STACK_ARG(0);
    LONG i;

    g_eax = 0;
    if (!dpc)
        return;
    dpc_lock();
    if (BRIDGE_MEM8(dpc + 2)) {
        for (i = g_dpc_head; i != g_dpc_tail; i = (i + 1) % XBOX_MAX_PENDING_DPC)
            if (g_dpc_queue[i].dpc == dpc)
                g_dpc_queue[i].dpc = 0;         /* drained as a no-op */
        BRIDGE_MEM8(dpc + 2) = 0;
        g_eax = 1;
    }
    LeaveCriticalSection(&g_dpc_lock);
}

/* Call a connected interrupt service routine.
 *
 * BOOLEAN ServiceRoutine(PKINTERRUPT, PVOID ServiceContext), __stdcall: its
 * `ret 8` takes the dummy return address and both arguments, so g_esp needs no
 * fixup. The caller must already have a guest stack and a TIB, which the timer
 * thread has.
 *
 * Returns what the routine returned -- an ISR that does not recognise the
 * interrupt returns FALSE, and that is worth seeing rather than assuming.
 */
uint32_t xbox_GetConnectedInterrupt(uint32_t vector);   /* defined below */

static int kernel_call_isr(uint32_t vector)
{
    uint32_t kint = xbox_GetConnectedInterrupt(vector);
    uint32_t routine, context;
    recomp_func_t fn;

    if (!kint)
        return -1;
    routine = BRIDGE_MEM32(kint + 0);
    context = BRIDGE_MEM32(kint + 4);
    if (!routine)
        return -1;
    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn)
        return -1;

    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = kint;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    { int _irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(_irql); }
    return (int)(g_eax & 1u);
}

/* The GPU's vertical blank, delivered rather than merely enabled.
 *
 * The D3D8 library linked into a title installs an ISR for this and then waits
 * on it. Nothing ever raised it, so a title whose frame loop waits for vblank
 * rather than polling stops after its first clear -- which is exactly where
 * Half-Life 2's loader stops, with its videos open, its 23 MB of UI textures
 * loaded, and no second frame.
 *
 * The ISR reads the NV2A's own interrupt status to decide whether the
 * interrupt is its business, so the registers have to say vblank before the
 * routine is called: PCRTC_INTR_0 bit 0 for the vblank itself, and PMC_INTR_0
 * bit 24 to say the PCRTC block is the source. Without those the handler looks,
 * finds nothing, and correctly declines.
 *
 * ponytail: a fixed 60 Hz off the timer tick rather than anything tied to the
 * display mode, and no field or interlace handling. A title that measures
 * refresh rate from this will read 60; a title that needs the real one wants
 * the mode AvSetDisplayMode was given, which is recorded a few files away.
 */
#define XBOX_NV2A_REG_BASE     0xFD000000u
#define NV2A_PMC_INTR_0        0x00000100u
#define NV2A_PMC_INTR_PCRTC    (1u << 24)
#define NV2A_PMC_INTR_EN_0     0x00000140u
#define NV2A_PCRTC_INTR_0      0x00600100u
#define NV2A_PCRTC_INTR_VBLANK (1u << 0)
#define NV2A_PCRTC_INTR_EN_0   0x00600140u
#define NV2A_VECTOR            3u

/* Milliseconds on the performance counter, for the vblank schedule, the
 * kernel timers and the vblank ack wait. GetTickCount64 is not good enough
 * for those under Wine: it reads a tick the wineserver advances in steps of
 * up to ~16 ms, so a schedule run on it fires vblanks 4-33 ms apart while
 * still averaging 60 Hz, and a title that does its streaming work once per
 * vblank misses its deadlines on the long gaps. */
static long long kernel_clock_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (!freq.QuadPart && !QueryPerformanceFrequency(&freq))
        return (long long)GetTickCount64();
    QueryPerformanceCounter(&now);
    return (long long)(now.QuadPart / freq.QuadPart) * 1000
           + (long long)(now.QuadPart % freq.QuadPart) * 1000 / freq.QuadPart;
}

/* The vblank acknowledgement, done when the guest gives it.
 *
 * PMC_INTR_0 and PCRTC_INTR_0 are write-1-to-clear. An XDK vblank DPC reads
 * PMC_INTR_0 to see what is pending, acks with PCRTC_INTR_0 = 1, and then
 * spins until PMC_INTR_0 shows PCRTC clear. Against plain RAM
 * only the nv2a-ack thread cleared that bit, once per pass. At Sleep(0) that
 * was immediate. At Sleep(1), with the push-buffer executor making each pass
 * take milliseconds, every vblank held the timer thread for the rest of a
 * pass: 35-43% of a core under Proton, 6.5% on macOS.
 *
 * So after the ISR, PCRTC_INTR_0's vblank bit is taken down (the ISR
 * has read it) and PMC's is left up for the DPC. This thread then waits for
 * the DPC's ack write, the bit coming back, and clears both. It blocks on an
 * event between vblanks.
 *
 * It used to give up after 2 ms. The DPC is queued, though, and the timer
 * thread runs it only once the one-CPU gate is free, so whenever a title
 * thread sat at DISPATCH across the vblank the DPC came late, acked into a
 * window nobody watched any more, and spun at DISPATCH until the nv2a-ack
 * pass came round. With a heavy push buffer a pass is hundreds of
 * milliseconds: the D3D vblank DPC then held the timer thread for a quarter
 * of the time, vblanks slowed to 45 Hz and audio streaming starved. So after
 * the first 2 ms of yielding the watch goes on at 1 ms waits, and past a
 * second at 16 ms ones, for as long as PMC_INTR_0's PCRTC bit is up: until
 * the ack comes, the next vblank re-arms it, or the bit goes down some other
 * way (the nv2a-ack backstop, xbox_VblankAckHeld).
 *
 * While the timer thread is between setting the two bits and the ISR's
 * return (tick_busy), PCRTC_INTR_0's bit is the new vblank, not an ack:
 * taking it then would ack the vblank before the ISR saw it. The bits are
 * changed with interlocked operations, since the guest and the nv2a-ack
 * thread write the same words. */
enum { VBL_ACK_FAST_MS = 2, VBL_ACK_LATE_MS = 1000, VBL_ACK_SLOW_STEP_MS = 16 };

int xbox_VblankAckWait(volatile uint32_t *pcrtc_intr, volatile uint32_t *pmc_intr,
                       volatile LONG *tick_busy, HANDLE rearm, long *late_ms)
{
    long long start = kernel_clock_ms(), now = start;

    *late_ms = 0;
    for (;;) {
        DWORD step;

        if (!*tick_busy && (*pcrtc_intr & NV2A_PCRTC_INTR_VBLANK)) {
            InterlockedAnd((volatile LONG *)pcrtc_intr,
                           (LONG)~NV2A_PCRTC_INTR_VBLANK);
            InterlockedAnd((volatile LONG *)pmc_intr,
                           (LONG)~NV2A_PMC_INTR_PCRTC);
            if (now - start <= VBL_ACK_FAST_MS)
                return XBOX_VBL_ACK_ON_TIME;
            *late_ms = (long)(now - start);
            return XBOX_VBL_ACK_LATE;
        }
        if (!*tick_busy && !(*pmc_intr & NV2A_PMC_INTR_PCRTC))
            return XBOX_VBL_ACK_NONE;   /* nobody is waiting on it */
        if (now - start <= VBL_ACK_FAST_MS) {
            SwitchToThread();
            now = kernel_clock_ms();
            continue;
        }
        step = now - start <= VBL_ACK_LATE_MS ? 1 : VBL_ACK_SLOW_STEP_MS;
        if (WaitForSingleObject(rearm, step) == WAIT_OBJECT_0) {
            /* The next vblank: it is that one's ack now. */
            return XBOX_VBL_ACK_REARMED;
        }
        now = kernel_clock_ms();
    }
}

static HANDLE g_vbl_ack_event;
/* Vblanks whose ack never came: the bit was left for the next one. Acks
 * that came after the 2 ms yield, and the latest of them. Reported, and
 * reset, on the 600-vblank line. */
static volatile LONG g_vbl_ack_giveups;
static volatile LONG g_vbl_ack_late, g_vbl_ack_late_max;
/* Vblanks the ISR declined: with the XDK ISR, ones that came while its DPC
 * had the interrupt masked. */
static volatile LONG g_vbl_declined;
/* Set by the timer thread from just before it raises the vblank bits until
 * the ISR has returned and the watch is re-armed. */
static volatile LONG g_vbl_tick_busy;
/* When the vblank-ack thread last saw the PCRTC bit acked or down: the low
 * 32 bits of kernel_clock_ms(), compared by unsigned difference. */
static volatile LONG g_vbl_ack_seen_ms;

static DWORD WINAPI kernel_vblank_ack_thread(LPVOID unused)
{
    int r = XBOX_VBL_ACK_ON_TIME;

    (void)unused;
    for (;;) {
        long late_ms;

        if (r != XBOX_VBL_ACK_REARMED)
            WaitForSingleObject(g_vbl_ack_event, INFINITE);
        r = xbox_VblankAckWait(
            &BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_0),
            &BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PMC_INTR_0),
            &g_vbl_tick_busy, g_vbl_ack_event, &late_ms);
        if (r != XBOX_VBL_ACK_REARMED)
            InterlockedExchange(&g_vbl_ack_seen_ms, (LONG)(uint32_t)kernel_clock_ms());
        if (r == XBOX_VBL_ACK_LATE) {
            InterlockedIncrement(&g_vbl_ack_late);
            if (late_ms > g_vbl_ack_late_max)
                InterlockedExchange(&g_vbl_ack_late_max, late_ms);
        } else if (r != XBOX_VBL_ACK_ON_TIME) {
            InterlockedIncrement(&g_vbl_ack_giveups);
        }
    }
    return 0;
}

static volatile LONG g_vbl_ack_owned;

/* The nv2a-ack thread's backstop. While the vblank-ack thread runs it owns
 * the two vblank bits, and nv2a-ack leaves them, unless no ack has come for
 * VBL_ACK_LATE_MS with no vblank being raised: then the guest is not acking
 * the XDK way (or its ack was lost), and nv2a-ack clears them each pass as it
 * did before the watch, so a DPC spinning on PMC_INTR_0 is let go. */
int xbox_VblankAckHeld(void)
{
    if (!g_vbl_ack_owned)
        return 0;
    if (g_vbl_tick_busy)
        return 1;
    return (uint32_t)kernel_clock_ms() - (uint32_t)g_vbl_ack_seen_ms
           <= VBL_ACK_LATE_MS;
}

void xbox_VblankArmBits(volatile uint32_t *pcrtc_intr, volatile LONG *tick_busy,
                        int raised)
{
    if (raised)
        InterlockedAnd((volatile LONG *)pcrtc_intr, (LONG)~NV2A_PCRTC_INTR_VBLANK);
    InterlockedExchange(tick_busy, 0);
}

/* Under an INIT_ONCE: on the ungated path a holder-run vblank and the timer
 * thread's can arm the first one together, and each would make an event and
 * a thread. FALSE leaves it to the next vblank to try again. */
static INIT_ONCE g_vbl_ack_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK kernel_vblank_ack_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    HANDLE t;

    (void)o; (void)p; (void)c;
    g_vbl_ack_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_vbl_ack_event)
        return FALSE;
    InterlockedExchange(&g_vbl_ack_seen_ms, (LONG)(uint32_t)kernel_clock_ms());
    t = CreateThread(NULL, 0, kernel_vblank_ack_thread, NULL, 0, NULL);
    if (!t) {
        CloseHandle(g_vbl_ack_event);
        g_vbl_ack_event = NULL;
        return FALSE;
    }
    CloseHandle(t);
    InterlockedExchange(&g_vbl_ack_owned, 1);
    return TRUE;
}

/* Arm the ack watch for the next ack. raised: this tick set PCRTC_INTR_0's
 * bit (the ISR ran, or declined), and the bit is taken down with tick_busy.
 * A masked tick set no PCRTC bit, so one that is up is the DPC's ack and is
 * left for the watch: an XDK DPC writes its ack once and then spins on
 * PMC_INTR_0, and wiping that write left it spinning, every guest raise
 * blocked behind it, until the nv2a-ack backstop a second later. */
static void kernel_vblank_arm(int raised)
{
    if (!InitOnceExecuteOnce(&g_vbl_ack_once, kernel_vblank_ack_init, NULL, NULL)) {
        InterlockedExchange(&g_vbl_tick_busy, 0);
        return;
    }
    xbox_VblankArmBits(&BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_0),
                       &g_vbl_tick_busy, raised);
    SetEvent(g_vbl_ack_event);
}


/* RECOMP_VBLANK: unset, empty or 0, no vblanks: 0 is off as it is for
 * RECOMP_FLIP_PACING and RECOMP_DISPATCH_GATE, so an A/B run's
 * RECOMP_VBLANK=0 does not quietly get the 60 Hz schedule a title's other
 * switches ask for. Any other value, the tick v0.14.0 had: a vblank
 * at most every 16 ms on GetTickCount64, checked on the timer thread's
 * 10 ms Sleep (about 50 Hz), raised with no regard for the enable registers
 * and acked by the nv2a-ack thread's passes. RECOMP_VBLANK=paced, or the
 * one-CPU gate on top of any RECOMP_VBLANK, selects the 60 Hz schedule
 * below instead: the absolute schedule on the performance counter, the
 * vblank-ack thread, PCRTC_INTR_EN_0 and PMC_INTR_EN_0 honoured, and
 * timeBeginPeriod(1) on Windows. The gate needs it (its DPCs come late,
 * and the tick's acks were lost behind them), and so does the flip hold
 * (RECOMP_FLIP_PACING), which holds flips to this count and would pace at
 * the tick's 50 Hz; a title that sets none of them gets the tick as
 * before. */
int xbox_VblankMode(const char *env, int paced_wanted)
{
    if (!env || !*env || !strcmp(env, "0"))
        return XBOX_VBLANK_OFF;
    if (paced_wanted || !strcmp(env, "paced"))
        return XBOX_VBLANK_PACED;
    return XBOX_VBLANK_TICK;
}

extern int nv2a_pb_exec_flip_pacing(void);

static int kernel_vblank_mode(void)
{
    static volatile LONG mode = -1;

    if (mode < 0)
        InterlockedExchange(&mode, xbox_VblankMode(getenv("RECOMP_VBLANK"),
                                                   xbox_DispatchGateOn()
                                                   || nv2a_pb_exec_flip_pacing()));
    return (int)mode;
}

/* The one vblank count. The tick increments it directly, the 60 Hz
 * schedule through kernel_vblank_count; only one of the two ever runs. */
static volatile LONG g_vblank_total;

uint32_t xbox_VblankCount(void)
{
    return (uint32_t)g_vblank_total;
}

/* Run once per vblank on the timer thread, after the count (xbox_VblankHookSet).
 * Set once before the guest starts; a pointer-sized volatile is enough. */
static void (*volatile s_vblank_hook)(void);

void xbox_VblankHookSet(void (*hook)(void))
{
    s_vblank_hook = hook;
}

/* The title's cheat codes (XBA-241, xbox_cheats.h) run first, then the hook. */
static void kernel_vblank_hook(void)
{
    void (*hook)(void) = s_vblank_hook;
    xbox_CheatsVblank();
    if (hook)
        hook();
}

/* Count one vblank, and once per 600 print a line that
 * should read ~10000 ms. Also the shortest and longest gap between two, by
 * the performance counter rather than the clock the schedule runs on. */
static void kernel_vblank_count(long long now)
{
    static unsigned long long total;
    static long long last_ms;
    static LARGE_INTEGER qf, qlast;
    static double gap_min = 1e9, gap_max;
    LARGE_INTEGER q;

    InterlockedIncrement(&g_vblank_total);
    kernel_vblank_hook();
    if (!qf.QuadPart)
        QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q);
    if (qlast.QuadPart && qf.QuadPart) {
        double g = (double)(q.QuadPart - qlast.QuadPart) * 1000.0
                   / (double)qf.QuadPart;
        if (g < gap_min) gap_min = g;
        if (g > gap_max) gap_max = g;
    }
    qlast = q;
    if (++total % 600 == 0) {
        fprintf(stderr, "  [NV2A] vblank %llu: last 600 in %lld ms,"
                " gap %.1f-%.1f ms, ack give-ups %ld, late acks %ld"
                " (max %ld ms), declined %ld\n",
                total, last_ms ? now - last_ms : 0, gap_min, gap_max,
                (long)InterlockedExchange(&g_vbl_ack_giveups, 0),
                (long)InterlockedExchange(&g_vbl_ack_late, 0),
                (long)InterlockedExchange(&g_vbl_ack_late_max, 0),
                (long)InterlockedExchange(&g_vbl_declined, 0));
        fflush(stderr);
        xbox_IrqReport();
        xbox_GuestCpuReport();
        gap_min = 1e9;
        gap_max = 0;
        last_ms = now;
    } else if (total == 1) {
        last_ms = now;
    }
}


/* The vblank ISR and what follows it, on whichever thread runs it: the
 * timer thread with the gate, or the gate holder at a safe point. */
static void kernel_vblank_deliver(void)
{
    static unsigned n;
    int claimed = kernel_call_isr(NV2A_VECTOR);

    /* Armed for a declined vblank too. The XDK ISR masks PMC_INTR_EN_0
     * and queues its DPC, which unmasks when it is done; until then
     * every vblank finds the mask down and is declined, but its status
     * bit stays latched in PMC_INTR_0, as on hardware. The DPC, run late
     * behind the one-CPU gate, then sees it, acks it and spins on it --
     * and with the watch armed only on a claim, nothing cleared it
     * until the nv2a-ack pass, hundreds of milliseconds under a heavy
     * push buffer. */
    if (claimed >= 0)
        kernel_vblank_arm(1);
    else
        InterlockedExchange(&g_vbl_tick_busy, 0);
    if (claimed == 0)
        InterlockedIncrement(&g_vbl_declined);
    if (n++ < 3) {
        fprintf(stderr, "  [NV2A] vblank -> ISR %s\n",
                claimed < 0 ? "not callable" :
                claimed ? "claimed it" : "declined it");
        fflush(stderr);
    }
}

/* The holder's run of a posted vblank: kernel_hal.c has put it on a worker
 * stack at the device IRQL. */
static void kernel_vblank_on_holder(void)
{
    kernel_vblank_deliver();
}

/* The NV2A asserts its interrupt line only while PMC_INTR_EN_0 is on. The
 * XDK ISR clears it before it queues its DPC and the DPC sets it again
 * last, so a vblank inside that window latches PMC_INTR_0's bit and calls
 * no ISR. It used to be posted regardless. A vblank posted to a gate holder
 * keeps tick_busy up until the holder's next safe point, and while it is up
 * the DPC's ack is not taken. When the holder is itself running the D3D
 * DPC (drained at a guest raise or lower, kernel_hal.c dpc_run_here) that
 * DPC spins on PMC_INTR_0 with no safe point in the loop: the ISR never
 * runs, tick_busy never drops, and the ack never comes. A title hung so
 * within minutes of boot. */
int xbox_VblankRaiseBits(volatile uint32_t *pcrtc_intr, volatile uint32_t *pmc_intr,
                         volatile uint32_t *pmc_intr_en, volatile LONG *tick_busy)
{
    /* Interlocked read: the enable is the holder's ISR and DPC writing. */
    if (!InterlockedCompareExchange((volatile LONG *)pmc_intr_en, 0, 0)) {
        InterlockedOr((volatile LONG *)pmc_intr, (LONG)NV2A_PMC_INTR_PCRTC);
        return 0;
    }
    InterlockedExchange(tick_busy, 1);
    InterlockedOr((volatile LONG *)pcrtc_intr, (LONG)NV2A_PCRTC_INTR_VBLANK);
    InterlockedOr((volatile LONG *)pmc_intr, (LONG)NV2A_PMC_INTR_PCRTC);
    return 1;
}

/* Raise one vblank: the PCRTC and PMC bits, then the ISR, then the ack
 * watch. */
static void kernel_vblank_raise(void)
{
    if (!xbox_GetConnectedInterrupt(NV2A_VECTOR))
        return;
    xbox_IrqSetHandler(XBOX_IRQ_VBLANK, kernel_vblank_on_holder);

    /* PCRTC_INTR_EN_0: D3D enables the vblank interrupt once it has
     * connected its ISR. Until then the hardware raises nothing. */
    if (!(BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_EN_0)
          & NV2A_PCRTC_INTR_VBLANK)) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [NV2A] vblank held: PCRTC_INTR_EN_0 off\n");
        return;
    }

    if (!xbox_VblankRaiseBits(
            &BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_0),
            &BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PMC_INTR_0),
            &BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PMC_INTR_EN_0),
            &g_vbl_tick_busy)) {
        /* Masked: what a declined delivery leaves, without the ISR, which
         * would decline it, and without touching PCRTC_INTR_0, which this
         * tick did not set. */
        kernel_vblank_arm(0);
        InterlockedIncrement(&g_vbl_declined);
        return;
    }

    /* Under the one-CPU gate when it is free; never waits for it (this is
     * the frame clock). Held, the ISR is posted to the holder and runs at
     * its next safe point: tick_busy stays up until then, since until the
     * ISR has run PCRTC_INTR_0's bit is this vblank, not an ack. A later
     * tick while it is pending only advances the counter, as a latched
     * line coalesces. */
    switch (xbox_IrqSafePointsOn() ? xbox_IrqPost(XBOX_IRQ_VBLANK)
                                   : XBOX_IRQ_UNGATED) {
    case XBOX_IRQ_GATED:
        kernel_vblank_deliver();
        xbox_DispatchGateLeave();
        break;
    case XBOX_IRQ_UNGATED: {
        /* No gate (the default: the try takes nothing, and the vblank runs
         * at once as it always did), RECOMP_IRQ_SAFE_POINTS=0 (beside a
         * holder, if any), or RECOMP_IRQ_SAFE_FORCE past RECOMP_IRQ_SAFE_MS. */
        int gated = xbox_DispatchGateTryEnter();
        if (!gated)
            xbox_IrqNoteUngated(XBOX_IRQ_VBLANK);
        kernel_vblank_deliver();
        if (gated)
            xbox_DispatchGateLeave();
        break;
    }
    default:
        break;                          /* the holder runs it */
    }
}

/* Vblank N is due at t0 + N * 1000 / 60 ms (50 for PAL): an absolute
 * schedule, so the rate holds however coarse the timer thread's sleep. It used to be
 * "now + 16" checked on a 10 ms tick, which fired every 20 ms -- 50 Hz -- and
 * a title's frame limiter counts these. Kept in nanoseconds: whole
 * milliseconds put each vblank up to a millisecond off its edge, and a title
 * that counts vblanks from the top of each frame takes that error straight
 * into its flip interval. A host that falls more than 100 ms behind (a
 * debugger stop, a stalled timer thread) restarts the schedule instead of
 * firing a burst. Returns 1 when it raised one; *next_due_ns is the next
 * edge, 0 when vblanks are off. */
#define VBLANK_RESTART_NS 100000000ull

static uint64_t vblank_due(uint64_t t0_ns, uint64_t n, uint32_t hz)
{
    return t0_ns + n * 1000000000ull / hz;
}

/* The rate is the display's: 50 Hz once a title sets a PAL-I mode, else 60
 * (xbox_AvRefreshHz, XBA-121); a change restarts the schedule. */
static int kernel_vblank_tick_ns(uint64_t now_ns, uint64_t *next_due_ns)
{
    static uint64_t t0_ns, count;
    static uint32_t hz;
    uint32_t want_hz;
    uint64_t due;

    *next_due_ns = 0;
    if (kernel_vblank_mode() != XBOX_VBLANK_PACED)
        return 0;
    want_hz = (uint32_t)xbox_AvRefreshHz();
    if (hz != want_hz) {
        hz = want_hz;
        t0_ns = now_ns;
        count = 0;
    }
    due = vblank_due(t0_ns, count, hz);
    if (now_ns < due) {
        *next_due_ns = due;
        return 0;
    }
    if (now_ns - due > VBLANK_RESTART_NS) {
        t0_ns = now_ns;
        count = 0;
    }
    count++;
    *next_due_ns = vblank_due(t0_ns, count, hz);
    kernel_vblank_count(kernel_clock_ms());
    kernel_vblank_raise();
    return 1;
}

/* RECOMP_VBLANK without the 60 Hz schedule: v0.14.0's tick, unchanged. On
 * the timer thread's plain 10 ms loop, so about every 20 ms. */
static void kernel_vblank_tick(void)
{
    static long long next_ms, last_ms;
    long long now;

    if (kernel_vblank_mode() != XBOX_VBLANK_TICK)
        return;

    now = (long long)xbox_GuestTickCount64();   /* fast-forward runs it faster (XBA-240) */
    if (now < next_ms)
        return;
    next_ms = now + 16;                       /* ~60 Hz */
    /* The rate it really runs at (the 10 ms Sleep makes it nearer 50 Hz),
     * in the same words as the 60 Hz schedule's line. Only a log line. */
    if (!last_ms)
        last_ms = now;              /* so the first line spans 599 vblanks */
    if (InterlockedIncrement(&g_vblank_total) % 600 == 0) {
        fprintf(stderr, "  [NV2A] vblank %ld (tick): last 600 in %lld ms\n",
                (long)g_vblank_total, now - last_ms);
        fflush(stderr);
        last_ms = now;
    }
    kernel_vblank_hook();

    if (!xbox_GetConnectedInterrupt(NV2A_VECTOR))
        return;

    BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PCRTC_INTR_0) |= NV2A_PCRTC_INTR_VBLANK;
    BRIDGE_MEM32(XBOX_NV2A_REG_BASE + NV2A_PMC_INTR_0)   |= NV2A_PMC_INTR_PCRTC;

    {
        static unsigned n;
        int claimed = kernel_call_isr(NV2A_VECTOR);
        if (n++ < 3)
            fprintf(stderr, "  [NV2A] vblank -> ISR %s\n",
                    claimed < 0 ? "not callable" :
                    claimed ? "claimed it" : "declined it");
        fflush(stderr);
    }
}

/* Take the next DPC off the queue, 0 when it is empty. The Inserted byte is
 * cleared before the routine runs, as the kernel does: the routine may queue
 * itself again. */
static int dpc_pop(PendingDpc *d)
{
    dpc_lock();
    if (g_dpc_head == g_dpc_tail) {
        LeaveCriticalSection(&g_dpc_lock);
        return 0;
    }
    *d = g_dpc_queue[g_dpc_head];
    g_dpc_head = (g_dpc_head + 1) % XBOX_MAX_PENDING_DPC;
    if (d->dpc)
        BRIDGE_MEM8(d->dpc + 2) = 0;
    LeaveCriticalSection(&g_dpc_lock);
    return 1;
}

/* Run whatever is queued. Called from the timer thread, which has the guest
 * stack and TIB that a deferred routine needs. Returns 1 when it stopped on
 * a busy gate with DPCs still queued; *ran counts the DPCs it ran. */
static int kernel_drain_dpcs(int *ran)
{
    *ran = 0;
    for (;;) {
        PendingDpc d;
        /* A DPC runs only while the one-CPU gate is free (no thread at
         * DISPATCH_LEVEL, no ISR or KeSynchronizeExecution routine), and
         * never waits for it: this thread is also the vblank and timer
         * source, which a guest parked at DISPATCH must not stop. What is
         * queued stays queued for the next tick, as DPCs wait on hardware
         * until the processor drops below DISPATCH. */
        if (!xbox_DispatchGateTryEnter()) {
            /* An unlocked look: a DPC queued just after is picked up by the
             * next pass anyway. */
            return g_dpc_head != g_dpc_tail;
        }
        if (!dpc_pop(&d)) {
            xbox_DispatchGateLeave();
            return 0;
        }
        if (d.dpc && kernel_run_dpc(d.dpc, d.arg1, d.arg2))
            (*ran)++;
        xbox_DispatchGateLeave();
    }
}

/* The timer thread's drain, for tests/irq_progress: the timer thread itself
 * cannot start without a title's TLS image for its TIB. */
int xbox_DpcDrainTimer(int *ran)
{
    return kernel_drain_dpcs(ran);
}

/* The queue, run on a guest thread that owns "the processor at DISPATCH":
 * its raise, before its critical region, and its release point
 * (kernel_hal.c, dpc_run_here). The caller holds the gate and is at or
 * below DISPATCH; kernel_run_dpc raises for the routine where it is not
 * there already. The routine runs on this thread's own guest stack, as the
 * kernel runs a DPC on the interrupted thread's. */
int xbox_DpcDrainHere(void)
{
    int ran = 0;
    PendingDpc d;

    while (dpc_pop(&d))
        if (d.dpc && kernel_run_dpc(d.dpc, d.arg1, d.arg2))
            ran++;
    return ran;
}

/* ── KeInitializeDpc (ordinal 107) ────────────────────────
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The Xbox KDPC structure is 32 bytes.
 * We zero it and set the routine and context pointers.
 */
static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    /* XBOX_TO_NATIVE maps a guest 0 to NULL, so an unchecked object pointer
     * makes this memset write through NULL inside the bridge. The export
     * returns void, so refusing is doing nothing -- which is what the real
     * kernel does with an object it cannot write. */
    if (!bridge_buf_ok(dpc_va, 32, "KeInitializeDpc")) {
        g_eax = 0;
        return;
    }

    /* Zero the structure (32 bytes) */
    memset(XBOX_TO_NATIVE(dpc_va), 0, 32);

    /* Set Type (0x13 = DpcObject) and fields */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

/* ── NV2A interrupt plumbing (ordinals 44, 98, 109) ───────
 *
 * The D3D8 library linked into a title installs an ISR for the GPU's vblank /
 * command-completion interrupt. There is no NV2A here and nothing ever raises
 * that interrupt, so these exist to let initialisation complete rather than to
 * deliver anything.
 *
 * KeConnectInterrupt reports success: reporting failure sends Halo's
 * rasterizer down an error path during preinitialize, and the goal is to get
 * past setup, not to pretend the hardware is broken.
 *
 * ponytail: no interrupt is ever delivered. Code that *waits* on the ISR
 * rather than polling will hang here, and the fix for that is to bridge the
 * D3D8 entry point that owns the wait, not to synthesise NV2A interrupts.
 */

/* ULONG HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql) */
static void bridge_HalGetInterruptVector(void)
{
    uint32_t level   = STACK_ARG(0);
    uint32_t irql_va = STACK_ARG(1);

    if (irql_va) {
        /* IRQL is conventionally the vector for device interrupts. */
        BRIDGE_MEM8(irql_va) = (uint8_t)level;
    }
    g_eax = level;
}

/* VOID KeInitializeInterrupt(PKINTERRUPT, ServiceRoutine, ServiceContext,
 *                            Vector, Irql, InterruptMode, ShareVector) */
static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t routine      = STACK_ARG(1);
    uint32_t context      = STACK_ARG(2);
    uint32_t vector       = STACK_ARG(3);

    if (!bridge_buf_ok(interrupt_va, 44, "KeInitializeInterrupt")) {
        g_eax = 0;
        return;
    }

    /* Xbox KINTERRUPT is 44 bytes. */
    memset(XBOX_TO_NATIVE(interrupt_va), 0, 44);
    BRIDGE_MEM32(interrupt_va + 0)  = routine;
    BRIDGE_MEM32(interrupt_va + 4)  = context;
    BRIDGE_MEM32(interrupt_va + 8)  = vector;
    g_eax = 0;
}

/* The interrupt objects a title has connected, by vector.
 *
 * KeInitializeInterrupt already writes the service routine, its context and
 * the vector into the guest KINTERRUPT; connecting is what says the title is
 * ready to be called on it. Recording that is what lets a device model raise
 * an interrupt at all -- without it the routine is written down in guest
 * memory and nothing on this side knows it is there.
 *
 * A small fixed table rather than a list: an Xbox has 26 interrupt vectors and
 * a title connects a handful, so the whole thing is smaller than the comment.
 */
#define XBOX_MAX_VECTORS 32
static uint32_t g_connected_isr[XBOX_MAX_VECTORS];   /* KINTERRUPT guest VA */

/* BOOLEAN KeConnectInterrupt(PKINTERRUPT Interrupt) */
static void bridge_KeConnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);

    if (interrupt_va) {
        uint32_t vector = BRIDGE_MEM32(interrupt_va + 8);
        if (vector < XBOX_MAX_VECTORS) {
            g_connected_isr[vector] = interrupt_va;
            fprintf(stderr, "  [KERNEL] KeConnectInterrupt: vector %u -> "
                            "routine 0x%08X context 0x%08X\n",
                    vector, BRIDGE_MEM32(interrupt_va + 0),
                    BRIDGE_MEM32(interrupt_va + 4));
            fflush(stderr);
        }
    }
    g_eax = 1;  /* connected -- see the note above */
}

/* The KINTERRUPT a title connected on this vector, or 0. Device models use it
 * to find the routine to call; the routine and its context are at +0 and +4. */
uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    return (vector < XBOX_MAX_VECTORS) ? g_connected_isr[vector] : 0;
}

/* Tests only: what KeConnectInterrupt records, without a thunk table. */
void xbox_ConnectInterruptForTest(uint32_t vector, uint32_t kinterrupt_va)
{
    if (vector < XBOX_MAX_VECTORS)
        g_connected_isr[vector] = kinterrupt_va;
}

/* ── MmClaimGpuInstanceMemory (ordinal 168) ───────────────
 * PVOID MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, SIZE_T *Padding)
 *
 * Reserves the GPU instance memory the NV2A keeps its object context in. On
 * hardware it sits at the very top of physical RAM, so the returned address is
 * the end of the contiguous window minus the request. D3D8 stores this and
 * indexes off it, so returning 0 (the unbridged default) had it building
 * pointers from a null base.
 *
 * MAXULONG_PTR means "claim everything left"; the console answers with the
 * default instance size rather than the whole of RAM.
 */
static void bridge_MmClaimGpuInstanceMemory(void)
{
    uint32_t bytes      = STACK_ARG(0);
    uint32_t padding_va = STACK_ARG(1);

    if (bytes == 0xFFFFFFFFu) {
        bytes = XBOX_GPU_INSTANCE_DEFAULT;
    }
    if (padding_va) {
        BRIDGE_MEM32(padding_va) = 0;
    }
    g_eax = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - bytes;
}

/* VOID HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION, BOOLEAN)
 * Records a callback for console shutdown. Nothing here ever shuts down that
 * way, so registration is accepted and dropped. */
/* --- HalRegisterShutdownNotification (ordinal 47, 2 args = 8 bytes)
 * VOID HalRegisterShutdownNotification(
 *          PHAL_SHUTDOWN_REGISTRATION ShutdownRegistration, BOOLEAN Register);
 * HAL_SHUTDOWN_REGISTRATION { NotificationRoutine; LONG Priority;
 * LIST_ENTRY ListEntry; }. Registers (or takes back) a routine the HAL
 * calls as NotificationRoutine(ShutdownRegistration) when the title leaves
 * through HalReturnToFirmware, highest Priority first (XBA-114). */
#define SHUTDOWN_NOTIFY_MAX 16
static uint32_t s_shutdown_reg[SHUTDOWN_NOTIFY_MAX];
static volatile LONG s_shutdown_lock;

static void bridge_HalRegisterShutdownNotification(void)
{
    uint32_t reg = STACK_ARG(0);
    int add = (STACK_ARG(1) & 0xFF) != 0, i, j;

    g_eax = 0;
    if (!reg || !bridge_va_mapped(reg, 16))
        return;
    while (InterlockedCompareExchange(&s_shutdown_lock, 1, 0) != 0)
        SwitchToThread();
    for (i = 0; i < SHUTDOWN_NOTIFY_MAX && s_shutdown_reg[i] != reg; i++)
        ;
    if (i < SHUTDOWN_NOTIFY_MAX) {           /* there: take it out */
        for (j = i; j + 1 < SHUTDOWN_NOTIFY_MAX; j++)
            s_shutdown_reg[j] = s_shutdown_reg[j + 1];
        s_shutdown_reg[SHUTDOWN_NOTIFY_MAX - 1] = 0;
    }
    if (add && s_shutdown_reg[SHUTDOWN_NOTIFY_MAX - 1] == 0) {
        int32_t pri = (int32_t)BRIDGE_MEM32(reg + 4);
        for (i = 0; s_shutdown_reg[i] && (int32_t)BRIDGE_MEM32(s_shutdown_reg[i] + 4) >= pri; i++)
            ;
        for (j = SHUTDOWN_NOTIFY_MAX - 1; j > i; j--)
            s_shutdown_reg[j] = s_shutdown_reg[j - 1];
        s_shutdown_reg[i] = reg;
    }
    InterlockedExchange(&s_shutdown_lock, 0);
}

/* HalReturnToFirmware: each registered routine, once, in priority order. */
static void bridge_run_shutdown_notifications(void)
{
    uint32_t regs[SHUTDOWN_NOTIFY_MAX];
    int i, n = 0;

    while (InterlockedCompareExchange(&s_shutdown_lock, 1, 0) != 0)
        SwitchToThread();
    for (i = 0; i < SHUTDOWN_NOTIFY_MAX && s_shutdown_reg[i]; i++)
        regs[n++] = s_shutdown_reg[i];
    memset(s_shutdown_reg, 0, sizeof s_shutdown_reg);
    InterlockedExchange(&s_shutdown_lock, 0);
    for (i = 0; i < n; i++) {
        uint32_t a[1] = { regs[i] }, routine = BRIDGE_MEM32(regs[i]);
        if (routine)
            bridge_call_guest(routine, a, 1);
    }
}

/* ── Kernel timers (ordinals 113, 149, 150, 97) ───────────
 *
 * Timers are waitable: KeInitializeTimerEx backs every guest KTIMER with a
 * Win32 event in the shadow table (manual reset for notification timers, auto
 * reset for synchronization timers), so a KeWaitForSingleObject on a timer
 * sleeps on a real object. KeSetTimer/KeSetTimerEx arm the polling table below
 * whose thread fires DPCs and signals that event; KeCancelTimer clears both. */

/* ── KeInitializeTimerEx (ordinal 113) ────────────────────
 * VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
 */
static void bridge_KeInitializeTimerEx(void)
{
    uint32_t timer_va = STACK_ARG(0);
    uint32_t type     = STACK_ARG(1);
    HANDLE   ev;

    if (!timer_va) {
        g_eax = 0;
        return;
    }

    memset(XBOX_TO_NATIVE(timer_va), 0, 40);

    /* Dispatcher header. 0x08 = notification timer, 0x09 = synchronization. */
    BRIDGE_MEM8(timer_va + 0)  = (uint8_t)(0x08 + (type & 1));
    BRIDGE_MEM8(timer_va + 2)  = 40;      /* Size */
    BRIDGE_MEM8(timer_va + 3)  = 0;       /* Inserted */
    BRIDGE_MEM32(timer_va + 4) = 0;       /* SignalState */
    BRIDGE_MEM32(timer_va + 8) = 0;       /* WaitListHead */
    BRIDGE_MEM32(timer_va + 12) = 0;

    ev = CreateEventW(NULL, (type == 0) ? TRUE : FALSE, FALSE, NULL);
    if (!ev) {
        fprintf(stderr, "  [KERNEL] KeInitializeTimerEx: CreateEventW failed "
                        "(error %u)\n", GetLastError());
        g_eax = 0;
        return;
    }
    ke_shadow_insert(timer_va, ev);
    ke_shadow_note_header(timer_va);
    g_eax = 0;
}

/* -- KeSetTimer / KeSetTimerEx (ordinal 149/150) ----------
 * BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
 * BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period,
 *                      PKDPC Dpc)
 *
 * These used to return FALSE and do nothing, on the grounds that timers were
 * not needed for basic execution. They are needed for more than that: a driver
 * that polls its hardware does it from a timer DPC, and one that never fires
 * is a state machine that never advances. Half-Life 2's USB stack sets one up
 * during XInitDevices and enumerates from it.
 *
 * A due time is in 100 ns units, negative for relative and positive for an
 * absolute time since 1601. Only the relative form is honoured here; an
 * absolute due time is treated as immediate, which is wrong in principle and
 * has not come up in practice.
 *
 * ponytail: one thread, a fixed table, and a 10 ms tick, so a due time is late
 * by up to a tick and a periodic timer drifts. Nothing here is scheduling
 * audio off a timer. A title that needs better wants the host's timer queue,
 * not a smaller sleep.
 */
/* 128 armed at once (KRN-9): with 32, the 33rd KeSetTimer returned as if
 * armed, reset the timer's event and never fired it, with no log line; a
 * title arming one per device or per stream waited for ever on it. A full
 * table now says so once. */
#define XBOX_MAX_TIMERS 128
typedef struct {
    uint32_t timer_va;      /* the guest KTIMER, 0 for a free slot */
    uint32_t dpc_va;
    long long due_ms;       /* host tick when it fires */
    long      period_ms;    /* 0 for one-shot */
} XboxTimer;
static XboxTimer g_timers[XBOX_MAX_TIMERS];
static CRITICAL_SECTION g_timer_lock;
static volatile int g_timer_started;
static INIT_ONCE g_timer_once = INIT_ONCE_STATIC_INIT;
static void kernel_timer_start_once(void);

/* Whether the timer thread runs the paced loop below (kernel_timer_loop_ns):
 * only for what needs it, the 60 Hz vblank (RECOMP_VBLANK=paced) and the
 * one-CPU gate (RECOMP_DISPATCH_GATE), whose DPCs wait on a busy gate.
 * Otherwise it is the plain 10 ms tick on GetTickCount64 it always was, with
 * RECOMP_VBLANK's tick on it, so a title that asks for neither is scheduled
 * as before. */
static int kernel_timer_paced(void)
{
    static int paced = -1;

    if (paced < 0)
        paced = kernel_vblank_mode() == XBOX_VBLANK_PACED
                || xbox_DispatchGateOn();
    return paced;
}

/* The clock the kernel timers' due times are kept on, the same one the loop
 * that fires them reads: the guest's, ahead of the host's by what
 * fast-forward has gained (guest_clock.h, XBA-240; nothing until it runs). */
static long long kernel_timer_clock_ms(void)
{
    return (kernel_timer_paced() ? kernel_clock_ms()
                                 : (long long)GetTickCount64())
           + (long long)xbox_GuestClockExtraUnits(1000);
}

/* Fire every kernel timer due at now_ms. Returns how many fired;
 * *next_due_ms is the earliest still armed, -1 when none. */
static int kernel_fire_timers(long long now, long long *next_due_ms)
{
    int i, fired = 0;

    *next_due_ms = -1;
    for (i = 0; i < XBOX_MAX_TIMERS; i++) {
        uint32_t dpc, fired_va;

        EnterCriticalSection(&g_timer_lock);
        if (!g_timers[i].timer_va || now < g_timers[i].due_ms) {
            if (g_timers[i].timer_va
                && (*next_due_ms < 0 || g_timers[i].due_ms < *next_due_ms))
                *next_due_ms = g_timers[i].due_ms;
            LeaveCriticalSection(&g_timer_lock);
            continue;
        }
        fired_va = g_timers[i].timer_va;
        dpc = g_timers[i].dpc_va;
        if (g_timers[i].period_ms > 0) {
            g_timers[i].due_ms = now + g_timers[i].period_ms;
            if (*next_due_ms < 0 || g_timers[i].due_ms < *next_due_ms)
                *next_due_ms = g_timers[i].due_ms;
        } else {
            g_timers[i].timer_va = 0;      /* one-shot, done */
        }
        LeaveCriticalSection(&g_timer_lock);
        fired++;

        /* Outside the lock: the routine can set or cancel timers. Like a
         * queued DPC it runs only while the one-CPU gate is free; with a
         * guest at DISPATCH it is queued instead, so the vblank and timer
         * source never waits for the gate. */
        if (dpc) {
            if (xbox_DispatchGateTryEnter()) {
                kernel_run_dpc(dpc, 0, 0);
                xbox_DispatchGateLeave();
            } else {
                xbox_DpcQueue(dpc, 0, 0);
            }
        }

        /* Wake anyone parked on the timer's shadow event; a timer with no
         * DPC is just a kernel sleep. */
        {
            HANDLE ev = ke_shadow_lookup(fired_va);
            if (ev) {
                SetEvent(ev);
                BRIDGE_MEM32(fired_va + 4) = 1;   /* SignalState */
            }
        }
    }
    return fired;
}


/* The timer thread's loop: sleep to the earliest of the next vblank edge,
 * the next kernel timer and a 10 ms cap, on the nanosecond clock, then
 * raise what is due, drain DPCs and fire timers.
 *
 * A drain that finds the one-CPU gate busy leaves its DPCs queued (it never
 * waits for the gate: this thread is the frame clock, and a guest parked at
 * DISPATCH must not stop it). It used to find them again only at the next
 * vblank or the 10 ms tick, and the title's vblank callback, which runs in
 * such a DPC, came up to 10 ms late -- the flip landed late with it. Now the
 * next pass comes within 1 ms while DPCs wait on the gate. */
static void kernel_timer_loop_ns(void)
{
    const uint64_t ms = 1000000u;
    uint64_t wait = 10 * ms;

    xbox_HostTimerThreadInit();
    for (;;) {
        uint64_t now, vbl_next, target;
        long long now_ms, timer_due;
        int ran, busy;

        /* Wakeable: KeInsertQueueDpc ends it, so a DPC that no guest thread
         * drains at a raise runs now rather than at the next edge. */
        xbox_HostTimerSleepNs(wait);
        xbox_FastForwardPoll();           /* the speed for this pass (XBA-240) */
        /* The vblank grid and the timers run on the guest clock (the host's
         * until fast-forward first runs); the waits to them are host time. */
        kernel_vblank_tick_ns(xbox_GuestNowNs(), &vbl_next);
        busy = kernel_drain_dpcs(&ran);   /* deferred work, before due timers */
        now_ms = kernel_timer_clock_ms();
        kernel_fire_timers(now_ms, &timer_due);
        if (g_dpc_head != g_dpc_tail)     /* a timer DPC queued on a busy gate */
            busy = 1;

        now = xbox_HostNowNs();
        target = now + 10 * ms;
        if (vbl_next) {
            /* Behind the grid (a late pass): catch up one vblank per
             * millisecond, not in a burst. */
            uint64_t gnow = now + xbox_GuestClockExtraNs();
            uint64_t v = vbl_next > gnow ? now + xbox_GuestClockHostNs(vbl_next - gnow)
                                         : now + ms;
            if (v < target)
                target = v;
        }
        if (timer_due >= 0) {
            uint64_t t = now + (timer_due > now_ms
                                ? xbox_GuestClockHostNs((uint64_t)(timer_due - now_ms) * ms) : 0);
            if (t < target)
                target = t;
        }
        if (busy && now + ms < target)
            target = now + ms;
        wait = target > now ? target - now : 0;
    }
}

static DWORD WINAPI kernel_timer_thread(LPVOID unused)
{
    int slot = xbox_worker_stack_alloc();

    (void)unused;
    if (slot < 0) {
        fprintf(stderr, "  [KERNEL] timer thread has no worker stack; "
                        "timer DPCs will not run\n");
        fflush(stderr);
        return 0;
    }
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    {
        /* Its own TIB, for the same reason bridge_thread_main gives one to
         * every worker: a DPC routine with an SEH prologue reads fs:[0],
         * and a thread whose g_fs_base is zero faults inside the runtime
         * before the routine runs. */
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib) {
            fprintf(stderr, "  [KERNEL] timer thread has no TIB; "
                            "timer DPCs will not run\n");
            fflush(stderr);
            return 0;
        }
        g_fs_base = tib;
    }

    if (!kernel_timer_paced()) {
        /* Neither the 60 Hz vblank nor the gate: the 10 ms tick, the
         * vblank tick when RECOMP_VBLANK asks for it, DPCs before due
         * timers. */
        for (;;) {
            long long timer_due;
            int ran;

            Sleep(xbox_GuestClockHostMs(10));   /* 10 guest ms (XBA-240) */
            xbox_FastForwardPoll();
            kernel_vblank_tick();  /* the GPU's frame clock */
            kernel_drain_dpcs(&ran);
            kernel_fire_timers(kernel_timer_clock_ms(), &timer_due);
        }
    }

#ifdef _WIN32
    /* Sleep(1) is 15.6 ms on native Windows without this. The loop sleeps
     * on a high-resolution waitable timer, and wants it for the Sleep
     * fallback where there is none. Wine needs nothing. */
    {
        typedef unsigned (WINAPI *tbp_fn)(unsigned);
        HMODULE winmm = LoadLibraryA("winmm.dll");
        tbp_fn tbp = winmm ? (tbp_fn)(void *)GetProcAddress(winmm,
                                                           "timeBeginPeriod")
                           : NULL;
        if (tbp)
            tbp(1);
    }
#endif

    kernel_timer_loop_ns();
    return 0;
}

/* Shared by KeSetTimer and KeSetTimerEx; period is 0 for the former. */
static void kernel_set_timer(uint32_t timer_va, long long due_100ns,
                             long period_ms, uint32_t dpc_va)
{
    long long delay_ms = (due_100ns < 0) ? (-due_100ns) / 10000 : 0;
    int i, free_slot = -1;
    uint32_t was_set = 0;

    kernel_timer_start_once();

    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < XBOX_MAX_TIMERS; i++) {
        if (g_timers[i].timer_va == timer_va) { free_slot = i; was_set = 1; break; }
        if (!g_timers[i].timer_va && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_timers[free_slot].timer_va  = timer_va;
        g_timers[free_slot].dpc_va    = dpc_va;
        g_timers[free_slot].due_ms    = kernel_timer_clock_ms() + delay_ms;
        g_timers[free_slot].period_ms = period_ms;
    }
    LeaveCriticalSection(&g_timer_lock);
    if (free_slot < 0) {
        static volatile LONG said;
        if (InterlockedExchange(&said, 1) == 0)
            fprintf(stderr, "  [KERNEL] KeSetTimer: all %d kernel timers are armed; timer "
                            "0x%08X will not fire (said once)\n", XBOX_MAX_TIMERS, timer_va);
    }

    /* A freshly set timer starts unsignaled, like the real KeSetTimer. */
    {
        HANDLE ev = ke_shadow_lookup(timer_va);
        if (ev)
            ResetEvent(ev);
    }
    g_eax = was_set;
}

static void bridge_KeSetTimer(void)
{
/* LARGE_INTEGER is two stack slots. */
    long long due = (long long)((uint64_t)STACK_ARG(1)
                              | ((uint64_t)STACK_ARG(2) << 32));
    kernel_set_timer(STACK_ARG(0), due, 0, STACK_ARG(3));
}

static void bridge_KeSetTimerEx(void)
{
    long long due = (long long)((uint64_t)STACK_ARG(1)
                              | ((uint64_t)STACK_ARG(2) << 32));
    kernel_set_timer(STACK_ARG(0), due, (long)STACK_ARG(3), STACK_ARG(4));
}

/* Drop a timer so it stops firing. Returns whether it was armed. */
int xbox_kernel_cancel_timer(uint32_t timer_va)
{
    int i, was_set = 0;

    if (!g_timer_started)
        return 0;
    EnterCriticalSection(&g_timer_lock);
    for (i = 0; i < XBOX_MAX_TIMERS; i++)
        if (g_timers[i].timer_va == timer_va) {
            g_timers[i].timer_va = 0;
            was_set = 1;
        }
    LeaveCriticalSection(&g_timer_lock);
    return was_set;
}

/* ── ExQueryPoolBlockSize (ordinal 24) ────────────────────
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryPoolBlockSize(void)
{
    /* Pool blocks come from xbox_HeapAlloc, so the block table has the real
     * answer. It used to return a literal 0 on the theory that this is only
     * ever used for stats -- which is a guess about the caller, and a title
     * that sizes a copy from it copies nothing. */
    g_eax = xbox_HeapBlockSize(STACK_ARG(0));
}

/* ── RtlNtStatusToDosError (ordinal 301) ─────────────────
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 */
static void bridge_RtlNtStatusToDosError(void)
{
    uint32_t status = STACK_ARG(0);

    /* Simple mapping of common status codes */
    switch (status) {
    case 0x00000000: g_eax = 0; break;          /* STATUS_SUCCESS → ERROR_SUCCESS */
    case 0xC0000034: g_eax = 2; break;          /* STATUS_OBJECT_NAME_NOT_FOUND → ERROR_FILE_NOT_FOUND */
    case 0xC000003A: g_eax = 3; break;          /* STATUS_OBJECT_PATH_NOT_FOUND → ERROR_PATH_NOT_FOUND */
    case 0xC0000022: g_eax = 5; break;          /* STATUS_ACCESS_DENIED → ERROR_ACCESS_DENIED */
    case 0xC0000008: g_eax = 6; break;          /* STATUS_INVALID_HANDLE → ERROR_INVALID_HANDLE */
    case 0xC0000017: g_eax = 8; break;          /* STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY */
    case 0xC000000D: g_eax = 87; break;         /* STATUS_INVALID_PARAMETER → ERROR_INVALID_PARAMETER */
    /* The informational and warning codes, which are not failures and must
     * not fall through to the generic answer.
     *
     * 317 is ERROR_MR_MID_NOT_FOUND -- "there is no message text for this
     * number" -- and as a default for a status nobody has mapped yet it is
     * honest. As an answer to "is this request still in flight?" it is not:
     * a caller comparing against ERROR_IO_PENDING gets "no" and takes the
     * branch for a request that never started.
     *
     * Shin Megami Tensei: Nine does exactly that. Its resource loader issues
     * a read, sees the call fail, asks for the error, and marks the object as
     * loading only when the answer is ERROR_IO_PENDING. With 317 the object
     * stayed idle, the poll that finishes the load returned "not started" on
     * every frame, and the title sat in its first boot state forever with
     * everything else working. */
    case 0x00000103: g_eax = 997; break;        /* STATUS_PENDING → ERROR_IO_PENDING */
    case 0x00000102: g_eax = 1460; break;       /* STATUS_TIMEOUT → ERROR_TIMEOUT */
    case 0x00000104: g_eax = 0; break;          /* STATUS_REPARSE → ERROR_SUCCESS */
    case 0x80000005: g_eax = 234; break;        /* STATUS_BUFFER_OVERFLOW → ERROR_MORE_DATA */
    case 0x80000006: g_eax = 18; break;         /* STATUS_NO_MORE_FILES → ERROR_NO_MORE_FILES */
    case 0xC0000011: g_eax = 38; break;         /* STATUS_END_OF_FILE → ERROR_HANDLE_EOF */
    case 0xC0000023: g_eax = 122; break;        /* STATUS_BUFFER_TOO_SMALL → ERROR_INSUFFICIENT_BUFFER */
    case 0xC0000035: g_eax = 183; break;        /* STATUS_OBJECT_NAME_COLLISION → ERROR_ALREADY_EXISTS */
    case 0xC00000BB: g_eax = 50; break;         /* STATUS_NOT_SUPPORTED → ERROR_NOT_SUPPORTED */
    /* The CRT heap-grow path retries at a new address only on
     * ERROR_INVALID_ADDRESS; any other answer makes it give up. */
    case 0xC0000018: g_eax = 487; break;        /* STATUS_CONFLICTING_ADDRESSES → ERROR_INVALID_ADDRESS */

    default:         g_eax = 317; break;         /* ERROR_MR_MID_NOT_FOUND (generic) */
    }
}

/* ── File I/O bridge helpers ─────────────────────────────── */

/*
 * Xbox structures use 32-bit pointers. On Win64, the C structs
 * (XBOX_OBJECT_ATTRIBUTES, etc.) have 64-bit pointers, so we can't
 * cast Xbox memory to them directly. Instead, parse the 32-bit
 * Xbox layout manually:
 *
 * XBOX_OBJECT_ATTRIBUTES (12 bytes):
 *   offset 0: RootDirectory  (uint32_t)
 *   offset 4: ObjectName     (uint32_t, Xbox VA to ANSI_STRING)
 *   offset 8: Attributes     (uint32_t)
 *
 * XBOX_ANSI_STRING (8 bytes):
 *   offset 0: Length          (uint16_t)
 *   offset 2: MaximumLength   (uint16_t)
 *   offset 4: Buffer          (uint32_t, Xbox VA to char[])
 *
 * XBOX_IO_STATUS_BLOCK (8 bytes):
 *   offset 0: Status          (uint32_t)
 *   offset 4: Information     (uint32_t)
 */

/* Extract the ANSI path string from an Xbox OBJECT_ATTRIBUTES */
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    uint32_t ansi_str_va, buf_va;
    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;
    /* XDK directory searches pass a counted prefix of "directory\\*".
     * The byte after Length need not be NUL or part of the object name. */
    static RECOMP_TLS char path[65536];
    uint16_t length=BRIDGE_MEM16(ansi_str_va);
    if(length>BRIDGE_MEM16(ansi_str_va+2)) return NULL;
    /* Length <= MaximumLength says the string is self-consistent; it does not
     * say the buffer it names is inside the mapping. Without this, a string
     * near the top of guest memory reads up to 64 KB off the end. */
    if (length && !bridge_va_mapped(buf_va, length)) return NULL;
    memcpy(path,XBOX_TO_NATIVE(buf_va),length);
    path[length]='\0';
    return path;
}

/* Write NTSTATUS + Information into Xbox IO_STATUS_BLOCK */
static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

/*
 * Handle table.
 *
 * Xbox memory only has 32-bit handle slots, but native HANDLEs are 64-bit
 * pointers (win32_compat objects, or real Win32 handles on Windows). Map
 * 32-bit tokens <-> native HANDLEs so a handle survives a round-trip through
 * Xbox memory. Tokens carry a tag in the high byte so they never collide
 * with the synthetic handles (0xDEAD0001 / 0xBEEF0010) used elsewhere.
 */
#define BRIDGE_HANDLE_TAG  0x48000000u
#define BRIDGE_HANDLE_MASK 0x00FFFFFFu
#define BRIDGE_HANDLE_MAX  16384
/* thobj_* above spell these out because it comes first. */
typedef char thobj_tag_matches[(BRIDGE_HANDLE_TAG == 0x48000000u &&
                                BRIDGE_HANDLE_MAX == 16384) ? 1 : -1];
static HANDLE s_handle_table[BRIDGE_HANDLE_MAX];

/* Every guest thread allocates and frees tokens. Without a lock, two threads
 * that ask at the same moment can both take the same free slot and get the
 * same token for different HANDLEs. A title hit that: one token named both a
 * new loader thread and a file a worker had open. When the worker closed its
 * file, the thread lost its guest object, and the main thread's
 * GetExitCodeThread poll never saw the thread end. */
static volatile LONG s_handle_lock;

static void handle_lock(void)
{
    while (InterlockedCompareExchange(&s_handle_lock, 1, 0) != 0)
        SwitchToThread();
}

static void handle_unlock(void) { InterlockedExchange(&s_handle_lock, 0); }

/* A token for h. The same HANDLE normally gets the same token. A duplicate
 * must not: on POSIX, DuplicateHandle returns its source pointer with one
 * more reference. If the duplicate shared the source's token, NtClose on
 * either one would clear the slot that both of them use. */
/* Both searches below ran over all 16,384 slots under the spin lock, so a
 * new handle (never in the table) read 128 KB twice: ~10 us per
 * NtCreateFile / NtCreateEvent / thread, with every other guest thread
 * spinning meanwhile (KRN-11). The table now keeps two bounds, both under
 * handle_lock: s_handle_hi, one past the highest slot in use, which bounds
 * the "same HANDLE" search; and s_handle_free, below which no slot is free,
 * where the free search starts. In practice both stay near the number of
 * handles open at once. */
static uint32_t s_handle_hi = 1, s_handle_free = 1;

static void handle_slot_set(uint32_t i, HANDLE h)    /* under handle_lock */
{
    s_handle_table[i] = h;
    if (h) {
        if (i >= s_handle_hi)
            s_handle_hi = i + 1;
        if (i == s_handle_free)
            s_handle_free = i + 1;
    } else {
        if (i < s_handle_free)
            s_handle_free = i;
        while (s_handle_hi > 1 && !s_handle_table[s_handle_hi - 1])
            s_handle_hi--;
    }
}

static uint32_t bridge_handle_token_ex(HANDLE h, int fresh)
{
    uint32_t i;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    /* The current-thread pseudo-handle names whichever thread uses it: in
     * the table it would follow every later caller, and a save state cannot
     * carry it. Nothing should hand it over; refuse it if something does. */
    if (h == (HANDLE)(intptr_t)-2) {
        static int logged = 0;
        if (!logged++) {
            fprintf(stderr, "  [BRIDGE] refusing to tokenize the current-thread "
                    "pseudo-handle\n");
            fflush(stderr);
        }
        return 0;
    }
    handle_lock();
    for (i = 1; !fresh && i < s_handle_hi; i++)
        if (s_handle_table[i] == h) {
            handle_unlock();
            return BRIDGE_HANDLE_TAG | i;
        }
    for (i = s_handle_free ? s_handle_free : 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == NULL) {
            handle_slot_set(i, h);
            handle_unlock();
            return BRIDGE_HANDLE_TAG | i;
        }
    handle_unlock();
    fprintf(stderr, "  [BRIDGE] handle table full\n");
    return 0;
}

static uint32_t bridge_handle_token(HANDLE h)
{
    return bridge_handle_token_ex(h, 0);
}

/* Store a native HANDLE into a 32-bit Xbox memory slot (as a token). */
static void bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    if (handle_va)
        BRIDGE_MEM32(handle_va) = bridge_handle_token(h);
}

/* Resolve a 32-bit Xbox handle slot back to a native HANDLE. */
static HANDLE bridge_read_handle(uint32_t va)
{
    uint32_t token = BRIDGE_MEM32(va);
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged value: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

/* Resolve a token to a HANDLE and release its table slot (for NtClose). */
/* Resolve a handle token passed BY VALUE, without consuming it.
 *
 * Three accessors, easily confused, and confusing two of them broke all file
 * I/O: bridge_read_handle(va) reads a token *from memory* and suits a PHANDLE
 * out-parameter; bridge_take_handle(token) resolves and CLEARS the table slot,
 * which is NtClose semantics; this one resolves and leaves the slot alone,
 * which is what every by-value HANDLE argument needs.
 *
 * NtSetInformationFile and friends take the handle by value, but were calling
 * bridge_read_handle on it -- dereferencing the token as if it were an address.
 * Halo created its save file successfully and then failed the very next call,
 * which surfaced as "couldn't open or create saved game file". */
static HANDLE bridge_resolve_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Pseudo-handles (NtCurrentProcess() = -1, NtCurrentThread() = -2) are
     * negative. Zero-extending them on a 64-bit host yields 0x00000000FFFFFFFE,
     * which Win32 rejects: X-Men Legends' CRT duplicates NtCurrentThread()
     * and spun forever on the STATUS_UNSUCCESSFUL that came back. */
    if (token >= 0xFFFFFFF0u)
        return (HANDLE)(intptr_t)(int32_t)token;
    /* Untagged: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

static HANDLE bridge_take_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX) {
            HANDLE h;
            handle_lock();
            h = s_handle_table[i];
            handle_slot_set(i, NULL);
            handle_unlock();
            return h;
        }
    }
    return NULL;   /* untagged -> not a table handle, do not close */
}

/* Build a native OBJECT_ATTRIBUTES wrapping the translated Xbox path. */
static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    const char* path = bridge_get_xbox_path(obj_attrs_va);
    name->Buffer        = (PCHAR)path;
    name->Length        = path ? (USHORT)strlen(path) : 0;
    name->MaximumLength = (USHORT)(name->Length + 1);
    uint32_t root = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va) : 0;
    /* -3 is the XDK DOS-device namespace, not a file handle. */
    oa->RootDirectory = root && root != 0xFFFFFFFDu ? bridge_resolve_handle(root) : NULL;
    oa->ObjectName    = name;
    oa->Attributes    = 0;
}

/*
 * The DVD drive as a device, not as a directory.
 *
 * A title that checks its media opens "\\Device\\CdRom0" itself -- the bare
 * device, with nothing after it -- and then issues IOCTLs on the handle. The
 * path table in kernel_path.c only carries the "\\Device\\CdRom0\\" form with
 * the separator, which is the prefix for reading a *file* off the disc, so the
 * bare open matched no rule, was reported as "Unrecognized Xbox path", and came
 * back STATUS_OBJECT_PATH_NOT_FOUND. DDS9 reads that as "no disc" and exits
 * through HalReturnToFirmware before it draws a frame.
 *
 * There is nothing on the host to open here: the game directory is a
 * directory, and a directory handle would not answer the IOCTLs that follow.
 * So the open returns a synthetic handle, in the same style as the ones
 * NtCreateDirectoryObject and the partition devices already hand out. It is
 * deliberately untagged, which bridge_resolve_handle passes through unchanged,
 * and distinct so bridge_NtDeviceIoControlFile can recognise it by value.
 *
 * Accepts the "\??\" prefix, since titles reach the device both ways.
 */
#define BRIDGE_CDROM_HANDLE 0xDECD0001u

static int bridge_is_cdrom_device(const char *path)
{
    if (!path) return 0;
    if (_strnicmp(path, "\\??\\", 4) == 0) path += 4;
    return _stricmp(path, "\\Device\\CdRom0") == 0;
}

/* Open a file by delegating to the ported xbox_NtCreateFile kernel HLE.
 * bridge_create_file_impl, below, wraps this with the per-attempt log. */
static NTSTATUS bridge_create_file_open(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options, XBOX_ANSI_STRING *name_out)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;
    XBOX_IO_STATUS_BLOCK   ios;
    HANDLE   h  = NULL;
    NTSTATUS st;

    bridge_build_oa(obj_attrs_va, &oa, &name);
    *name_out = name;
    if (!name.Buffer) {
        bridge_write_iostatus(iostatus_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    if (bridge_is_cdrom_device(name.Buffer)) {
        fprintf(stderr, "  [FILE] %s -> synthetic DVD device handle\n",
                name.Buffer);
        if (handle_va)
            BRIDGE_MEM32(handle_va) = BRIDGE_CDROM_HANDLE;
        bridge_write_iostatus(iostatus_va, 0, 1 /* FILE_OPENED */);
        return 0;
    }

    memset(&ios, 0, sizeof(ios));

    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL,
                           file_attrs, share, disposition, options);

    if (NT_SUCCESS(st)) {
        bridge_write_handle(handle_va, h);
        bridge_write_iostatus(iostatus_va, ios.Status, (uint32_t)ios.Information);
    } else {
        bridge_write_iostatus(iostatus_va, st, 0);
    }
    return st;
}

/* The per-call [READ] and [FILE] success lines (KRN-5).
 *
 * They printed and flushed on every guest read and every successful
 * NtCreateFile. On Android stderr is a pipe a thread pumps to logcat
 * (android_log.h), so a title streaming in small reads paid a formatted
 * line and a write per read on the thread whose stall is audio dry time,
 * and blocked when the 64 KB pipe filled. Now the first IO_LOG_FIRST of
 * each kind print, then only failures, unless RECOMP_READ_LOG=1 (every
 * read) or RECOMP_MISSING_FILES=all (every open); RECOMP_READ_LOG=0 drops
 * the successful ones from the start. The [READ] window summary still
 * gives the health figure every 10 s. */
#define IO_LOG_FIRST 64

static int bridge_read_log_mode(void)       /* 0 never, 1 first N, 2 every */
{
    static volatile int mode = -1;
    if (mode < 0) {
        const char *v = getenv("RECOMP_READ_LOG");
        mode = !v || !*v ? 1 : (v[0] == '0' ? 0 : 2);
    }
    return mode;
}

static int bridge_io_log_success(volatile LONG *count, int every)
{
    int mode = bridge_read_log_mode();
    if (every || mode == 2)
        return 1;
    if (mode == 0)
        return 0;
    return InterlockedIncrement(count) <= IO_LOG_FIRST;
}

/* Every guest open -- NtCreateFile, NtOpenFile, IoCreateFile -- comes through
 * here, so this is where each attempt is logged and where the missing-file
 * report (kernel_missing.c) sees it. NtOpenFile and IoCreateFile used to
 * print nothing at all. Their failures print now, since the report needs
 * them; their successes only under RECOMP_MISSING_FILES=all, because a
 * title that streams through IoCreateFile would otherwise log
 * every open by default. ntcreate: the caller is NtCreateFile, whose
 * success line has always printed. */
static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options, int ntcreate)
{
    XBOX_ANSI_STRING name;
    NTSTATUS st = bridge_create_file_open(handle_va, access, obj_attrs_va,
                                          iostatus_va, file_attrs, share,
                                          disposition, options, &name);
    uint32_t status = (uint32_t)st;
    static volatile LONG opens_logged;
    int printed = 0;

    /* Paired with the [PATH] line the translation just printed: that says what
     * was asked for, this says whether it opened. A failed open is not itself
     * a bug -- a title probing the cache partition before the disc expects one
     * -- so the status is what separates a probe from a real miss. */
    {
        extern uint32_t xbox_LastFileError(void);
        uint32_t _e = status ? xbox_LastFileError() : 0u;
        /* The guest's own path, read from its OBJECT_ATTRIBUTES. The POSIX
         * path backend prints no [PATH] line, so without this a failed open
         * on macOS said that something was missing but not what. */
        char gpath[160] = "?";
        if (name.Buffer) {
            uint32_t len = name.Length, k;
            if (len >= sizeof gpath) len = sizeof gpath - 1;
            for (k = 0; k < len; k++)
                gpath[k] = name.Buffer[k];
            gpath[len] = 0;
        }
        if (status) {
            /* Once a path has its "[FILE] missing" line, its later attempts
             * are counted rather than printed: a looping stream re-opens a
             * missing file every loop (97 lines in 100 s on one title). The
             * report decides; with it off, every attempt prints as before. */
            if (!xbox_missing_is_reported(gpath) && (printed = 1))
                fprintf(stderr, "  [FILE] %s (disp %u) -> 0x%08X FAILED (win32 err=%u%s)\n",
                        gpath, (unsigned)disposition, status, _e,
                        _e == 32u ? " ERROR_SHARING_VIOLATION"
                      : _e ==  2u ? " ERROR_FILE_NOT_FOUND"
                      : _e ==  3u ? " ERROR_PATH_NOT_FOUND" : "");
            if (name.Buffer)
                xbox_missing_note(gpath, status, (uint32_t)access,
                                  (uint32_t)disposition, (uint32_t)options, 0);
        } else if (xbox_missing_verbose()
                   || (ntcreate && bridge_io_log_success(&opens_logged, 0))) {
            fprintf(stderr, "  [FILE] %s (disp %u opts 0x%X from 0x%08X) -> 0x%08X\n",
                    gpath, (unsigned)disposition, (unsigned)options,
                    g_xbox_kernel_caller, status);
            printed = 1;
        }
    }
    if (printed)
        fflush(stderr);
    return st;
}

/* ── RtlInitAnsiString (ordinal 289) ──────────────────────
 * VOID RtlInitAnsiString(PANSI_STRING Destination, PCSZ Source)
 *
 * Fills an ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; }.
 * Unbridged this returned 0 and wrote nothing, so every path a title built
 * this way arrived at NtCreateFile as a null Buffer and failed with
 * STATUS_OBJECT_PATH_NOT_FOUND -- which looks like a missing file rather than
 * a missing bridge. Halo builds its map paths exactly this way.
 */
/* -- RtlEqualString (ordinal 279, 3 args) ----------------
 * BOOLEAN RtlEqualString(PSTRING String1, PSTRING String2, BOOLEAN CaseInSens)
 *
 * The fields are read out by hand rather than casting the guest struct. A
 * guest ANSI_STRING is {USHORT Length, USHORT MaximumLength, 32-bit Buffer},
 * eight bytes; the native one has a 64-bit PCHAR, so a cast would read
 * MaximumLength and Buffer from the wrong offsets and then dereference a guest
 * VA as a host address. RtlInitAnsiString stores a guest VA in that field --
 * see the bridge below -- so it has to be translated, not passed through.
 *
 * Stubbed, this returned 0: "never equal". Wreckless initialises a string and
 * compares it in a critical-section-protected lookup, so every comparison
 * missing turned that lookup into unbounded recursion and the process died of
 * a host stack overflow 200 kernel calls in.
 */
static void bridge_RtlEqualString(void)
{
    uint32_t s1_va  = STACK_ARG(0);
    uint32_t s2_va  = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    XBOX_ANSI_STRING a, b;

    if (!s1_va || !s2_va) {
        g_eax = 0;
        return;
    }
    a.Length        = BRIDGE_MEM16(s1_va + 0);
    a.MaximumLength = BRIDGE_MEM16(s1_va + 2);
    a.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    b.Length        = BRIDGE_MEM16(s2_va + 0);
    b.MaximumLength = BRIDGE_MEM16(s2_va + 2);
    b.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));

    if (!a.Buffer || !b.Buffer) {
        g_eax = 0;
        return;
    }
    g_eax = xbox_RtlEqualString(&a, &b, (BOOLEAN)nocase) ? 1 : 0;
}

static void bridge_RtlInitAnsiString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const char *src = (const char *)XBOX_TO_NATIVE(src_va);
        size_t len = strlen(src);
        if (len > 0xFFFE) {
            len = 0xFFFE;
        }
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)len;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(len + 1);
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── NtCreateFile (ordinal 190, 9 args = 36 bytes) ─────── */
static void bridge_NtCreateFile(void)
{
    uint32_t handle_va   = STACK_ARG(0);  /* PHANDLE */
    uint32_t access      = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs   = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus    = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    /* arg4: AllocationSize - ignored */
    uint32_t file_attrs  = STACK_ARG(5);  /* FileAttributes */
    uint32_t share       = STACK_ARG(6);  /* ShareAccess */
    uint32_t disposition = STACK_ARG(7);  /* CreateDisposition */
    uint32_t options     = STACK_ARG(8);  /* CreateOptions */

    /* The out-parameter addresses matter as much as the result: this bridge
     * hands them to a real Win32 call, so a bogus one has Windows itself write
     * into Xbox memory. That is how a wild write ends up with a stack inside
     * ntdll and no recompiled frame to blame. */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options, 1);

    /* FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT. Neither set
     * means the caller wants asynchronous completion on this handle. */
    if (g_eax == 0 && handle_va && (options & 0x30u) == 0u)
        bridge_note_async_handle(BRIDGE_MEM32(handle_va));

    /* An FMV the host can decode itself.
     *
     * The title's own decoder is emulated like everything else, but it only
     * produces pixels once there is something to execute its GPU work -- so on
     * a bring-up where that does not exist yet, the video the game just asked
     * for can still be shown. The trigger is the title opening the file, so
     * this plays when the game decides to play it, not on a timer, and it
     * plays the file the game chose.
     *
     * Off unless RECOMP_FMV_HOST is set: it is a substitute for the title's
     * own output, and that should be a decision rather than a default. */
    if (g_eax == 0 && getenv("RECOMP_FMV_HOST")) {
        /* Declared here rather than included: the player lives in xbox_video,
         * which links xbox_d3d8, and having the kernel include its header
         * would make the dependency circular for no gain. Both land in the
         * same executable. */
        extern int xbox_VideoPlayFile(const char *host_path);
        extern int xbox_VideoIsPlaying(void);

        char host[MAX_PATH * 2];
        size_t n = 0;

        const wchar_t *w = xbox_LastHostPath();

        while (n < sizeof(host) - 1 && w[n]) {
            host[n] = (char)w[n];
            n++;
        }
        host[n] = 0;
        if (n > 4 && _stricmp(host + n - 4, ".wmv") == 0
                && !xbox_VideoIsPlaying())
            xbox_VideoPlayFile(host);
    }

    /* An XMV movie: the runtime's decoder (src/video/xmv_hle.c) plays the
     * file the title opened last when a title layer hands it the XDK's
     * XMVDecoder_GetNextFrame (XBA-70). Only the name is kept here. */
    if (g_eax == 0) {
        extern void xbox_MovieFileOpened(const char *host_path);
        const wchar_t *w = xbox_LastHostPath();
        size_t n = 0;

        while (w[n])
            n++;
        if (n > 4 && w[n - 4] == L'.' && (w[n - 3] | 0x20) == L'x' && (w[n - 2] | 0x20) == L'm'
                && (w[n - 1] | 0x20) == L'v') {
            char host[MAX_PATH * 2];
            size_t k;
            for (k = 0; k < n && k < sizeof(host) - 1; k++)
                host[k] = (char)w[k];
            host[k] = 0;
            xbox_MovieFileOpened(host);
        }
    }
}

/* ── NtOpenFile (ordinal 202, 6 args = 24 bytes) ──────── */
static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);  /* PHANDLE */
    uint32_t access    = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus  = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    uint32_t share     = STACK_ARG(4);  /* ShareAccess */
    uint32_t options   = STACK_ARG(5);  /* OpenOptions */

    /* NtOpenFile = NtCreateFile with FILE_OPEN disposition */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1 /* FILE_OPEN */, options, 0);
}

/*
 * Completion for a file request that carried an Event or an APC routine.
 *
 * Both bridges below do the I/O synchronously, and used to drop args 1-3
 * (Event, ApcRoutine, ApcContext) on the floor. A title that issues an async
 * request and waits alertably for the completion then waits forever: Halo's
 * cache-partition setup does exactly that, gives up after its 5-second SleepEx,
 * and asserts "setup for new cache file failed (#0)".
 *
 * The APC runs at the issuing thread's next alertable user-mode wait, as on
 * the hardware (bridge_run_io_apcs), not inline here: a title that depends on
 * the APC *not* having run yet notices, and Fallout: Brotherhood of Steel and
 * Fable do. RECOMP_INLINE_APC=1 (or xbox_SetDeferredIoApc(0)) runs it inline,
 * the old default.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

static void deliver_one_apc(uint32_t apc_routine, uint32_t apc_context,
                            uint32_t iostatus)
{
    /* The APC can be game code or a kernel export. Halo's XAPI passes the
     * latter -- 0xFE0000FC, one of our own synthetic thunk VAs -- so the recomp
     * dispatch correctly fails to find it and the kernel fallback is the one
     * that matters. Checking only recomp_lookup left it undelivered. */
    recomp_func_t fn = recomp_lookup(apc_routine);
    if (!fn) fn = recomp_lookup_manual(apc_routine);
    if (!fn) fn = recomp_lookup_kernel(apc_routine);
    if (fn) {
        /* VOID NTAPI ApcRoutine(PVOID ApcContext, PIO_STATUS_BLOCK, ULONG)
         *
         * Restore esp from before the frame rather than adding a fixed amount
         * back. The routine is stdcall and pops its own 16 bytes (a kernel
         * thunk does the same), so the old `g_esp += 12` left the caller 12
         * bytes high. Callers ending in `leave` never noticed; FBoS's wait
         * wrapper sub_000907CB does `pop esi` first and came back with a
         * stack address in its caller's esi. */
        CallbackRegs saved;
        callback_regs_save(&saved);
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = apc_context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;   /* dummy return address */
        {
            /* An APC runs in its thread's context: it holds the guest CPU. */
            int took = xbox_GuestCpuEnterCallback();
            fn();
            xbox_GuestCpuLeaveCallback(took);
        }
        callback_regs_restore(&saved, 0);
    } else {
        uint32_t ord = 0;
        if (apc_routine >= KERNEL_VA_BASE && apc_routine < KERNEL_VA_END) {
            ord = g_slot_ordinals[(apc_routine - KERNEL_VA_BASE) / 4];
        }
        fprintf(stderr, "  [KERNEL] file I/O APC 0x%08X unresolved"
                " (kernel ordinal %u)\n", apc_routine, ord);
        fflush(stderr);
    }
}

/* A request that fails synchronously -- an error status (NT_ERROR) the call
 * returns itself, not STATUS_PENDING -- queues no APC: the I/O manager
 * completes it without a completion APC and the caller learns of it from
 * the return value. XAPI's ReadFileEx then returns FALSE and its caller does
 * not expect the completion routine; running it as well handled the failure
 * twice (XBA-133). The event is still set and the status block written, as
 * before: a title that waits on it regardless does not hang here. */
static int bridge_io_failed_sync(uint32_t status, int pending)
{
    return ((status >> 30) == 3u) && !pending;
}

/* Per-thread pending-APC ring. An APC is delivered on the thread that issued
 * the request, which is also the thread that waits, so thread-local is right.
 * The status block is written and the event set before this, so a title that
 * polls either, or waits on the event without being alertable, sees the
 * request complete whether or not its APC has run -- as on the console.
 *
 * A file joined to an I/O completion object (FileCompletionInformation)
 * posts a request that names no APC routine but an ApcContext -- XAPI's
 * overlapped ReadFile passes the OVERLAPPED there -- to that object, with
 * the file's key and the request's status and byte count (XBA-114). */
static void bridge_complete_file_io(uint32_t file_token, uint32_t event_token,
                                    uint32_t apc_routine, uint32_t apc_context,
                                    uint32_t iostatus, uint32_t status,
                                    uint32_t info, int pending)
{
    if (event_token) {
        HANDLE ev = bridge_resolve_handle(event_token);
        if (ev) SetEvent(ev);
    }
    if (bridge_io_failed_sync(status, pending))
        return;
    if (!apc_routine && apc_context)
        ks_port_post_for_file(bridge_resolve_handle(file_token), apc_context,
                              status, info);
    if (apc_routine) {
        if (defer_io_apc() && g_io_apc_count < IO_APC_QUEUE) {
            uint32_t *q = g_io_apc[g_io_apc_count++];
            q[0] = apc_routine; q[1] = apc_context; q[2] = iostatus;
        } else {
            if (defer_io_apc()) {
                static int told;
                if (!told++)
                    fprintf(stderr, "  [KERNEL] more than %d file I/O APCs queued"
                                    " on one thread without an alertable wait:"
                                    " the next ones run inline\n", IO_APC_QUEUE);
            }
            deliver_one_apc(apc_routine, apc_context, iostatus);
        }
    }
}

/* An alertable user-mode wait with APCs queued runs them all and returns
 * STATUS_USER_APC without waiting, as the real kernel does. A non-alertable or
 * kernel-mode wait leaves them queued.
 *
 * "All" means both of the thread's user APC sources (XBA-102 x XBA-113): the
 * file-I/O completions of this ring, then the APCs KeInsertQueueApc queued on
 * its KTHREAD (kernel_ksync.c), each in its own order. On the console both
 * are one FIFO; the two lists here have no common clock, so the ring goes
 * first. Before, a wait answered by one source left the other for a later
 * wait. bridge_apc_safe_point answers through the same helper.
 *
 * Checked before the object, even one already signalled. The console tests the
 * object first, but there the request is still in flight when its thread
 * starts waiting and the completion's APC ends the wait; here the request
 * completed inside the call, so APC-first is what reproduces the console's
 * timeline. Object-first, a wait on the file handle of a finished read would
 * return at once with the APC still queued. Fallout: Brotherhood
 * of Steel's streamer (sub_00038EE0) marks a slot busy after ReadFileEx and
 * waits on the file handle for the completion to clear it; run inline, the
 * completion cleared the flag before it was set and the wait never ended.
 *
 * The queue is copied out first: a completion routine may issue the next read,
 * which queues again, and that one belongs to the next wait. */
static uint32_t thobj_current(void);
static void bridge_apc_deliver(uint32_t apc);

static int bridge_run_user_apcs(void)
{
    uint32_t q[IO_APC_QUEUE][3];
    int n = g_io_apc_count, i, ran = 0;

    if (n) {
        memcpy(q, g_io_apc, sizeof(q[0]) * n);
        g_io_apc_count = 0;
        for (i = 0; i < n; i++)
            deliver_one_apc(q[i][0], q[i][1], q[i][2]);
        ran = 1;
    }
    if (ks_apc_pending()) {
        uint32_t kt = thobj_current(), apc;
        while ((apc = ks_apc_take(kt, 1)) != 0) {
            bridge_apc_deliver(apc);
            ran = 1;
        }
    }
    if (ran)
        g_eax = 0x000000C0u;             /* STATUS_USER_APC */
    return ran;
}

/* ── Waking a thread blocked in an alertable wait (XBA-133) ──
 *
 * A user APC queued for another thread (KeInsertQueueApc, NtQueueApcThread)
 * and an alert (KeAlertThread) end that thread's alertable wait on the
 * console. Here the thread is blocked in a host wait, so each guest thread
 * that makes an alertable wait has an entry: its KTHREAD, a handle to its
 * host thread (w32_thread_apc_handle), what kind of alertable wait it is in
 * now, and its Alerted flags (KTHREAD.Alerted[mode] on the console). The
 * waker queues a no-op host APC to the thread, which ends its host wait
 * (WAIT_IO_COMPLETION, STATUS_ALERTED); the thread then runs its user APCs
 * and the wait returns STATUS_USER_APC, or STATUS_ALERTED for an alert.
 *
 * Each of the wait bridges calls bridge_run_io_apcs before its host wait
 * (bridge_wait_begin) and bridge_wait_done after it. begin marks the wait,
 * throws away a host APC left over from an earlier wake (one that came
 * after its wait had ended for another reason), then answers the wait
 * before blocking: STATUS_ALERTED when the thread is alerted for this mode,
 * STATUS_USER_APC when a user-mode wait finds user APCs queued. A waker
 * looks at the mark after queueing, and begin looks at the queues after
 * marking, so an APC is either seen by begin or wakes the host wait.
 *
 * A user APC wakes only a user-mode alertable wait, a user-mode alert too;
 * a kernel-mode alert wakes either, as on the console. RECOMP_APC_WAKE=0
 * turns the waking off (the old behaviour: the APC runs at the thread's
 * next alertable wait). */
#define THALERT_MAX 512

typedef struct {
    uint32_t kthread;           /* 0: free */
    HANDLE   host;              /* the host thread, NULL until it waited */
    HANDLE   src;               /* the thread's own handle it was duplicated from */
    int      waiting;           /* 0, or 1 + KPROCESSOR_MODE of the alertable wait */
    uint8_t  alerted[2];        /* KernelMode, UserMode */
} ThAlert;

static ThAlert s_thalert[THALERT_MAX];
static volatile LONG s_thalert_lock;

static void thalert_lock(void)
{
    while (InterlockedCompareExchange(&s_thalert_lock, 1, 0) != 0)
        SwitchToThread();
}

static void thalert_unlock(void) { InterlockedExchange(&s_thalert_lock, 0); }

static int apc_wake_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APC_WAKE");
        on = !(e && *e == '0');
    }
    return on;
}

/* The entry of `kthread`, made when `create`. Lock held. */
static ThAlert *thalert_find(uint32_t kthread, int create)
{
    ThAlert *free_e = NULL;
    int i;
    if (!kthread)
        return NULL;
    for (i = 0; i < THALERT_MAX; i++) {
        if (s_thalert[i].kthread == kthread)
            return &s_thalert[i];
        if (!s_thalert[i].kthread && !free_e)
            free_e = &s_thalert[i];
    }
    if (create && free_e) {
        memset(free_e, 0, sizeof *free_e);
        free_e->kthread = kthread;
    }
    return create ? free_e : NULL;
}

static VOID CALLBACK thalert_noop(ULONG_PTR unused) { (void)unused; }

/* The calling thread's KTHREAD, as thobj_current, but never made here: a
 * thread whose TIB names none has nothing a waker could name either. */
static uint32_t thalert_self_kt(void)
{
    if (g_thread_obj_va)
        return g_thread_obj_va;
    return g_fs_base ? BRIDGE_MEM32(g_fs_base + 0x28) : 0;
}

static RECOMP_TLS HANDLE t_thalert_host;   /* this thread's, once taken */

static HANDLE thalert_host_self(void)
{
    if (!t_thalert_host) {
#ifdef _WIN32
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &t_thalert_host, 0, FALSE, DUPLICATE_SAME_ACCESS);
#else
        t_thalert_host = w32_thread_apc_handle();
#endif
    }
    return t_thalert_host;
}

/* The calling thread starts (kind 1 + mode) or ends (0) an alertable wait.
 * Starting answers the thread's Alerted flag for that mode: 1 when it was
 * set (and is now clear). */
static int thalert_mark(int kind)
{
    uint32_t kt = thalert_self_kt();
    HANDLE host = kind ? thalert_host_self() : NULL;
    int alerted = 0;
    ThAlert *e;

    thalert_lock();
    e = thalert_find(kt, kind != 0);
    if (e) {
        if (kind && host && e->src != host) {
            HANDLE old = e->host;     /* first wait, or a KTHREAD reused by a new thread */
            HANDLE dup = NULL;
            DuplicateHandle(GetCurrentProcess(), host, GetCurrentProcess(), &dup,
                            0, FALSE, DUPLICATE_SAME_ACCESS);
            e->host = dup;
            e->src = host;
            if (old) CloseHandle(old);
        }
        e->waiting = kind;
        if (kind && e->alerted[kind - 1]) {
            e->alerted[kind - 1] = 0;
            e->waiting = 0;
            alerted = 1;
        }
    }
    thalert_unlock();
    return alerted;
}

/* Wake `kthread` for a user APC queued to it (from another thread). */
static void thalert_wake_for_apc(uint32_t kthread)
{
    ThAlert *e;
    if (!apc_wake_enabled() || !kthread || kthread == thalert_self_kt())
        return;
    thalert_lock();
    e = thalert_find(kthread, 0);
    if (e && e->waiting == 2 /* user-mode alertable */ && e->host)
        QueueUserAPC(thalert_noop, e->host, 0);
    thalert_unlock();
}

/* KeAlertThread: the previous Alerted[mode]; sets it and wakes an
 * alertable wait it may end (a kernel-mode alert ends either kind). */
static int thalert_alert(uint32_t kthread, int mode)
{
    ThAlert *e;
    int prev = 0;
    mode = mode ? 1 : 0;
    thalert_lock();
    e = thalert_find(kthread, 1);
    if (e) {
        prev = e->alerted[mode];
        if (!prev) {
            if (e->waiting && (mode == 0 || e->waiting == 2) && e->host
                    && kthread != thalert_self_kt()) {
                /* Ends the wait instead: the flag of the wait's own mode
                 * stands for the wake, and bridge_wait_done clears it. */
                e->alerted[e->waiting - 1] = 1;
                QueueUserAPC(thalert_noop, e->host, 0);
            } else {
                e->alerted[mode] = 1;
            }
        }
    }
    thalert_unlock();
    return prev;
}

/* KeTestAlertThread: Alerted[mode], cleared. */
static int thalert_test(uint32_t kthread, int mode)
{
    ThAlert *e;
    int was = 0;
    thalert_lock();
    e = thalert_find(kthread, 0);
    if (e) {
        was = e->alerted[mode ? 1 : 0];
        e->alerted[mode ? 1 : 0] = 0;
    }
    thalert_unlock();
    return was;
}

/* The thread is gone: its entry goes, with the host handle. */
static void thalert_forget(uint32_t kthread)
{
    ThAlert *e;
    HANDLE h = NULL;
    thalert_lock();
    e = thalert_find(kthread, 0);
    if (e) {
        h = e->host;
        memset(e, 0, sizeof *e);
    }
    thalert_unlock();
    if (h) CloseHandle(h);
    if (t_thalert_host && kthread == g_thread_obj_va) {   /* the exiting thread's own */
        CloseHandle(t_thalert_host);
        t_thalert_host = NULL;
    }
}

/* bridge_wait_begin: see above. Returns 1 when the wait is answered
 * (g_eax set) and must not be made. */
static int bridge_run_io_apcs(uint32_t alertable, uint32_t wait_mode)
{
    wait_mode &= 0xFF;
    if (!(alertable & 0xFF))
        return 0;
    if (thalert_mark(1 + (wait_mode == 1))) {
        g_eax = 0x00000101u;             /* STATUS_ALERTED */
        return 1;
    }
    SleepEx(0, TRUE);                    /* a stale wake-up from an earlier wait */
    if (wait_mode == 1 && bridge_run_user_apcs()) {
        thalert_mark(0);
        return 1;
    }
    return 0;
}

/* After the host wait of an alertable wait. Woken (STATUS_ALERTED from the
 * host): a user-mode wait runs the APCs that woke it and returns
 * STATUS_USER_APC; an alert's flag is cleared and STATUS_ALERTED stays. */
static void bridge_wait_done(uint32_t alertable, uint32_t wait_mode)
{
    wait_mode &= 0xFF;
    if (!(alertable & 0xFF))
        return;
    thalert_mark(0);
    if (g_eax != 0x00000101u)
        return;
    if (wait_mode == 1 && bridge_run_user_apcs())
        return;
    thalert_test(thalert_self_kt(), wait_mode == 1);
}

/* -- XeLoadSection / XeUnloadSection (ordinals 327/328, 1 arg = 4 bytes) --
 *
 * NTSTATUS XeLoadSection(PXBE_SECTION_HEADER Section);
 *
 * On hardware a section marked non-preload is paged in from disc on demand,
 * and a title that keeps its video decoder in one -- Wreckless keeps WMVDEC
 * there -- calls this before touching it. Every section is already resident
 * here; xbox_xbe_sections.c keeps the reference counts the console keeps and
 * copies a section again from the image where its bytes may have changed
 * (a writable one reloaded after a full unload, one of two sections at the
 * same addresses) -- XBA-124, RECOMP_XBE_SECTIONS=0 for the count only.
 *
 * Done against guest memory rather than the PXBE_SECTION_HEADER struct: the
 * on-disc header is nine 32-bit fields and a digest, and the native struct
 * declares some of them as pointers, so on x64 its layout is not the 56 bytes
 * actually there.
 *
 *   +0x14  section name address      +0x18  section reference count
 */
#define XBE_SECTION_REFCOUNT_OFFSET 0x18

static void bridge_XeSection(int load)
{
    uint32_t section = STACK_ARG(0);

    g_eax = load ? xbox_XbeSectionLoad(section) : xbox_XbeSectionUnload(section);
    if (KERNEL_LOG_ON() && g_eax == 0)
        fprintf(stderr, "  [XBE] Xe%sSection(0x%08X) refcount=%u\n",
                load ? "Load" : "Unload", section, BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET));
}

static void bridge_XeLoadSection(void)   { bridge_XeSection(1); }
static void bridge_XeUnloadSection(void) { bridge_XeSection(0); }

/* -- RtlUnwind (ordinal 312, 4 args = 16 bytes) -------------------------
 *
 * VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
 *                PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
 *
 * Discards the SEH registration frames between the current one and
 * TargetFrame, letting each handler run its __finally blocks on the way past,
 * and leaves fs:[0] pointing at TargetFrame. The walk is xbox_SehUnwind
 * (xbox_seh.c), which checks every record before following it.
 *
 * Left unbridged this returned 0 without touching anything, which is not a
 * harmless stub: MSVC's _global_unwind2 calls it and then carries on as if the
 * frames were gone, so the chain kept pointing into stack that had already
 * been reused and the next dispatch walked records built out of live locals.
 *
 * NT ends an unwind by continuing at TargetIp with eax = ReturnValue.
 * _global_unwind2 passes the address right after its own call, so returning
 * is the same thing; any other TargetIp cannot be reached from here.
 */
static void bridge_RtlUnwind(void)
{
    uint32_t target_frame = STACK_ARG(0);
    uint32_t target_ip    = STACK_ARG(1);
    uint32_t exc_record   = STACK_ARG(2);
    uint32_t ret_value    = STACK_ARG(3);
    uint32_t caller       = g_xbox_kernel_caller;

    xbox_SehUnwind(target_frame, exc_record, caller);

    /* Land on the target even if the walk stopped early: leaving fs:[0] on a
     * discarded frame is worse than losing a __finally. */
    if (target_frame && target_frame != XBOX_SEH_END_OF_CHAIN)
        BRIDGE_MEM32(XBOX_FS_BASE) = target_frame;

    if (target_ip && target_ip != caller) {
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "  [SEH] RtlUnwind: TargetIp 0x%08X is not the "
                    "return address 0x%08X; returning there instead\n",
                    target_ip, caller);
            fflush(stderr);
        }
    }
    g_eax = ret_value;
}


/* Which open file handles were asked for asynchronously.
 *
 * NtCreateFile takes FILE_SYNCHRONOUS_IO_ALERT (0x10) and
 * FILE_SYNCHRONOUS_IO_NONALERT (0x20) in CreateOptions. With neither, the
 * handle is asynchronous: NtReadFile on it returns STATUS_PENDING even with
 * no event, and the caller waits on the handle. Completing every read
 * synchronously answers a different question than the one that was asked.
 *
 * A flat array because a title has a handful of files open at once and a
 * linear scan of sixty-four entries costs less than a hash would.
 */
#define BRIDGE_ASYNC_MAX 64
static uint32_t g_async_handles[BRIDGE_ASYNC_MAX];

static void bridge_note_async_handle(uint32_t token)
{
    int i;
    if (!token)
        return;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token)
            return;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (!g_async_handles[i]) { g_async_handles[i] = token; return; }
}

static void bridge_forget_async_handle(uint32_t token)
{
    int i;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token) { g_async_handles[i] = 0; return; }
}

static int bridge_handle_is_async(uint32_t token)
{
    int i;
    if (!token)
        return 0;
    for (i = 0; i < BRIDGE_ASYNC_MAX; i++)
        if (g_async_handles[i] == token)
            return 1;
    return 0;
}

/* Off unless RECOMP_ASYNC_IO is set, so the two behaviours stay comparable
 * rather than one being swapped in blind. */
static int bridge_async_io_enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_ASYNC_IO") ? 1 : 0;
    return on;
}

/* Read latency, always on: every guest read (NtReadFile, NtReadFileScatter)
 * is timed on the host, and the first read 10 s or more after the window
 * began prints the window:
 *   [READ] window_s=10.0 reads=152 mb=4.9 max_ms=12.3 max_want=65536 max_from=0x0001ABCD
 * max_ms: the longest single read (the host's time in xbox_NtReadFile, or
 * the whole scatter request), with its size and caller. A title that
 * streams music from disc starves its stream voice when one read outlasts
 * what the voice has queued, which the [APU] health line shows as dry
 * time. Nothing when no read comes. A spin flag keeps two threads reading
 * at once from mixing the counters; the cost is two counter reads a read. */
static struct {
    volatile LONG busy;
    long long window_us, max_us;
    unsigned  count;
    uint64_t  bytes;
    uint32_t  max_want, max_from;
} s_read_lat;

static long long bridge_clock_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (!freq.QuadPart && !QueryPerformanceFrequency(&freq))
        return (long long)GetTickCount64() * 1000;
    QueryPerformanceCounter(&now);
    return (long long)(now.QuadPart / freq.QuadPart) * 1000000
           + (long long)(now.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart;
}

static void bridge_read_done(long long t0_us, uint32_t want, uint32_t got)
{
    long long now = bridge_clock_us(), dur = now - t0_us;

    while (InterlockedCompareExchange(&s_read_lat.busy, 1, 0) != 0)
        SwitchToThread();
    if (!s_read_lat.window_us)
        s_read_lat.window_us = t0_us;
    s_read_lat.count++;
    s_read_lat.bytes += got;
    if (dur > s_read_lat.max_us) {
        s_read_lat.max_us = dur;
        s_read_lat.max_want = want;
        s_read_lat.max_from = g_xbox_kernel_caller;
    }
    if (now - s_read_lat.window_us >= 10000000) {
        fprintf(stderr, "  [READ] window_s=%.1f reads=%u mb=%.1f max_ms=%.1f max_want=%u max_from=0x%08X\n",
                (now - s_read_lat.window_us) / 1e6, s_read_lat.count,
                s_read_lat.bytes / 1048576.0, s_read_lat.max_us / 1000.0,
                s_read_lat.max_want, s_read_lat.max_from);
        s_read_lat.window_us = now;
        s_read_lat.count = 0;
        s_read_lat.bytes = 0;
        s_read_lat.max_us = 0;
    }
    InterlockedExchange(&s_read_lat.busy, 0);
}

/* ── NtReadFile (ordinal 219, 8 args = 32 bytes) ──────── */
static void bridge_NtReadFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off = {0};
    PLARGE_INTEGER poff = NULL;
    long long t0_us;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(buffer_va, length, "NtReadFile")) {
        bridge_write_iostatus(iostatus, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    t0_us = bridge_clock_us();
    g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_read_done(t0_us, length, (uint32_t)ios.Information);

    /* What a read actually delivered. A decoder that rejects its input cannot
     * say whether the bytes were wrong or the read was, and the two look
     * identical from inside the title -- the first bytes settle it. */
    {
        static volatile LONG reads_logged;
        const uint8_t *p = (const uint8_t *)XBOX_TO_NATIVE(buffer_va);
        uint32_t got = (uint32_t)ios.Information;
        /* Failures and short reads always print (KRN-5). */
        int failed = ios.Status != 0 || got != length;
        if (bridge_io_log_success(&reads_logged, failed)) {
            /* The offset matters as much as the length. A title streaming a pack
             * file reads sector-aligned chunks, so the first bytes belong to
             * whatever precedes the file it actually wants, and a read that stops
             * early looks identical to one that never started -- until you can
             * see where each one landed. */
            if (poff)
                fprintf(stderr, "  [READ] from=0x%08X ev=%08X apc=%08X buf=0x%08X @%lld want=%u got=%u st=0x%08X  %02X %02X %02X %02X\n",
                        g_xbox_kernel_caller, STACK_ARG(1), STACK_ARG(2), buffer_va,
                        (long long)off.QuadPart, length, got,
                        (uint32_t)ios.Status,
                        got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                        got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
            else
                fprintf(stderr, "  [READ] from=0x%08X buf=0x%08X esp=0x%08X @seq want=%u got=%u st=0x%08X  %02X %02X %02X %02X\n",
                        g_xbox_kernel_caller, buffer_va, g_esp,
                        length, got, (uint32_t)ios.Status,
                        got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                        got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
            fflush(stderr);
        }
    }
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus, g_eax, (uint32_t)ios.Information,
                            bridge_async_io_enabled() && bridge_handle_is_async(STACK_ARG(0)));

    /* An asynchronous request returns STATUS_PENDING, even when the data was
     * already there.
     *
     * A caller that asked to be told later is told later on Windows: the read
     * returns 0x00000103 and the status block carries the result once the
     * handle signals. Returning STATUS_SUCCESS instead is not a harmless
     * shortcut, it is a different contract, and code written against the real
     * one takes the branch that says nothing is in flight.
     *
     * The read itself stays synchronous here: the status block is already
     * written and the event already signalled, so a caller that waits is
     * satisfied immediately. Only the answer changes.
     *
     * An event or APC alone does not make a read asynchronous: on a
     * synchronous handle the kernel waits and returns the final status. */
    if (bridge_async_io_enabled() && bridge_handle_is_async(STACK_ARG(0)))
        g_eax = STATUS_PENDING;
}

/* ── NtWriteFile (ordinal 236, 8 args = 32 bytes) ─────── */
static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off = {0};
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(buffer_va, length, "NtWriteFile")) {
        bridge_write_iostatus(iostatus, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus, g_eax, (uint32_t)ios.Information, 0);
}

/* ── NtQueryInformationFile (ordinal 211, 5 args = 20 bytes) */
static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtQueryInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtSetInformationFile (ordinal 226, 5 args = 20 bytes) ─ */
static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtSetInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }

    /* FileCompletionInformation (30): { HANDLE Port; PVOID Key; }, the
     * port a handle token. XAPI's CreateIoCompletionPort on a file joins it
     * to the port: its requests with an ApcContext and no APC routine post
     * their completion there (bridge_complete_file_io, XBA-114). */
    if (infoclass == 30u) {
        if (length < 8u) {
            g_eax = 0xC0000004u;             /* STATUS_INFO_LENGTH_MISMATCH */
        } else {
            HANDLE port = bridge_resolve_handle(BRIDGE_MEM32(info_va));
            g_eax = ks_port_associate(handle, port, BRIDGE_MEM32(info_va + 4))
                  ? 0xC000000Du : 0;         /* STATUS_INVALID_PARAMETER */
        }
        bridge_write_iostatus(ios_va, (NTSTATUS)g_eax, 0);
        return;
    }

    /* FileRenameInformation (10) carries a pointer, so it is unpacked here,
     * where guest addresses mean something. Guest layout, 16 bytes:
     *     +0  BOOLEAN       ReplaceIfExists
     *     +4  HANDLE        RootDirectory
     *     +8  OBJECT_STRING FileName { u16 Length, u16 Max, char *Buffer }
     * A title that writes a z:\ cache file as .tmp and renames it to its
     * final name never found the final file (0xC0000034) without this. */
    if (infoclass == 10u) {
        XBOX_OBJECT_ATTRIBUTES oa;
        XBOX_ANSI_STRING name;
        static RECOMP_TLS char path[1024];
        uint32_t root, buf;
        uint16_t len;

        if (length < 16u) {
            g_eax = 0xC0000004u;             /* STATUS_INFO_LENGTH_MISMATCH */
            bridge_write_iostatus(ios_va, (NTSTATUS)g_eax, 0);
            return;
        }
        root = BRIDGE_MEM32(info_va + 4);
        len  = BRIDGE_MEM16(info_va + 8);
        buf  = BRIDGE_MEM32(info_va + 12);
        if (!len || len >= sizeof path || !bridge_va_mapped(buf, len)) {
            g_eax = (uint32_t)STATUS_OBJECT_NAME_INVALID;
            bridge_write_iostatus(ios_va, (NTSTATUS)g_eax, 0);
            return;
        }
        memcpy(path, XBOX_TO_NATIVE(buf), len);
        path[len] = '\0';
        name.Buffer = path;
        name.Length = len;
        name.MaximumLength = (USHORT)(len + 1);
        /* -3 is the XDK DOS-device namespace, as in bridge_build_oa. */
        oa.RootDirectory = root && root != 0xFFFFFFFDu
                         ? bridge_resolve_handle(root) : NULL;
        oa.ObjectName = &name;
        oa.Attributes = 0;
        g_eax = (uint32_t)xbox_RenameFileByHandle(handle, &oa,
                    (BOOLEAN)(BRIDGE_MEM8(info_va) != 0));
        if (g_eax)
            fprintf(stderr, "  [FILE] rename to %s -> 0x%08X FAILED\n",
                    path, g_eax);
        bridge_write_iostatus(ios_va, (NTSTATUS)g_eax, 0);
        return;
    }

    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryVolumeInformationFile (ordinal 218, 5 args = 20 bytes) */
static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtQueryVolumeInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryFullAttributesFile (ordinal 210, 2 args = 8 bytes) */
static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));
    /* Asking whether a file exists is not expecting it: a probe, listed only
     * under RECOMP_MISSING_FILES=all. */
    if (g_eax)
        xbox_missing_note(name.Buffer, g_eax, 0, XBOX_FILE_OPEN, 0, 1);
}

/* ── NtFlushBuffersFile (ordinal 198, 2 args = 8 bytes) ─── */
static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtDeleteFile (ordinal 195, 1 arg = 4 bytes) ─────── */
static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

/* ── NtQueryDirectoryFile (ordinal 207, 10 args = 40 bytes) ─ */
static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    uint32_t info_class  = STACK_ARG(7);
    uint32_t filename_va = STACK_ARG(8);  /* PXBOX_ANSI_STRING */
    uint32_t restart     = STACK_ARG(9);  /* BOOLEAN */
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    /* The guest's buffer and pattern are checked like the other file
     * bridges' (KRN-13): a bad info_va or Length faulted inside the
     * runtime instead of returning STATUS_ACCESS_VIOLATION. */
    if (!bridge_buf_ok(info_va, length, "NtQueryDirectoryFile")
            || (filename_va && !bridge_buf_ok(filename_va, 8, "NtQueryDirectoryFile"))) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (filename_va) {
        /* Xbox ANSI_STRING: 0=Length(u16), 2=MaximumLength(u16), 4=Buffer(u32) */
        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        if (fn_buf && !bridge_buf_ok(fn_buf, fn.Length, "NtQueryDirectoryFile")) {
            bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
            g_eax = 0xC0000005u;
            return;
        }
        fn.Buffer        = fn_buf ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length, (XBOX_FILE_INFORMATION_CLASS)info_class,
                pfn, (BOOLEAN)restart);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* Symbolic link objects (KRN-14).
 *
 * NtOpenSymbolicLinkObject succeeded for any name and
 * NtQuerySymbolicLinkObject answered \Device\CdRom0 for any handle, though
 * kernel_io.c's table knows every link the title (or XAPI) made: a title
 * asking where T: or its own Z: points was told the DVD. The open now
 * resolves the name -- a link in the table, or \??\D:, which the kernel
 * itself makes for the DVD -- and fails with STATUS_OBJECT_NAME_NOT_FOUND
 * otherwise, as the console does. The handle names a slot that remembers
 * the target; it stays untagged (0xDEAD0001 for D:, 0xDEAD1000 | slot for
 * the others), so NtClose skips it as before. */
#define SYMLINK_SLOTS 16
static char s_symlink_target[SYMLINK_SLOTS][128];
static volatile LONG s_symlink_next;
static const char s_cdrom_target[] = "\\Device\\CdRom0";

static int symlink_is_dvd(const char *name)
{
    if (name[0] == '\\' && name[1] == '?' && name[2] == '?' && name[3] == '\\')
        name += 4;
    return (name[0] == 'D' || name[0] == 'd') && name[1] == ':' && name[2] == '\0';
}

static const char *symlink_target_of(uint32_t handle)
{
    if ((handle & 0xFFFFF000u) == 0xDEAD1000u && (handle & 0xFFFu) < SYMLINK_SLOTS)
        return s_symlink_target[handle & 0xFFFu];
    return s_cdrom_target;                 /* 0xDEAD0001, and anything older */
}

/* ── NtOpenSymbolicLinkObject (ordinal 203, 2 args = 8 bytes) */
static void bridge_NtOpenSymbolicLinkObject(void)
{
    uint32_t handle_va = STACK_ARG(0);
    const char *name = bridge_get_xbox_path(STACK_ARG(1));
    const char *target = name ? xbox_LookupSymbolicLink(name) : NULL;
    uint32_t handle;

    if (!name) {
        g_eax = 0xC0000033u;               /* STATUS_OBJECT_NAME_INVALID */
        return;
    }
    if (!target && !symlink_is_dvd(name)) {
        static int said;
        if (said++ < 8)
            fprintf(stderr, "  [KERNEL] NtOpenSymbolicLinkObject: no link named %s\n", name);
        if (handle_va) BRIDGE_MEM32(handle_va) = 0;
        g_eax = 0xC0000034u;               /* STATUS_OBJECT_NAME_NOT_FOUND */
        return;
    }
    if (!target) {
        handle = 0xDEAD0001u;
    } else {
        uint32_t slot = (uint32_t)(InterlockedIncrement(&s_symlink_next) - 1) % SYMLINK_SLOTS;
        snprintf(s_symlink_target[slot], sizeof s_symlink_target[slot], "%s", target);
        handle = 0xDEAD1000u | slot;
    }
    if (handle_va) BRIDGE_MEM32(handle_va) = handle;
    g_eax = STATUS_SUCCESS;
}

/* ── NtQuerySymbolicLinkObject (ordinal 215, 3 args = 12 bytes) */
static void bridge_NtQuerySymbolicLinkObject(void)
{
    uint32_t handle = STACK_ARG(0);
    uint32_t target_va = STACK_ARG(1);
    uint32_t retlen_va = STACK_ARG(2);
    const char* target = symlink_target_of(handle);
    USHORT len = (USHORT)strlen(target);

    if (retlen_va) BRIDGE_MEM32(retlen_va) = (uint32_t)len;

    /* Say so when the buffer could not be filled.
     *
     * Reporting STATUS_SUCCESS with an untouched output buffer is the same
     * defect that left ordinal 215 unrouted: the caller believes it has a
     * device path and parses whatever was already in that memory. Half-Life 2
     * does exactly that -- it walked uninitialised bytes and dereferenced
     * 0x68737572, the ASCII "rush", as a pointer. That only looked survivable
     * because the RAM mirrors happened to back the address; with a mapping
     * that does not alias, it faults immediately.
     *
     * STATUS_BUFFER_TOO_SMALL is the honest answer, and it is one the caller
     * already has to handle -- it is what a real kernel returns when the
     * ANSI_STRING it was handed has no room. */
    if (!target_va) {
        g_eax = 0xC0000023u;             /* STATUS_BUFFER_TOO_SMALL */
        return;
    }
    {
        uint16_t max_len = BRIDGE_MEM16(target_va + 2);
        uint32_t buf_va  = BRIDGE_MEM32(target_va + 4);

        if (!buf_va || len >= max_len) {
            static unsigned warned;
            if (warned++ < 4) {
                fprintf(stderr,
                        "  [KERNEL] NtQuerySymbolicLinkObject: buffer 0x%08X "
                        "max=%u cannot hold %u bytes; returning "
                        "STATUS_BUFFER_TOO_SMALL\n",
                        buf_va, (unsigned)max_len, (unsigned)len + 1);
                fflush(stderr);
            }
            g_eax = 0xC0000023u;         /* STATUS_BUFFER_TOO_SMALL */
            return;
        }
        memcpy(XBOX_TO_NATIVE(buf_va), target, len + 1);
        BRIDGE_MEM16(target_va) = len;
    }
    g_eax = STATUS_SUCCESS;
}

/* ── IoCreateFile (ordinal 67, 10 args = 40 bytes) ────── */
static void bridge_IoCreateFile(void)
{
    /* Same as NtCreateFile with an extra Options arg at the end */
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options, 0);
}

/* -- NtDeviceIoControlFile (ordinal 196, 10 args = 40 bytes) ----
 *
 * NTSTATUS NtDeviceIoControlFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
 *                                PIO_STATUS_BLOCK, ULONG IoControlCode,
 *                                PVOID In, ULONG InLen,
 *                                PVOID Out, ULONG OutLen);
 */
#define IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO 0x00074004u

/* The retail hard disk, in the units DISK_GEOMETRY reports. Deliberately the
 * same geometry kernel_path.c writes into the partition table it synthesises,
 * so a title that reads both sees one disk rather than two. */
#define XBOX_DISK_BYTES_PER_SECTOR   512u
#define XBOX_DISK_SECTORS_PER_TRACK  63u
#define XBOX_DISK_TRACKS_PER_CYL     255u
#define XBOX_DISK_CYLINDERS          1216u    /* ~10 GB, a retail 8 GB drive */

/* The Xbox DVD security sector, answered to READ DVD STRUCTURE (SCSI 0xAD)
 * from the running title's own certificate.
 *
 * XAPI start-up reads this sector when bit 0 of the certificate's dword at
 * +0x1D8 is set (only certificates longer than 0x1D8 bytes have that field)
 * and returns to the dashboard (HalReturnToFirmware) unless the sector matches
 * the certificate. The check itself is the only specification there is; it
 * looks at three fields of the DataBuffer (a 4-byte header, then the
 * 1632-byte sector):
 *   - the dword at +0x4AB is the certificate's time stamp (cert + 4);
 *   - the 20 bytes at +0x4CF are SHA-1 of the length 0x4CB, as a
 *     little-endian dword, followed by the 0x4CB bytes at +4;
 *   - the signature at +0x4E3 verifies (XcVerifyPKCS1Signature, which
 *     answers TRUE here).
 * There is no disc to read, so the sector holds nothing else (zeros): only
 * what the title's certificate asks for. Returns 1 when it wrote a sector,
 * 0 when the title does not ask for one (DataBuffer left as it was). */
#define DVD_SECTOR_TIMESTAMP   0x4ABu
#define DVD_SECTOR_HASHED_LEN  0x4CBu
#define DVD_SECTOR_DIGEST      0x4CFu
#define DVD_SECTOR_SIGNATURE   0x4E3u
#define XBE_HEADER_CERT_VA     (0x00010000u + 0x118u)
#define XBE_CERT_SECURITY_FLAGS 0x1D8u

static int bridge_dvd_security_sector(uint32_t data_va, uint32_t data_len)
{
    uint32_t cert = BRIDGE_MEM32(XBE_HEADER_CERT_VA);
    uint32_t cert_size, n = DVD_SECTOR_HASHED_LEN;
    uint8_t len_le[4];
    XBOX_SHA_CONTEXT sha;
    UCHAR digest[20];
    uint32_t i;

    if (!data_va || data_len < DVD_SECTOR_SIGNATURE)
        return 0;
    /* The certificate lives in the image header mapped at 0x10000. */
    if (cert < 0x00010000u || cert >= 0x00020000u)
        return 0;
    cert_size = BRIDGE_MEM32(cert);
    if (cert_size < XBE_CERT_SECURITY_FLAGS + 4 || cert_size > 0x1000u)
        return 0;
    if (!(BRIDGE_MEM32(cert + XBE_CERT_SECURITY_FLAGS) & 1u))
        return 0;

    for (i = 0; i < data_len; i++)
        BRIDGE_MEM8(data_va + i) = 0;
    BRIDGE_MEM32(data_va + DVD_SECTOR_TIMESTAMP) = BRIDGE_MEM32(cert + 4);

    len_le[0] = (uint8_t)n;         len_le[1] = (uint8_t)(n >> 8);
    len_le[2] = (uint8_t)(n >> 16); len_le[3] = (uint8_t)(n >> 24);
    xbox_XcSHAInit(&sha);
    xbox_XcSHAUpdate(&sha, len_le, 4);
    xbox_XcSHAUpdate(&sha, (const UCHAR *)XBOX_TO_NATIVE(data_va + 4), n);
    xbox_XcSHAFinal(&sha, digest);
    for (i = 0; i < 20; i++)
        BRIDGE_MEM8(data_va + DVD_SECTOR_DIGEST + i) = digest[i];
    return 1;
}

static void bridge_NtDeviceIoControlFile(void)
{
    uint32_t handle  = STACK_ARG(0);
    uint32_t ios_va  = STACK_ARG(4);
    uint32_t ioctl   = STACK_ARG(5);
    uint32_t out_va  = STACK_ARG(8);
    uint32_t out_len = STACK_ARG(9);

    /* IOCTLs aimed at the DVD device (see bridge_is_cdrom_device).
     *
     * These are the media check: the title asks the drive to confirm a disc is
     * present and that it is the one it expects. There is no drive here and no
     * disc to describe, so the honest answer is the one that lets the title
     * proceed -- the alternative is STATUS_NOT_SUPPORTED, which it reads as a
     * failed check and answers with HalReturnToFirmware.
     *
     * Reported rather than silent: which codes a title sends is the useful
     * fact when the check still fails, and guessing at them from documentation
     * is how this layer accumulates handlers for IOCTLs nothing ever sends. The
     * output buffer is zeroed, so a title that reads a result field back sees a
     * defined value instead of whatever was on its heap. */
    if (handle == BRIDGE_CDROM_HANDLE) {
        uint32_t in_va  = STACK_ARG(6);
        uint32_t in_len = STACK_ARG(7);

        /* The media check arrives as a SCSI pass-through, so the answer the
         * title reads is not the IOCTL's output buffer -- that is NULL here,
         * with length zero -- but the DataBuffer the request points at.
         * Returning STATUS_SUCCESS alone leaves that buffer as the title
         * zeroed it, which it reads as a failed check; DDS9 retries five
         * times and then exits through HalReturnToFirmware.
         *
         * SCSI_PASS_THROUGH_DIRECT, 32-bit layout, 44 bytes:
         *   0 Length(USHORT)  2 ScsiStatus  3 PathId  4 TargetId  5 Lun
         *   6 CdbLength  7 SenseInfoLength  8 DataIn
         *   12 DataTransferLength  16 TimeOutValue  20 DataBuffer
         *   24 SenseInfoOffset  28 Cdb[16] */
        if (in_va && in_len >= 44 && BRIDGE_MEM8(in_va + 28) == 0x5A) {
            uint32_t data_va  = BRIDGE_MEM32(in_va + 20);
            uint32_t data_len = BRIDGE_MEM32(in_va + 12);
            uint32_t page     = BRIDGE_MEM8(in_va + 30) & 0x3F;

            fprintf(stderr, "  [FILE] DVD MODE SENSE(10) page 0x%02X, "
                            "%u bytes -> authentication page\n", page, data_len);

            if (data_va && data_len) {
                uint32_t i;
                for (i = 0; i < data_len; i++)
                    BRIDGE_MEM8(data_va + i) = 0;

                /* An 8-byte MODE SENSE(10) parameter header, then the page.
                 * The three bytes that matter are named by the title's own
                 * validation at guest 0x0021EA56-0x0021EA6D, which is the only
                 * specification of this page there is: byte 11 must be exactly
                 * 1, and bytes 10 and 12 must both be non-zero. Anything else
                 * is read as "not the expected disc". */
                if (data_len >= 2) {
                    BRIDGE_MEM8(data_va + 0) = 0;
                    BRIDGE_MEM8(data_va + 1) = 26;   /* mode data length */
                }
                if (data_len >= 10) {
                    BRIDGE_MEM8(data_va + 8) = 0x3E; /* page code */
                    BRIDGE_MEM8(data_va + 9) = 18;   /* page length */
                }
                if (data_len >= 13) {
                    BRIDGE_MEM8(data_va + 10) = 1;   /* non-zero */
                    BRIDGE_MEM8(data_va + 11) = 1;   /* exactly 1 */
                    BRIDGE_MEM8(data_va + 12) = 1;   /* non-zero */
                }
            }

            BRIDGE_MEM8(in_va + 2) = 0;              /* ScsiStatus = GOOD */
            bridge_write_iostatus(ios_va, 0, in_len);
            g_eax = 0;
            return;
        }

        /* Any other command sent as IOCTL_SCSI_PASS_THROUGH_DIRECT (READ DVD
         * STRUCTURE, INQUIRY, ...) is answered the same way: the drive reports
         * GOOD and the DataBuffer holds defined bytes, not what the title's
         * heap happened to contain. There is no disc to describe, so the
         * contents are zero -- except the security sector a title whose
         * certificate asks for one gets (bridge_dvd_security_sector). */
        if (ioctl == 0x4D014u && in_va && in_len >= 44) {
            uint32_t data_va  = BRIDGE_MEM32(in_va + 20);
            uint32_t data_len = BRIDGE_MEM32(in_va + 12);
            uint32_t opcode   = BRIDGE_MEM8(in_va + 28);
            uint32_t i;

#if !defined(_WIN32)
            /* With a disc image mounted (kernel_disc.c, XBA-126), sector
             * reads and READ CAPACITY come from its game partition. */
            if (data_va && data_len
                    && xbox_disc_scsi((const uint8_t *)XBOX_TO_NATIVE(in_va + 28),
                                      (uint8_t *)XBOX_TO_NATIVE(data_va), data_len)) {
                fprintf(stderr, "  [FILE] DVD SCSI command 0x%02X, %u bytes "
                                "-> from the disc image\n", opcode, data_len);
            } else
#endif
            if (opcode == 0xAD && bridge_dvd_security_sector(data_va, data_len)) {
                fprintf(stderr, "  [FILE] DVD SCSI command 0xAD, %u bytes "
                                "-> security sector for this title's "
                                "certificate\n", data_len);
            } else {
                fprintf(stderr, "  [FILE] DVD SCSI command 0x%02X, %u bytes "
                                "-> zeroed\n", opcode, data_len);
                if (data_va)
                    for (i = 0; i < data_len; i++)
                        BRIDGE_MEM8(data_va + i) = 0;
            }
            BRIDGE_MEM8(in_va + 2) = 0;              /* ScsiStatus = GOOD */
            bridge_write_iostatus(ios_va, 0, in_len);
            g_eax = 0;
            return;
        }

        fprintf(stderr, "  [FILE] DVD device IOCTL 0x%X (in=%u out=%u) "
                        "-> STATUS_SUCCESS\n", ioctl, in_len, out_len);
        if (out_va && out_len) {
            uint32_t i;
            for (i = 0; i < out_len; i++)
                BRIDGE_MEM8(out_va + i) = 0;
        }
        bridge_write_iostatus(ios_va, 0, out_len);
        g_eax = 0;
        return;
    }

    if (ioctl == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
        /* DISK_GEOMETRY: Cylinders (LARGE_INTEGER), MediaType,
         * TracksPerCylinder, SectorsPerTrack, BytesPerSector -- 24 bytes. */
        if (!out_va || out_len < 24) {
            bridge_write_iostatus(ios_va, 0xC0000023u, 0); /* BUFFER_TOO_SMALL */
            g_eax = 0xC0000023u;
            return;
        }
        BRIDGE_MEM32(out_va +  0) = XBOX_DISK_CYLINDERS;
        BRIDGE_MEM32(out_va +  4) = 0;
        BRIDGE_MEM32(out_va +  8) = 0x0B;      /* FixedMedia */
        BRIDGE_MEM32(out_va + 12) = XBOX_DISK_TRACKS_PER_CYL;
        BRIDGE_MEM32(out_va + 16) = XBOX_DISK_SECTORS_PER_TRACK;
        BRIDGE_MEM32(out_va + 20) = XBOX_DISK_BYTES_PER_SECTOR;
        bridge_write_iostatus(ios_va, 0, 24);
        g_eax = 0;
        return;
    }

    if (ioctl == IOCTL_DISK_GET_PARTITION_INFO) {
        /* PARTITION_INFORMATION: StartingOffset and PartitionLength as
         * LARGE_INTEGERs, then HiddenSectors, PartitionNumber, and three
         * bytes of type/flags -- 32 bytes. The length is the image's own
         * size, which kernel_path.c set from the partition table. */
        HANDLE h = bridge_resolve_handle(STACK_ARG(0));
        LARGE_INTEGER size;

        if (!out_va || out_len < 32) {
            bridge_write_iostatus(ios_va, 0xC0000023u, 0);
            g_eax = 0xC0000023u;
            return;
        }
        size.QuadPart = 0;
        if (h && h != INVALID_HANDLE_VALUE)
            GetFileSizeEx(h, &size);
        BRIDGE_MEM32(out_va +  0) = 0;                       /* StartingOffset */
        BRIDGE_MEM32(out_va +  4) = 0;
        BRIDGE_MEM32(out_va +  8) = (uint32_t)size.LowPart;  /* PartitionLength */
        BRIDGE_MEM32(out_va + 12) = (uint32_t)size.HighPart;
        BRIDGE_MEM32(out_va + 16) = 0;                       /* HiddenSectors  */
        BRIDGE_MEM32(out_va + 20) = 1;                       /* PartitionNumber*/
        BRIDGE_MEM32(out_va + 24) = 0x00010106u;             /* type/boot/recog */
        BRIDGE_MEM32(out_va + 28) = 0;
        bridge_write_iostatus(ios_va, 0, 32);
        g_eax = 0;
        return;
    }

    fprintf(stderr, "  [FILE] NtDeviceIoControlFile(0x%X) - unhandled\n", ioctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu; /* STATUS_NOT_SUPPORTED */
}

/* ── NtFsControlFile (ordinal 200, 10 args = 40 bytes) ──── */
/* The one FSCTL with a contract nxdk documents is FSCTL_DISMOUNT_VOLUME
 * (CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 8, METHOD_BUFFERED, FILE_ANY_ACCESS) =
 * 0x00090020): XAPI dismounts a volume before it formats it or lets a memory
 * unit go. The host files behind a volume have no cache to flush or mount
 * to drop, as for IoDismountVolume (no-op by design), so it succeeds and the
 * event, if any, is set (XBA-138). Any other code stays
 * STATUS_NOT_SUPPORTED, logged with its code. */
#define FSCTL_DISMOUNT_VOLUME_ 0x00090020u
static void bridge_NtFsControlFile(void)
{
    uint32_t fsctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);

    if (fsctl == FSCTL_DISMOUNT_VOLUME_) {
        fprintf(stderr, "  [FILE] NtFsControlFile(FSCTL_DISMOUNT_VOLUME) - nothing to flush\n");
        bridge_write_iostatus(ios_va, 0, 0);
        bridge_complete_file_io(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
                                STACK_ARG(3), ios_va, 0, 0, 0);
        g_eax = 0;
        return;
    }
    fprintf(stderr, "  [FILE] NtFsControlFile(0x%X) - not supported\n", fsctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu;
}

/* ── NtCreateDirectoryObject (ordinal 188) ──────────────── */
static void bridge_NtCreateDirectoryObject(void)
{
    /* Return STATUS_SUCCESS with a fake handle */
    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* IoCreateSymbolicLink (ordinal 67, 2 args)
 *
 * Was a bare "return STATUS_SUCCESS": the title was told its link existed and
 * nothing recorded it. Titles mount their own drive letters this way --
 * Wreckless links \??\Z: to \Device\Harddisk0\Partition1\ and then loads
 * every asset through z:\ -- so dropping the link left xbox_translate_path
 * applying the generic "Z: is the cache partition" rule, and every asset open
 * failed with ERROR_FILE_NOT_FOUND a whole boot later.
 *
 * Both arguments are guest ANSI_STRINGs whose Buffer field holds a guest VA,
 * so passing the structs straight through would have xbox_copy_ansi read a
 * 32-bit guest address as a 64-bit host pointer. Rebuild them by hand, the way
 * bridge_RtlEqualString does.
 */
static void bridge_IoCreateSymbolicLink(void)
{
    uint32_t link_va   = STACK_ARG(0);
    uint32_t target_va = STACK_ARG(1);
    XBOX_ANSI_STRING link, target;

    if (!link_va) {
        g_eax = 0xC000000Du;  /* STATUS_INVALID_PARAMETER */
        return;
    }
    link.Length        = BRIDGE_MEM16(link_va + 0);
    link.MaximumLength = BRIDGE_MEM16(link_va + 2);
    link.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(link_va + 4));

    if (target_va) {
        target.Length        = BRIDGE_MEM16(target_va + 0);
        target.MaximumLength = BRIDGE_MEM16(target_va + 2);
        target.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(target_va + 4));
    } else {
        target.Length = target.MaximumLength = 0;
        target.Buffer = NULL;
    }

    g_eax = (uint32_t)xbox_IoCreateSymbolicLink(&link,
                                                target_va ? &target : NULL);
}

static uint32_t ob_from_token(uint32_t token);
static int ob_type_ok(uint32_t body, uint32_t type);

/* ── ObReferenceObjectByHandle (ordinal 246) ─────────────── */
static void bridge_ObReferenceObjectByHandle(void)
{
    /* Xbox: NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)
     * 3 args (not 6 like Windows NT) */
    uint32_t handle = STACK_ARG(0);
    uint32_t obj_type = STACK_ARG(1);
    uint32_t object_ptr = STACK_ARG(2);
    /* Threads have a guest object (see thobj_alloc); -2 is NtCurrentThread. */
    uint32_t thread = handle == 0xFFFFFFFEu ? thobj_current()
                                            : thobj_from_token(handle);

    /* An object of ObCreateObject's (XBA-114): its body, one more
     * reference, and the type checked as the console does. */
    {
        uint32_t body = ob_from_token(handle);
        if (body) {
            if (!ob_type_ok(body, obj_type)) {
                if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
                g_eax = 0xC0000024u;         /* STATUS_OBJECT_TYPE_MISMATCH */
                return;
            }
            InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(body - 0x10u));
            if (object_ptr) BRIDGE_MEM32(object_ptr) = body;
            g_eax = 0;
            return;
        }
    }
    if (thread) {
        thobj_ref(thread);
        if (object_ptr) BRIDGE_MEM32(object_ptr) = thread;
        g_eax = 0;
        return;
    }
    /* Other objects still have no guest body. */
    if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── RtlRaiseException (ordinal 302) ─────────────────────
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Dispatches through the guest's SEH chain (xbox_seh.c): the lifted handlers
 * run, a handler may continue execution (the registers it left in the
 * context come back), or resume at an __except block or a catch continuation
 * (which does not return here). Unhandled, the raise is logged and returns,
 * as it did before dispatch existed. RECOMP_SEH_DISPATCH=0 skips the dispatch.
 */
static void bridge_raise_record(const char *name, uint32_t record_ptr)
{
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;
    int handled = 0;

    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] %s: record=0x%08X code=0x%08X (#%d)\n",
                name, record_ptr, code, raise_count);
        fflush(stderr);
    }

    if (record_ptr && xbox_SehDispatchEnabled())
        handled = xbox_SehRaiseRecord(record_ptr, g_xbox_kernel_caller,
                                      g_esp + 4);
    if (handled)
        return;

    /* Unhandled float exceptions: clear the FPU status.
     *
     * The CRT raises STATUS_FLOAT_* (0xC0000090-0xC0000096) after an x87/SSE
     * fault and re-checks the FPU status when the raise returns. A handler
     * that continues execution clears it through the context; with none, the
     * caller would see the exception still pending and re-raise for ever.
     */
    if (code >= 0xC0000090u && code <= 0xC0000096u) {
        _clearfp();
    }

    g_eax = 0;
}

static void bridge_RtlRaiseException(void)
{
    bridge_raise_record("RtlRaiseException", STACK_ARG(0));
}

/* ── MmMapIoSpace (ordinal 177) ──────────────────────────
 * PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
 *
 * Maps physical I/O memory (GPU registers, etc.) into virtual address space.
 * Allocate from Xbox heap so the returned pointer is a valid Xbox VA.
 */
static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* ── MmPersistContiguousMemory (ordinal 178) ─────────────
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* ── Generic fallback for simple value-only functions ────── */
static void bridge_generic_stub(void)
{
    /* Success-returning stub for functions whose callers only check for 0.
     * Deliberately silent: the caller (kernel_thunk_dispatch) warns for
     * ordinals with no bridge at all, which is the case worth hearing about. */
    g_eax = 0;
}


/* ══════════════════════════════════════════════════════════════════════════
 * Wrappers for the ordinals Halo 2276's thunk table binds but the bridge did
 * not route. Every one of these already had a working xbox_* implementation in
 * src/kernel/*.c; only the wrapper that moves arguments off the simulated stack
 * was missing, so each call was silently a no-op returning 0.
 *
 * Guest pointers go through XBOX_TO_NATIVE, which maps NULL to NULL. Scalars
 * pass straight through. Handles are tokens, not host HANDLEs, so they go
 * through bridge_resolve_handle / bridge_write_handle.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── AvGetSavedDataAddress (ordinal 1, void) */
static void bridge_AvGetSavedDataAddress(void)
{
    g_eax = (uint32_t)xbox_AvGetSavedDataAddress();
}

/* ── AvSendTVEncoderOption (ordinal 2, 4 args) */
static void bridge_AvSendTVEncoderOption(void)
{
    xbox_AvSendTVEncoderOption(XBOX_TO_NATIVE(STACK_ARG(0)),
                               STACK_ARG(1), STACK_ARG(2),
                               (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

/* ── ExFreePool (ordinal 17, 1 arg)
 * Was resolving to a DATA address before the kernel_data_va_for_ordinal fix,
 * so the title was calling into kernel data. Even after that it was an
 * unbridged no-op, which leaks every pool block the title ever frees.
 *
 * Deliberately does NOT call xbox_ExFreePool: that one HeapFrees its argument
 * on the host heap, but every pool block reaches the title as a guest VA from
 * xbox_HeapAlloc, so only the guest heap can release it. xbox_HeapFree answers
 * through the block table and takes the raw 32-bit guest VA. */
static void bridge_ExFreePool(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── IoCreateDevice (ordinal 65, 6 args) ────────────────
 *
 * NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject,
 *                         ULONG DeviceExtensionSize,
 *                         PANSI_STRING DeviceName,
 *                         DEVICE_TYPE DeviceType,
 *                         BOOLEAN Exclusive,
 *                         PDEVICE_OBJECT *DeviceObject);
 *
 * Deliberately does NOT call xbox_IoCreateDevice. That one allocates from the
 * host heap and writes a 64-bit native pointer through this 4-byte guest
 * out-parameter -- the memory-model mismatch the NOT ROUTED note below
 * describes. The object has to live in guest memory because the caller
 * immediately walks it, so allocate it there.
 *
 * Leaving the ordinal unbridged was not the safe option it looked like: the
 * unresolved thunk returns 0, which is STATUS_SUCCESS, so the title proceeds
 * with a NULL device. Wreckless reads DeviceExtension from it and `rep stosd`s
 * DeviceExtensionSize bytes through the NULL, erasing the fake TIB at guest
 * VA 0. The crash surfaced in SetLastError, thousands of calls later.
 *
 * Layout is the Xbox DEVICE_OBJECT: Flags at 0x14, DeviceExtension at 0x18,
 * DeviceType at 0x1C, StackSize at 0x1E (nxdk xboxkrnl.h; written to 0x1D,
 * StartIoFlags, until XBA-138), header 0x38 bytes. Only the fields a
 * title actually reads are filled; the rest is zero, which is what a freshly
 * created device object holds anyway.
 */
#define XBOX_DEVICE_OBJECT_SIZE 0x38u

static void bridge_IoCreateDevice(void)
{
    uint32_t extension_size = STACK_ARG(1);
    uint32_t device_type    = STACK_ARG(3);
    uint32_t out_va         = STACK_ARG(5);
    uint32_t object_va;

    if (!out_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }

    object_va = xbox_HeapAlloc(XBOX_DEVICE_OBJECT_SIZE + extension_size, 16);
    if (!object_va) {
        g_eax = 0xC000009Au;   /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }

    memset(XBOX_TO_NATIVE(object_va), 0,
           XBOX_DEVICE_OBJECT_SIZE + extension_size);
    BRIDGE_MEM16(object_va + 0x02) = (uint16_t)XBOX_DEVICE_OBJECT_SIZE;
    BRIDGE_MEM32(object_va + 0x04) = 1;                 /* ReferenceCount   */
    BRIDGE_MEM32(object_va + 0x08) = STACK_ARG(0);      /* DriverObject     */
    BRIDGE_MEM32(object_va + 0x18) =                    /* DeviceExtension  */
        extension_size ? object_va + XBOX_DEVICE_OBJECT_SIZE : 0;
    BRIDGE_MEM8(object_va + 0x1C)  = (uint8_t)device_type;
    BRIDGE_MEM8(object_va + 0x1E)  = 1;                 /* StackSize        */

    BRIDGE_MEM32(out_va) = object_va;
    g_eax = 0;                                          /* STATUS_SUCCESS   */
}

/* ── KeCancelTimer (ordinal 97, 1 arg)
 * BOOLEAN KeCancelTimer(PKTIMER Timer) -- returns whether it was set. */
static void bridge_KeCancelTimer(void)
{
/* Both halves: the shadow object this runtime keeps, and the firing
     * table above, or a cancelled timer keeps calling its DPC. */
    uint32_t timer_va = STACK_ARG(0);
    int armed = xbox_kernel_cancel_timer(timer_va);
    g_eax = (uint32_t)xbox_KeCancelTimer(XBOX_TO_NATIVE(timer_va)) || armed;
}

/* ── KeDisconnectInterrupt (ordinal 100, 1 arg) */
static void bridge_KeDisconnectInterrupt(void)
{
    g_eax = (uint32_t)xbox_KeDisconnectInterrupt(
        (PXBOX_KINTERRUPT)XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* ── KeSetBasePriorityThread (ordinal 143, 2 args) */
/* KeQueryBasePriorityThread (ordinal 124, 1 arg). The implementation has
 * existed in kernel_thread.c all along; only the bridge wrapper was
 * missing, so the thunk fell through to the fallback and returned 0. */
/* The host thread behind a guest thread object: the KE shadow holds one for
 * every spawned worker, and the calling thread's own object is itself. The
 * object VA used to be passed on as if it were a host handle, so
 * Get/SetThreadPriority failed on every call and no title's priorities ever
 * took effect -- Kung Fu Chaos raises its loader thread, and without that the
 * order between it and the main thread on the way out of GET READY was free
 * to differ from the console's. */
static HANDLE bridge_thread_host(uint32_t obj)
{
    HANDLE h = ke_shadow_lookup(obj);
    if (!h && obj && obj == g_thread_obj_va)
        h = GetCurrentThread();
    return h;
}

static void bridge_KeQueryBasePriorityThread(void)
{
    HANDLE h = bridge_thread_host(STACK_ARG(0));
    g_eax = h ? (uint32_t)xbox_KeQueryBasePriorityThread(h) : 0;
}

static void bridge_KeSetBasePriorityThread(void)
{
    HANDLE h = bridge_thread_host(STACK_ARG(0));
    g_eax = h ? (uint32_t)xbox_KeSetBasePriorityThread(h, (LONG)STACK_ARG(1)) : 0;
}

/* ── KeStallExecutionProcessor (ordinal 151, 1 arg) */
static void bridge_KeStallExecutionProcessor(void)
{
    xbox_KeStallExecutionProcessor(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmLockUnlockBufferPages (ordinal 175, 3 args) */
static void bridge_MmLockUnlockBufferPages(void)
{
    xbox_MmLockUnlockBufferPages(XBOX_TO_NATIVE(STACK_ARG(0)),
                                 STACK_ARG(1), (BOOLEAN)STACK_ARG(2));
    g_eax = 0;
}

/* ── MmQueryAllocationSize (ordinal 180, 1 arg)
 *
 * Answered from the guest heap's block table, NOT from xbox_MmQueryAllocationSize.
 * That one calls VirtualQuery, which on a translated guest address reports the
 * size of the whole 64 MB guest mapping -- a confidently wrong answer where the
 * title expects the size of the block it allocated. This is the memory-model
 * check the parked-bridge list below asks for, done: the question is about
 * guest memory, so only the guest allocator can answer it.
 */
static void bridge_MmQueryAllocationSize(void)
{
    uint32_t va = STACK_ARG(0);
    uint32_t n = xbox_ContiguousBlockSize(va);   /* XPhysicalSize asks this too */
    g_eax = n ? n : xbox_HeapBlockSize(va);
}

/* ── NtCreateMutant (ordinal 192, 3 args) */
static void bridge_NtCreateMutant(void)
{
    uint32_t handle_va = STACK_ARG(0);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING name;
    HANDLE h = NULL;
    NTSTATUS st;

    bridge_build_oa(STACK_ARG(1), &oa, &name);
    st = xbox_NtCreateMutant(&h, STACK_ARG(1) ? &oa : NULL,
                             (BOOLEAN)STACK_ARG(2));
    if (st >= 0 && handle_va) bridge_write_handle(handle_va, h);
    g_eax = (uint32_t)st;
}

/* ── NtReleaseMutant (ordinal 221, 2 args)
 *
 * NTSTATUS NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
 *
 * The partner of NtCreateMutant above, and routing one without the other is a
 * deadlock generator: the create succeeds, the release silently does nothing,
 * and the mutex stays held forever by a thread that has already exited.
 *
 * That is what the Xbox Dashboard's audio streaming did. Each ambient WAV gets
 * five events, a worker thread and a mutant; the worker finished, failed to
 * release, and terminated. The next attempt could not take the mutex, so the
 * dashboard reopened the same file and spawned another worker with another
 * 512 KB stack, forever -- visible only as a heap that climbed and a tick that
 * never returned.
 *
 * Memory model: a handle token in, an optional 4-byte LONG out through a guest
 * address. Nothing allocates, frees, or hands back a host pointer.
 */
static void bridge_NtReleaseMutant(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtReleaseMutant(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* -- NtSuspendThread (ordinal 231, 2 args) --------------------------------
 *
 * NTSTATUS NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
 *
 * The implementation was already here and only the dispatch entry was missing,
 * which is worse than an outright stub: the call returned 0, so a thread that
 * parked itself believed it had stopped and carried straight on. Wreckless
 * does that on a worker, and the "suspended" thread spun through 289 million
 * kernel calls while the title thought it was idle.
 */
static void bridge_NtSuspendThread(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSuspendThread(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PULONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* ── NtResumeThread (ordinal 224, 2 args) */
static void bridge_NtResumeThread(void)
{
    g_eax = (uint32_t)xbox_NtResumeThread(
        bridge_resolve_handle(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── ObfDereferenceObject (ordinal 250, fastcall: object in ecx)
 * Not STACK_ARG(0). Xbox uses __fastcall here, so the argument never reaches
 * the stack and the arg-size entry is 0. Reading it off the stack would
 * dereference whatever the caller happened to leave there. */
static int  ob_is(uint32_t body);
static void ob_deref(uint32_t body);

static void bridge_ObfDereferenceObject(void)
{
    if (ob_is(g_ecx)) {   /* an object of ObCreateObject's (XBA-114) */
        ob_deref(g_ecx);
        g_eax = 0;
        return;
    }
    thobj_unref(g_ecx);   /* no-op unless ecx is a thread object */
    xbox_ObfDereferenceObject(XBOX_TO_NATIVE(g_ecx));
    g_eax = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * More wrappers for xbox_* implementations that had no route. Same rule as
 * the block above: each one was checked against the memory-model bar --
 * reads/writes only happen at caller-supplied guest addresses, XBOX_TO_NATIVE
 * maps guest NULL to host NULL, and none of them allocates, frees, or hands
 * back a host pointer. Any exception is noted in place.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── ObfReferenceObject (ordinal 251, fastcall: object in ecx)
 * Mirror of 250. Xbox uses __fastcall here too, so the argument never reaches
 * the stack and the arg-size entry is 0; reading it off the stack would
 * dereference whatever the caller happened to leave there. */
static void bridge_ObfReferenceObject(void)
{
    if (ob_is(g_ecx))     /* PointerCount, in its OBJECT_HEADER (XBA-114) */
        InterlockedIncrement((volatile LONG *)XBOX_TO_NATIVE(g_ecx - 0x10u));
    thobj_ref(g_ecx);     /* no-op unless ecx is a thread object */
    xbox_ObfReferenceObject(XBOX_TO_NATIVE(g_ecx));
    g_eax = 0;
}

/* ── HalReadSMBusValue / HalWriteSMBusValue (ordinals 45 / 50, 4 args)
 *
 * The SMC side of the AV-pack / temperature queries titles make at init.
 * Unrouted they read 0 -- "no AV pack connected" -- which made D3D pick the
 * lowest common denominator display mode and 40C sensors read as 0C.
 *
 * HalReadSMBusValue writes one ULONG through a caller-supplied DataValue and
 * nothing else; HalWriteSMBusValue is all scalars. Both are the safe kind the
 * note below names.
 */
static void bridge_HalReadSMBusValue(void)
{
    uint32_t data_va = STACK_ARG(3);

    g_eax = (uint32_t)xbox_HalReadSMBusValue(
        (UCHAR)STACK_ARG(0), (UCHAR)STACK_ARG(1),
        (BOOLEAN)STACK_ARG(2), (PULONG)XBOX_TO_NATIVE(data_va));
}

static void bridge_HalWriteSMBusValue(void)
{
    g_eax = (uint32_t)xbox_HalWriteSMBusValue(
        (UCHAR)STACK_ARG(0), (UCHAR)STACK_ARG(1),
        (BOOLEAN)STACK_ARG(2), STACK_ARG(3));
}

/* ── HalReadWritePCISpace (ordinal 46, 6 args)
 *
 * PCI config space for the NV2A/southbridge; on reads the kernel serves a
 * zeroed buffer, so only a memset at a caller-supplied address happens. */
static void bridge_HalReadWritePCISpace(void)
{
    xbox_HalReadWritePCISpace(
        STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
        XBOX_TO_NATIVE(STACK_ARG(3)), STACK_ARG(4), (BOOLEAN)STACK_ARG(5));
    g_eax = 0;
}

/* ── HalRequestSoftwareInterrupt / HalClearSoftwareInterrupt (48 / 38, 1 arg)
 * DPC/APC delivery hooks with no interrupt-driven machinery behind them here;
 * both xbox_* versions are documented no-ops. */
static void bridge_HalRequestSoftwareInterrupt(void)
{
    xbox_HalRequestSoftwareInterrupt((KIRQL)STACK_ARG(0));
    g_eax = 0;
}

static void bridge_HalClearSoftwareInterrupt(void)
{
    xbox_HalClearSoftwareInterrupt((KIRQL)STACK_ARG(0));
    g_eax = 0;
}

/* ── ExSaveNonVolatileSetting (ordinal 29, 4 args)
 *
 * The write half of the EEPROM pair whose read side is already routed.
 * xbox_ExSaveNonVolatileSetting logs and returns success without ever reading
 * the Value buffer, so marshalling it across is cosmetic but keeps the write
 * path out of the unknowable-stub category. */
static void bridge_ExSaveNonVolatileSetting(void)
{
    g_eax = (uint32_t)xbox_ExSaveNonVolatileSetting(
        STACK_ARG(0), STACK_ARG(1), XBOX_TO_NATIVE(STACK_ARG(2)),
        STACK_ARG(3));
}

/* ── MmUnmapIoSpace (ordinal 183, 2 args)
 *
 * Deliberately does NOT call xbox_MmUnmapIoSpace: that one VirtualFrees its
 * argument, but the mapping coming back out of bridge_MmMapIoSpace was carved
 * from the guest heap, so only the guest heap can release it. xbox_HeapFree
 * matches against the block table and takes the raw 32-bit guest VA. */
static void bridge_MmUnmapIoSpace(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── IoDeleteDevice (ordinal 68, 1 arg)
 *
 * Same memory model as MmUnmapIoSpace: bridge_IoCreateDevice allocates the
 * device object on the guest heap, so deletion has to answer there too --
 * HeapFree (what xbox_IoDeleteDevice calls) would free a pointer that heap
 * never saw. */
static void bridge_IoDeleteDevice(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── KeQueryInterruptTime (ordinal 125, void)
 * Returns a 64-bit tick count; the caller reads it as edx:eax, so the high
 * half goes to g_edx exactly like the performance-counter bridges. */
static void bridge_KeQueryInterruptTime(void)
{
    uint64_t t = xbox_KeQueryInterruptTime();

    g_eax = (uint32_t)t;
    g_edx = (uint32_t)(t >> 32);
}

/* ── KeSaveFloatingPointState / KeRestoreFloatingPointState (142 / 139, 1 arg)
 *
 * D3D brackets its fixed-function transform code with these on hardware. Both
 * xbox_* implementations are successful no-ops -- Windows user mode preserves
 * FP state across context switches -- so marshalling is a pass-through of one
 * address. */
static void bridge_KeSaveFloatingPointState(void)
{
    g_eax = (uint32_t)xbox_KeSaveFloatingPointState(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_KeRestoreFloatingPointState(void)
{
    g_eax = (uint32_t)xbox_KeRestoreFloatingPointState(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* ── NtCreateSemaphore (ordinal 193, 4 args)
 *
 * Handle-shaped twin of NtCreateEvent/NtCreateMutant. The native HANDLE is
 * 8 bytes and the guest slot is 4, so the create goes through a local and the
 * result lands via bridge_write_handle. */
static void bridge_NtCreateSemaphore(void)
{
    uint32_t handle_va = STACK_ARG(0);
    HANDLE h = NULL;
    NTSTATUS st;

    st = xbox_NtCreateSemaphore(
        &h, XBOX_TO_NATIVE(STACK_ARG(1)),
        (LONG)STACK_ARG(2), (LONG)STACK_ARG(3));
    if (st >= 0 && handle_va)
        bridge_write_handle(handle_va, h);
    g_eax = (uint32_t)st;
}

/* ── NtReleaseSemaphore (ordinal 222, 3 args)
 * Handle token in, LONG by value, optional 4-byte PLONG out. */
static void bridge_NtReleaseSemaphore(void)
{
    uint32_t count_va = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtReleaseSemaphore(
        bridge_resolve_handle(STACK_ARG(0)), (LONG)STACK_ARG(1),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* ── KeAlertThread (ordinal 93, 2 args)
 * Thread passed as an object; resolves through the handle table the way the
 * Nt* thread calls do, degrading to a pass-through for synthetic handles. */
static void bridge_KeAlertThread(void)
{
    /* BOOLEAN KeAlertThread(PKTHREAD Thread, KPROCESSOR_MODE AlertMode): the
     * thread's previous Alerted[AlertMode]. Thread is the KTHREAD, not a
     * handle; it used to go to xbox_KeAlertThread as one, which queued a
     * host APC on whatever that resolved to (XBA-133). */
    g_eax = (uint32_t)thalert_alert(STACK_ARG(0), (int)(STACK_ARG(1) & 0xFF));
}

/* ── KeWaitForMultipleObjects (ordinal 158, 8 args)
 *
 * KeWaitForSingleObject is routed; the multiple-object sibling was not. Like
 * NtWaitForMultipleObjectsEx, the Objects[] array in guest memory holds 32-bit
 * handle tokens or guest dispatcher-object addresses, so each is resolved
 * before the native wait sees it. */
#define BRIDGE_MAXIMUM_WAIT_OBJECTS 64

static void bridge_KeWaitForMultipleObjects(void)
{
    uint32_t count      = STACK_ARG(0);
    uint32_t objects_va = STACK_ARG(1);
    uint32_t wait_type  = STACK_ARG(2);
    uint32_t alertable  = STACK_ARG(5);   /* 3=WaitReason, 4=WaitMode */
    uint32_t timeout_va = STACK_ARG(6);
    HANDLE handles[BRIDGE_MAXIMUM_WAIT_OBJECTS];
    uint32_t guest_events[BRIDGE_MAXIMUM_WAIT_OBJECTS] = {0};
    int event_types[BRIDGE_MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (count == 0) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }
    if (bridge_run_io_apcs(alertable, STACK_ARG(4)))
        return;
    if (count > BRIDGE_MAXIMUM_WAIT_OBJECTS)
        count = BRIDGE_MAXIMUM_WAIT_OBJECTS;
    for (i = 0; i < count; i++) {
        uint32_t object = objects_va ? BRIDGE_MEM32(objects_va + i * 4) : 0;
        handles[i] = ke_guest_event_prepare(object, &event_types[i]);
        if (handles[i]) {
            guest_events[i] = object;
        } else {
            handles[i] = ke_shadow_lookup(object);
            if (!handles[i])
                handles[i] = bridge_resolve_handle(object);
        }
    }

    {   /* Filed under the first object (kernel_wait_stats.h). */
        uint32_t site = g_xbox_kernel_caller;
        uint64_t t0 = xbox_wait_stats_begin(bridge_timeout_is_poll(timeout_va));
        g_eax = (uint32_t)xbox_KeWaitForMultipleObjects(
            count, (PVOID *)handles, wait_type,
            STACK_ARG(3), (KPROCESSOR_MODE)STACK_ARG(4),
            (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_va),
            XBOX_TO_NATIVE(STACK_ARG(7)));
        /* A timed wait served before it times out (bridge_ke_wait_one). */
        if (g_eax == (uint32_t)STATUS_TIMEOUT
            && wait_timeout_is_timed(XBOX_TO_NATIVE(timeout_va))
            && xbox_IrqServiceTimedWait()) {
            LARGE_INTEGER zero;
            zero.QuadPart = 0;
            for (i = 0; i < count; i++)
                if (guest_events[i])
                    ke_guest_event_prepare(guest_events[i], &event_types[i]);
            g_eax = (uint32_t)xbox_KeWaitForMultipleObjects(
                count, (PVOID *)handles, wait_type,
                STACK_ARG(3), (KPROCESSOR_MODE)STACK_ARG(4),
                (BOOLEAN)alertable, &zero, XBOX_TO_NATIVE(STACK_ARG(7)));
            if (g_eax < count)
                wait_served_note(objects_va ? BRIDGE_MEM32(objects_va) : 0, site);
        }
        bridge_wait_done(alertable, STACK_ARG(4));
        xbox_wait_stats_wait_multi(objects_va ? BRIDGE_MEM32(objects_va) : 0, site, g_eax, t0,
                                   count, wait_type == 0);
    }

    /* Only a successful wait consumes synchronization events. WaitAny's
     * result selects one member; WaitAll consumes every signalled member. */
    for (i = 0; i < count; i++) {
        if (guest_events[i] && event_types[i] == 1 &&
            ((wait_type == 0 && g_eax == 0) ||
             (wait_type != 0 && g_eax < count && g_eax == i)))
            BRIDGE_MEM32(guest_events[i] + 4) = 0;
    }
}

#undef BRIDGE_MAXIMUM_WAIT_OBJECTS

/* ── NtSetSystemTime (ordinal 228, 2 args)
 * Accepts the new time, ignores it, reports the old one through the optional
 * PreviousTime out-parameter (8-byte FILETIME at a guest address). */
static void bridge_NtSetSystemTime(void)
{
    uint32_t prev_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSetSystemTime(
        XBOX_TO_NATIVE(STACK_ARG(0)),
        prev_va ? (PLARGE_INTEGER)XBOX_TO_NATIVE(prev_va) : NULL);
}

/* ── ObReferenceObjectByName (ordinal 247, 5 args)
 *
 * The ANSI_STRING is rebuilt by hand (native struct has a 64-bit Buffer at
 * offset 8, the guest one a 32-bit Buffer at offset 4) and the Object
 * out-parameter is filled through a local so the 8-byte NULL store lands in a
 * local instead of spilling past a 4-byte guest slot. */
static void bridge_ObReferenceObjectByName(void)
{
    uint32_t name_va   = STACK_ARG(0);
    uint32_t object_va = STACK_ARG(4);
    XBOX_ANSI_STRING name;
    PVOID object_local = NULL;

    if (!name_va) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_ObReferenceObjectByName(
        &name, STACK_ARG(1), XBOX_TO_NATIVE(STACK_ARG(2)),
        XBOX_TO_NATIVE(STACK_ARG(3)), &object_local);
    if (object_va)
        BRIDGE_MEM32(object_va) = (uint32_t)(uintptr_t)object_local;
}

/* ── RtlInitUnicodeString (ordinal 290, 2 args)
 *
 * Mirror of the RtlInitAnsiString bridge: the guest UNICODE_STRING is
 * { USHORT Length; USHORT MaximumLength; 32-bit Buffer; } and the Buffer field
 * must carry the guest VA of the source, so the fields are written by hand
 * rather than letting the native xbox_* write a 64-bit host pointer into a
 * 4-byte guest slot. Lengths are in bytes (wide chars x2). */
static void bridge_RtlInitUnicodeString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const uint16_t *wp = (const uint16_t *)XBOX_TO_NATIVE(src_va);
        size_t bytes = 0;

        while (wp[bytes / 2])
            bytes += sizeof(uint16_t);
        if (bytes > 0xFFFE)
            bytes = 0xFFFE;
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)bytes;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(bytes + sizeof(uint16_t));
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── RtlTimeFieldsToTime (ordinal 304, 2 args)
 * Counterpart of the routed RtlTimeToTimeFields: reads a XBOX_TIME_FIELDS and
 * writes a LARGE_INTEGER, both at caller-supplied guest addresses. */
static void bridge_RtlTimeFieldsToTime(void)
{
    g_eax = (uint32_t)xbox_RtlTimeFieldsToTime(
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── PhyGetLinkState (ordinal 252, 1 arg) */
static void bridge_PhyGetLinkState(void)
{
    g_eax = (uint32_t)xbox_PhyGetLinkState((BOOLEAN)STACK_ARG(0));
}

/* ── PhyInitialize (ordinal 253, 2 args) */
static void bridge_PhyInitialize(void)
{
    g_eax = (uint32_t)xbox_PhyInitialize((BOOLEAN)STACK_ARG(0),
                                         XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── RtlTimeToTimeFields (ordinal 305, 2 args) */
static void bridge_RtlTimeToTimeFields(void)
{
    xbox_RtlTimeToTimeFields(
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcSHAInit / XcSHAUpdate / XcSHAFinal (ordinals 335-337) */
static void bridge_XcSHAInit(void)
{
    xbox_XcSHAInit((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_XcSHAUpdate(void)
{
    xbox_XcSHAUpdate((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                     (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)),
                     STACK_ARG(2));
    g_eax = 0;
}

static void bridge_XcSHAFinal(void)
{
    xbox_XcSHAFinal((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcRC4Key / XcRC4Crypt (ordinals 338-339) */
static void bridge_XcRC4Key(void)
{
    xbox_XcRC4Key((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                  STACK_ARG(1),
                  (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcRC4Crypt(void)
{
    xbox_XcRC4Crypt((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    STACK_ARG(1),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── XcHMAC (ordinal 340, 7 args) */
static void bridge_XcHMAC(void)
{
    xbox_XcHMAC((const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
                (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

/* ── XcDESKeyParity (ordinal 346, 2 args) */
static void bridge_XcDESKeyParity(void)
{
    xbox_XcDESKeyParity((PUCHAR)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1));
    g_eax = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * The rest of the xbox_* implementations that had no route. Each was checked
 * against the memory-model bar: reads or writes only happen at
 * caller-supplied guest addresses, XBOX_TO_NATIVE maps guest NULL to host
 * NULL, and nothing allocates, frees, or hands back a host pointer unless the
 * exception is stated in place. I/O Manager and HAL entries are mostly
 * documented stubs whose whole contract is a return value; the Xc* crypto
 * entries are stubs and guest-buffer operations.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── DbgBreakPoint (ordinal 5, void) */
static void bridge_DbgBreakPoint(void)
{
    xbox_DbgBreakPoint();
    g_eax = 0;
}

/* ── AvSetSavedDataAddress (ordinal 4, 1 arg) */
static void bridge_AvSetSavedDataAddress(void)
{
    xbox_AvSetSavedDataAddress(STACK_ARG(0));
    g_eax = 0;
}

/* ── HalDisableSystemInterrupt (ordinal 39, 2 args) */
static void bridge_HalDisableSystemInterrupt(void)
{
    xbox_HalDisableSystemInterrupt(STACK_ARG(0), (KIRQL)STACK_ARG(1));
    g_eax = 0;
}

/* ── HalInitiateShutdown (ordinal 360, void)
 * The implementation calls ExitProcess, so this never actually returns. */
static void bridge_HalInitiateShutdown(void)
{
    xbox_HalInitiateShutdown();
    g_eax = 0;
}

/* ── HalIsResetOrShutdownPending (ordinal 358, void) */
static void bridge_HalIsResetOrShutdownPending(void)
{
    g_eax = (uint32_t)xbox_HalIsResetOrShutdownPending();
}

/* ── WRITE_PORT_BUFFER_ULONG / WRITE_PORT_BUFFER_USHORT (334 / 333, 3 args)
 * Port I/O stubs that ignore every argument, so pointers pass through without
 * translation -- nothing dereferences them. */
static void bridge_WRITE_PORT_BUFFER_ULONG(void)
{
    xbox_WRITE_PORT_BUFFER_ULONG((PULONG)(uintptr_t)STACK_ARG(0),
        (PULONG)(uintptr_t)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

static void bridge_WRITE_PORT_BUFFER_USHORT(void)
{
    xbox_WRITE_PORT_BUFFER_USHORT((PUSHORT)(uintptr_t)STACK_ARG(0),
        (PUSHORT)(uintptr_t)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

/* ── I/O Manager stubs (ordinals 61, 62, 69, 73, 74, 79, 81-87, 359)
 *
 * The Xbox I/O manager is used internally by the XDK libraries we replace, so
 * these are all stubs whose contract is a return value plus occasional writes
 * to caller-supplied guest structures (IRP, IO_STATUS_BLOCK, symlink table).
 * None of them hands back a pointer the title dereferences.
 *
 * IofCallDriver / IofCompleteRequest are __fastcall on Xbox, so their
 * arguments live in ecx/edx, never on the stack -- the arg-size entries are 0
 * and reading STACK_ARG would dereference whatever the caller left there.
 */
/* ── IRPs in guest memory (XBA-138) ──
 *
 * Layout source. The IRP header is nxdk's xboxkrnl.h (struct _IRP, CC0),
 * whose offsets Cxbx-Reloaded's xboxkrnl types state field by field up to
 * Tail (0x3C): Type 0x00, Size 0x02, Flags 0x04, ThreadListEntry 0x08,
 * IoStatus 0x10, StackCount 0x18, CurrentLocation 0x19, PendingReturned
 * 0x1A, Cancel 0x1B, UserIosb 0x1C, UserEvent 0x20, Overlay (8-aligned)
 * 0x28, UserBuffer 0x30, SegmentArray 0x34, LockedBufferLength 0x38, Tail
 * 0x3C. Tail.Overlay is nxdk's (DriverContext[5], so Thread 0x50,
 * ListEntry 0x54, CurrentStackLocation 0x5C, OriginalFileObject 0x60;
 * Cxbx has DriverContext[4], which would move those four down by 4) and
 * the size is 0x3C + KAPC (0x28) rounded to 8 = 0x68. There is no
 * MdlAddress or SystemBuffer: the console's IRP carries the caller's
 * buffer in UserBuffer.
 *
 * IO_STACK_LOCATION: MajorFunction..Control in the first four bytes and
 * Parameters at 0x04 (Read/Write: Length 0x04, Key 0x08, ByteOffset 0x0C)
 * are Cxbx's. The rest -- DeviceObject 0x14, FileObject 0x18,
 * CompletionRoutine 0x1C, Context 0x20, 0x24 bytes in all -- is the NT x86
 * stack location from memory, NOT checked against a console source: nxdk
 * does not define the structure and Cxbx's (0x18 bytes, no completion
 * routine) cannot be the whole of it, since Xbox drivers set completion
 * routines. The semantics (IoInitializeIrp, IoBuildAsynchronousFsdRequest,
 * IofCallDriver, IofCompleteRequest) are NT's, from memory. If a title's
 * driver shows another stack-location size, IRP_SL_SIZE is the one number
 * to change. RECOMP_IRP_DISPATCH=0 puts back the stubs of before XBA-138
 * (IofCallDriver answers success without dispatching, IofCompleteRequest
 * does nothing, IoBuildAsynchronousFsdRequest answers NULL). */
#define IRP_SIZE              0x68u
#define IRP_SL_SIZE           0x24u
#define IRP_TYPE              6u      /* IO_TYPE_IRP */
#define IRP_O_TYPE            0x00u
#define IRP_O_SIZE            0x02u
#define IRP_O_THREADLIST      0x08u
#define IRP_O_STATUS          0x10u
#define IRP_O_INFORMATION     0x14u
#define IRP_O_STACKCOUNT      0x18u
#define IRP_O_CURLOC          0x19u
#define IRP_O_PENDING         0x1Au
#define IRP_O_USERIOSB        0x1Cu
#define IRP_O_USEREVENT       0x20u
#define IRP_O_USERBUFFER      0x30u
#define IRP_O_THREAD          0x50u
#define IRP_O_CURSTACK        0x5Cu
#define SL_O_MAJOR            0x00u
#define SL_O_CONTROL          0x03u
#define SL_O_LENGTH           0x04u
#define SL_O_BYTEOFFSET       0x0Cu
#define SL_O_DEVICE           0x14u
#define SL_O_COMPLETION       0x1Cu
#define SL_O_CONTEXT          0x20u
#define SL_PENDING_RETURNED   0x01u
#define SL_INVOKE_ON_SUCCESS  0x40u
#define SL_INVOKE_ON_ERROR    0x80u
#define DEVOBJ_O_DRIVER       0x08u
#define DEVOBJ_O_STACKSIZE    0x1Eu   /* nxdk: DeviceType 0x1C, StartIoFlags 0x1D */
#define DRVOBJ_O_MAJOR        0x0Cu   /* after StartIo, DeleteDevice, DismountVolume */
#define IRP_MJ_READ_          0x02u
#define IRP_MJ_WRITE_         0x03u
#define IRP_MJ_MAXIMUM_       0x0Du
#define STATUS_MORE_PROCESSING_REQUIRED_ 0xC0000016u
#define STATUS_INVALID_DEVICE_REQUEST_   0xC0000010u

static int irp_dispatch_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_IRP_DISPATCH");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

/* IoInitializeIrp: zeroed, Type/Size/StackCount, CurrentLocation one past
 * the last stack location and CurrentStackLocation pointing there, as NT's
 * does; ThreadListEntry an empty list. */
static void irp_init(uint32_t irp, uint32_t packet_size, uint32_t stack_size)
{
    memset(XBOX_TO_NATIVE(irp), 0, packet_size);
    BRIDGE_MEM16(irp + IRP_O_TYPE) = (uint16_t)IRP_TYPE;
    BRIDGE_MEM16(irp + IRP_O_SIZE) = (uint16_t)packet_size;
    BRIDGE_MEM8(irp + IRP_O_STACKCOUNT) = (uint8_t)stack_size;
    BRIDGE_MEM8(irp + IRP_O_CURLOC) = (uint8_t)(stack_size + 1);
    BRIDGE_MEM32(irp + IRP_O_THREADLIST) = irp + IRP_O_THREADLIST;
    BRIDGE_MEM32(irp + IRP_O_THREADLIST + 4) = irp + IRP_O_THREADLIST;
    BRIDGE_MEM32(irp + IRP_O_CURSTACK) = irp + IRP_SIZE + stack_size * IRP_SL_SIZE;
}

/* IoAllocateIrp's body: the guest heap, freed by IoFreeIrp. 0 on failure. */
static uint32_t irp_alloc(uint32_t stack_size)
{
    uint32_t total, va;

    stack_size &= 0xFFu;                  /* CCHAR */
    total = IRP_SIZE + stack_size * IRP_SL_SIZE;
    va = xbox_HeapAlloc(total, 16);
    if (!va) {
        fprintf(stderr, "  [KERNEL] IoAllocateIrp: %u bytes REFUSED\n", total);
        return 0;
    }
    irp_init(va, total, stack_size);
    return va;
}

/* A guest stdcall routine's NTSTATUS: bridge_call_guest, keeping eax (the
 * caller's other registers come back). `missing` when it is not in the
 * dispatch. */
static uint32_t irp_call_guest(uint32_t va, const uint32_t *args, int n,
                               uint32_t missing)
{
    recomp_func_t fn = recomp_lookup(va);
    CallbackRegs saved;
    int i;

    if (!fn) fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] IRP routine 0x%08X unresolved\n", va);
        return missing;
    }
    callback_regs_save(&saved);
    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4; BRIDGE_MEM32(g_esp) = args[i];
    }
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;          /* dummy return address */
    {
        int took = xbox_GuestCpuEnterCallback();
        fn();
        xbox_GuestCpuLeaveCallback(took);
    }
    {
        uint32_t r = g_eax;
        callback_regs_restore(&saved, 0);
        return r;
    }
}

/* KeSetEvent on a guest KEVENT, through its bridge, on a frame of its own
 * laid out as the dispatcher hands one over (return address already
 * popped: the first argument at esp). */
static void bridge_KeSetEvent(void);
static void irp_set_event(uint32_t ev)
{
    uint32_t esp = g_esp, eax = g_eax;
    g_esp -= 12;
    BRIDGE_MEM32(g_esp) = ev;
    BRIDGE_MEM32(g_esp + 4) = 0;
    BRIDGE_MEM32(g_esp + 8) = 0;
    bridge_KeSetEvent();
    g_esp = esp;
    g_eax = eax;
}

/* IofCompleteRequest (NT's, from memory): from the current stack location
 * up, each completion routine whose SL_INVOKE_ON_* bit matches the status
 * runs as Routine(DeviceObject, Irp, Context); STATUS_MORE_PROCESSING_REQUIRED
 * stops there (the routine owns the IRP: it frees or reuses it). Otherwise
 * the status goes to *UserIosb and UserEvent is set. The IRP is never freed
 * here: one built by IoBuildAsynchronousFsdRequest is the caller's to free
 * (IoFreeIrp), on the console as in NT. */
static void irp_complete(uint32_t irp)
{
    uint32_t count = BRIDGE_MEM8(irp + IRP_O_STACKCOUNT);
    uint32_t iosb, ev;

    for (;;) {
        uint32_t loc = BRIDGE_MEM8(irp + IRP_O_CURLOC);
        uint32_t sl = BRIDGE_MEM32(irp + IRP_O_CURSTACK);
        uint32_t status, ctl, routine, invoke;

        if (loc > count)
            break;
        /* Step up first, as NT does: the routine sees its caller's location. */
        BRIDGE_MEM8(irp + IRP_O_CURLOC) = (uint8_t)(loc + 1);
        BRIDGE_MEM32(irp + IRP_O_CURSTACK) = sl + IRP_SL_SIZE;
        ctl = BRIDGE_MEM8(sl + SL_O_CONTROL);
        BRIDGE_MEM8(irp + IRP_O_PENDING) = (uint8_t)(ctl & SL_PENDING_RETURNED);
        status = BRIDGE_MEM32(irp + IRP_O_STATUS);
        routine = BRIDGE_MEM32(sl + SL_O_COMPLETION);
        invoke = (int32_t)status >= 0 ? SL_INVOKE_ON_SUCCESS : SL_INVOKE_ON_ERROR;
        if (routine && (ctl & invoke)) {
            uint32_t dev = loc + 1 <= count
                         ? BRIDGE_MEM32(sl + IRP_SL_SIZE + SL_O_DEVICE) : 0;
            uint32_t a[3] = { dev, irp, BRIDGE_MEM32(sl + SL_O_CONTEXT) };
            if (irp_call_guest(routine, a, 3, 0) == STATUS_MORE_PROCESSING_REQUIRED_)
                return;
        }
    }
    iosb = BRIDGE_MEM32(irp + IRP_O_USERIOSB);
    if (iosb) {
        BRIDGE_MEM32(iosb) = BRIDGE_MEM32(irp + IRP_O_STATUS);
        BRIDGE_MEM32(iosb + 4) = BRIDGE_MEM32(irp + IRP_O_INFORMATION);
    }
    ev = BRIDGE_MEM32(irp + IRP_O_USEREVENT);
    if (ev)
        irp_set_event(ev);
}

/* IofCallDriver (NT's, from memory): the next stack location becomes the
 * current one and takes the device, and the device's driver dispatches on
 * the major function: DriverObject->MajorFunction[major](DeviceObject, Irp).
 * Only a title's own driver has one (IoCreateDevice keeps the title's
 * DRIVER_OBJECT; this runtime hands out no kernel device objects). No
 * dispatch: the IRP completes with STATUS_INVALID_DEVICE_REQUEST, which is
 * IoInvalidDeviceRequest's answer, and that is returned. */
static uint32_t irp_call_driver(uint32_t dev, uint32_t irp)
{
    uint32_t loc, sl, major, drv, dispatch = 0;

    if (!dev || !irp)
        return STATUS_INVALID_PARAMETER;
    loc = BRIDGE_MEM8(irp + IRP_O_CURLOC);
    if (loc <= 1) {
        fprintf(stderr, "  [KERNEL] IofCallDriver: IRP 0x%08X has no stack location left\n", irp);
        return STATUS_INVALID_PARAMETER;
    }
    BRIDGE_MEM8(irp + IRP_O_CURLOC) = (uint8_t)(loc - 1);
    sl = BRIDGE_MEM32(irp + IRP_O_CURSTACK) - IRP_SL_SIZE;
    BRIDGE_MEM32(irp + IRP_O_CURSTACK) = sl;
    BRIDGE_MEM32(sl + SL_O_DEVICE) = dev;
    major = BRIDGE_MEM8(sl + SL_O_MAJOR);
    drv = BRIDGE_MEM32(dev + DEVOBJ_O_DRIVER);
    if (drv && major <= IRP_MJ_MAXIMUM_)
        dispatch = BRIDGE_MEM32(drv + DRVOBJ_O_MAJOR + 4 * major);
    if (dispatch) {
        uint32_t a[2] = { dev, irp };
        return irp_call_guest(dispatch, a, 2, STATUS_INVALID_DEVICE_REQUEST_);
    }
    BRIDGE_MEM32(irp + IRP_O_STATUS) = STATUS_INVALID_DEVICE_REQUEST_;
    BRIDGE_MEM32(irp + IRP_O_INFORMATION) = 0;
    irp_complete(irp);
    return STATUS_INVALID_DEVICE_REQUEST_;
}

static void bridge_IoBuildDeviceIoControlRequest(void)
{
    g_eax = (uint32_t)xbox_IoBuildDeviceIoControlRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        (BOOLEAN)STACK_ARG(6), bridge_resolve_handle(STACK_ARG(7)),
        (PXBOX_IO_STATUS_BLOCK)XBOX_TO_NATIVE(STACK_ARG(8)));
}

static void bridge_IoBuildSynchronousFsdRequest(void)
{
    g_eax = (uint32_t)(uintptr_t)xbox_IoBuildSynchronousFsdRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), bridge_resolve_handle(STACK_ARG(5)),
        (PXBOX_IO_STATUS_BLOCK)XBOX_TO_NATIVE(STACK_ARG(6)));
}

static void bridge_IoDeleteSymbolicLink(void)
{
    uint32_t name_va = STACK_ARG(0);
    XBOX_ANSI_STRING name;

    if (!name_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va + 0);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_IoDeleteSymbolicLink(&name);
}

static void bridge_IoInitializeIrp(void)
{
    if (!irp_dispatch_on())
        xbox_IoInitializeIrp(XBOX_TO_NATIVE(STACK_ARG(0)),
            (USHORT)STACK_ARG(1), (CCHAR)STACK_ARG(2));
    else if (STACK_ARG(0))
        irp_init(STACK_ARG(0), STACK_ARG(1) & 0xFFFFu, STACK_ARG(2) & 0xFFu);
    g_eax = 0;
}

static void bridge_IoInvalidDeviceRequest(void)
{
    g_eax = (uint32_t)xbox_IoInvalidDeviceRequest(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)));
}

static void bridge_IoMarkIrpMustComplete(void)
{
    xbox_IoMarkIrpMustComplete(XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_IoSetIoCompletion(void)
{
    g_eax = (uint32_t)xbox_IoSetIoCompletion(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), (NTSTATUS)STACK_ARG(3),
        (ULONG_PTR)STACK_ARG(4));
}

static void bridge_IoStartNextPacket(void)
{
    xbox_IoStartNextPacket(XBOX_TO_NATIVE(STACK_ARG(0)),
        (BOOLEAN)STACK_ARG(1));
    g_eax = 0;
}

static void bridge_IoStartNextPacketByKey(void)
{
    xbox_IoStartNextPacketByKey(XBOX_TO_NATIVE(STACK_ARG(0)),
        (BOOLEAN)STACK_ARG(1), STACK_ARG(2));
    g_eax = 0;
}

static void bridge_IoStartPacket(void)
{
    xbox_IoStartPacket(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), (PULONG)XBOX_TO_NATIVE(STACK_ARG(2)),
        XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

static void bridge_IoSynchronousDeviceIoControlRequest(void)
{
    g_eax = (uint32_t)xbox_IoSynchronousDeviceIoControlRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(6)), (BOOLEAN)STACK_ARG(7));
}

static void bridge_IoSynchronousFsdRequest(void)
{
    g_eax = (uint32_t)xbox_IoSynchronousFsdRequest(
        STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
        XBOX_TO_NATIVE(STACK_ARG(4)));
}

static void bridge_IofCallDriver(void)
{
    if (!irp_dispatch_on())
        g_eax = (uint32_t)xbox_IofCallDriver(XBOX_TO_NATIVE(g_ecx),
                                            XBOX_TO_NATIVE(g_edx));
    else
        g_eax = irp_call_driver(g_ecx, g_edx);
}

static void bridge_IofCompleteRequest(void)
{
    if (irp_dispatch_on() && g_ecx)
        irp_complete(g_ecx);
    else
        xbox_IofCompleteRequest(XBOX_TO_NATIVE(g_ecx), (CCHAR)g_edx);
    g_eax = 0;
}

/* ── MmLockUnlockPhysicalPage (ordinal 176, 2 args)
 * Page locking is a no-op in user mode; the physical address is a value,
 * never dereferenced. */
static void bridge_MmLockUnlockPhysicalPage(void)
{
    xbox_MmLockUnlockPhysicalPage(STACK_ARG(0), (BOOLEAN)STACK_ARG(1));
    g_eax = 0;
}

/* ── MmFreeSystemMemory (ordinal 172, 2 args)
 * Guest-heap twin of the allocator above; nothing on the host heap has ever
 * seen this address. */
static void bridge_MmFreeSystemMemory(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmCreateKernelStack / MmDeleteKernelStack (169 / 170, 2 args)
 *
 * Same memory model as the pair above. The Xbox convention is that
 * MmCreateKernelStack returns the TOP of the stack and the delete call
 * receives (StackBase=top, StackLimit=base). Both halves of that come from the
 * guest heap here: create returns base+size, delete frees the base (arg 1). */
static void bridge_MmCreateKernelStack(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t base_va = xbox_HeapAlloc(size, 16);

    g_eax = base_va ? base_va + size : 0;
}

static void bridge_MmDeleteKernelStack(void)
{
    xbox_HeapFree(STACK_ARG(1));
    g_eax = 0;
}

/* ── Xc* remaining crypto (ordinals 341-345, 347-351)
 *
 * RSA, XcModExp and DES are real (kernel_crypto.c, which documents the buffer
 * layouts): every pointer is a guest buffer, translated as is.
 * XcVerifyPKCS1Signature checks for real but accepts a failure unless
 * RECOMP_PKCS1_VERIFY=strict: what titles verify is signed with Microsoft's
 * key, which is not here. */
static void bridge_XcPKGetKeyLen(void)
{
    g_eax = (uint32_t)xbox_XcPKGetKeyLen(XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_XcPKDecPrivate(void)
{
    g_eax = (uint32_t)xbox_XcPKDecPrivate(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcPKEncPublic(void)
{
    g_eax = (uint32_t)xbox_XcPKEncPublic(XBOX_TO_NATIVE(STACK_ARG(0)),
        XBOX_TO_NATIVE(STACK_ARG(1)), XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcVerifyPKCS1Signature(void)
{
    g_eax = (uint32_t)xbox_XcVerifyPKCS1Signature(
        XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)));
}

static void bridge_XcModExp(void)
{
    g_eax = (uint32_t)xbox_XcModExp((PULONG)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(2)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)), STACK_ARG(4));
}

static void bridge_XcKeyTable(void)
{
    xbox_XcKeyTable(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcBlockCrypt(void)
{
    xbox_XcBlockCrypt(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)),
        XBOX_TO_NATIVE(STACK_ARG(2)), XBOX_TO_NATIVE(STACK_ARG(3)),
        STACK_ARG(4));
    g_eax = 0;
}

static void bridge_XcBlockCryptCBC(void)
{
    xbox_XcBlockCryptCBC(STACK_ARG(0), STACK_ARG(1),
        XBOX_TO_NATIVE(STACK_ARG(2)), XBOX_TO_NATIVE(STACK_ARG(3)),
        XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
        XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

static void bridge_XcCryptService(void)
{
    g_eax = (uint32_t)xbox_XcCryptService(STACK_ARG(0), XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* The original CRYPTO_VECTOR (16 entries, ordinals 335-350) the title gets
 * back holds the guest addresses of those exports: the synthetic thunk VA of
 * the slot the title imported each through, 0 for one it did not import. */
static void bridge_XcUpdateCrypto(void)
{
    uint32_t rom = STACK_ARG(1);
    int i, slot;

    xbox_XcUpdateCrypto(XBOX_TO_NATIVE(STACK_ARG(0)), XBOX_TO_NATIVE(rom));
    if (rom && bridge_va_mapped(rom, 16 * 4))
        for (i = 0; i < 16; i++) {
            uint32_t va = 0;
            for (slot = 0; slot < XBOX_KERNEL_THUNK_TABLE_SIZE; slot++)
                if (g_slot_ordinals[slot] == (ULONG)(335 + i)) {
                    va = KERNEL_VA_BASE + (uint32_t)slot * 4;
                    break;
                }
            BRIDGE_MEM32(rom + 4 * (uint32_t)i) = va;
        }
    g_eax = 0;
}

/* ── RtlRip (ordinal 352, 3 args)
 * The CRT rip/assert path. All three pointers are guest C strings, so each is
 * translated; nothing is written. */
static void bridge_RtlRip(void)
{
    xbox_RtlRip((PCHAR)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PCHAR)XBOX_TO_NATIVE(STACK_ARG(1)),
        (PCHAR)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── RtlAnsiStringToUnicodeString (ordinal 260, 3 args) ──────────────────
 *
 * NOT a call into xbox_RtlAnsiStringToUnicodeString. That one HeapAllocs on
 * the host heap and stores the resulting 64-bit host pointer into the guest
 * string's 4-byte Buffer field -- the memory-model mismatch. This bridge does
 * the whole conversion by hand:
 *
 *  - Source fields are read from the guest struct (Buffer at offset 4);
 *  - the destination stays wherever the title put it (non-allocating), or is
 *    carved from the guest heap (allocating) and its guest VA stored at
 *    offset 4;
 *  - conversion writes only at guest addresses.
 */
static void bridge_RtlAnsiStringToUnicodeString(void)
{
    uint32_t dst_va  = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint32_t s_len, s_buf_va, d_max, d_buf_va = 0;
    uint32_t unicode_bytes;
    int result;

    if (!dst_va || !src_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    s_len   = BRIDGE_MEM16(src_va + 0);
    s_buf_va = BRIDGE_MEM32(src_va + 4);
    if (!s_buf_va && s_len) {          /* an empty string may have no buffer */
        g_eax = 0xC000000Du;
        return;
    }
    unicode_bytes = (uint32_t)(s_len + 1) * sizeof(WCHAR);

    d_max = BRIDGE_MEM16(dst_va + 2);
    if (do_alloc) {
        d_buf_va = xbox_HeapAlloc(unicode_bytes, 16);
        if (!d_buf_va) {
            g_eax = 0xC0000017u;   /* STATUS_NO_MEMORY */
            return;
        }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)unicode_bytes;
        BRIDGE_MEM32(dst_va + 4) = d_buf_va;
    } else {
        if (d_max < unicode_bytes) {
            g_eax = 0xC0000205u;   /* STATUS_BUFFER_OVERFLOW */
            return;
        }
        d_buf_va = BRIDGE_MEM32(dst_va + 4);
        if (!d_buf_va) {
            g_eax = 0xC000000Du;
            return;
        }
    }

    /* An empty source converts to an empty string (KRN-12): the
     * conversion returns 0 for it, which read as a failure, left Length
     * unset and leaked the buffer just allocated. NT answers success. */
    result = s_len == 0 ? 0 : MultiByteToWideChar(CP_ACP, 0,
        (const char*)XBOX_TO_NATIVE(s_buf_va), (int)s_len,
        (WCHAR*)XBOX_TO_NATIVE(d_buf_va), (int)(unicode_bytes / sizeof(WCHAR)));
    if (result > 0 || s_len == 0) {
        BRIDGE_MEM16(dst_va + 0) = (uint16_t)(result * sizeof(WCHAR));
        ((WCHAR*)XBOX_TO_NATIVE(d_buf_va))[result] = 0;
        g_eax = 0;
    } else {
        if (do_alloc) {
            xbox_HeapFree(d_buf_va);
            BRIDGE_MEM16(dst_va + 2) = 0;
            BRIDGE_MEM32(dst_va + 4) = 0;
        }
        g_eax = 0xC0000001u;   /* STATUS_UNSUCCESSFUL */
    }
}

/* ── RtlUnicodeStringToAnsiString (ordinal 308, 3 args)
 * Mirror of the ANSI→Unicode bridge above. */
static void bridge_RtlUnicodeStringToAnsiString(void)
{
    uint32_t dst_va  = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint32_t s_len, s_buf_va, d_max, d_buf_va = 0;
    uint32_t ansi_bytes;
    int result;

    if (!dst_va || !src_va) {
        g_eax = 0xC000000Du;
        return;
    }
    s_len   = BRIDGE_MEM16(src_va + 0);
    s_buf_va = BRIDGE_MEM32(src_va + 4);
    if (!s_buf_va && s_len) {          /* an empty string may have no buffer */
        g_eax = 0xC000000Du;
        return;
    }
    ansi_bytes = s_len / sizeof(WCHAR) + 1;

    d_max = BRIDGE_MEM16(dst_va + 2);
    if (do_alloc) {
        d_buf_va = xbox_HeapAlloc(ansi_bytes, 16);
        if (!d_buf_va) {
            g_eax = 0xC0000017u;
            return;
        }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)ansi_bytes;
        BRIDGE_MEM32(dst_va + 4) = d_buf_va;
        d_max = ansi_bytes;            /* the room the terminator check uses */
    } else {
        if (d_max < ansi_bytes) {
            g_eax = 0xC0000205u;
            return;
        }
        d_buf_va = BRIDGE_MEM32(dst_va + 4);
        if (!d_buf_va) {
            g_eax = 0xC000000Du;
            return;
        }
    }

    result = s_len / sizeof(WCHAR) == 0 ? 0 : WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR*)XBOX_TO_NATIVE(s_buf_va), (int)(s_len / sizeof(WCHAR)),
        (char*)XBOX_TO_NATIVE(d_buf_va), (int)ansi_bytes, NULL, NULL);
    if (result > 0 || s_len / sizeof(WCHAR) == 0) {   /* empty: success (KRN-12) */
        BRIDGE_MEM16(dst_va + 0) = (uint16_t)result;
        if ((uint16_t)result < d_max)
            ((char*)XBOX_TO_NATIVE(d_buf_va))[result] = 0;
        g_eax = 0;
    } else {
        if (do_alloc) {
            xbox_HeapFree(d_buf_va);
            BRIDGE_MEM16(dst_va + 2) = 0;
            BRIDGE_MEM32(dst_va + 4) = 0;
        }
        g_eax = 0xC0000001u;
    }
}

/* ── NtDuplicateObject (ordinal 197, 3 args)
 *
 * Xbox NtDuplicateObject has one process, and the handle table is ours: a
 * by-value source token in, a 4-byte guest slot for the duplicated target.
 * The source resolves through the table, DuplicateHandle produces a real
 * native handle, and the target lands via bridge_write_handle as a fresh
 * token -- same shape as NtCreateEvent. DUPLICATE_CLOSE_SOURCE also closes
 * the native handle behind the source token, which is why the source token is
 * NOT consumed here (the OS handle it wraps is gone after this call). */
static void bridge_NtDuplicateObject(void)
{
    uint32_t target_va = STACK_ARG(1);
    HANDLE src = bridge_resolve_handle(STACK_ARG(0));
    HANDLE dup = NULL;
    DWORD opts = 0;

    if (!target_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }
    if (STACK_ARG(2) & 0x1) opts |= DUPLICATE_CLOSE_SOURCE;
    if (STACK_ARG(2) & 0x2) opts |= DUPLICATE_SAME_ACCESS;

    if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(),
                         &dup, 0, FALSE, opts)) {
        static int logged = 0;
        if (logged++ < 8) {
            fprintf(stderr, "  [KERNEL] NtDuplicateObject: token=0x%08X "
                    "handle=%p failed (error %lu)\n",
                    STACK_ARG(0), src, (unsigned long)GetLastError());
            fflush(stderr);
        }
        g_eax = 0xC0000001u;   /* STATUS_UNSUCCESSFUL */
        return;
    }
    /* A host may hand back the current-process pseudo-handle itself as the
     * duplicate (the POSIX shim does). It is not a table handle -- the token
     * allocator would read it as INVALID_HANDLE_VALUE -- so give the guest
     * its own NtCurrentProcess() value, which resolves back to it. */
    if (dup == (HANDLE)(intptr_t)-1) {
        BRIDGE_MEM32(target_va) = 0xFFFFFFFFu;
        g_eax = 0;
        return;
    }
    /* The current-thread pseudo-handle back is no duplicate: it names
     * whoever holds it, not this thread, and in the table it made every
     * save-state capture refuse. A host that cannot give the thread a real
     * handle fails the call, and no token is allocated. */
    if (dup == (HANDLE)(intptr_t)-2) {
        static int logged = 0;
        if (!logged++) {
            fprintf(stderr, "  [KERNEL] NtDuplicateObject: token=0x%08X: the "
                    "host returned the current-thread pseudo-handle; "
                    "failing\n", STACK_ARG(0));
            fflush(stderr);
        }
        g_eax = 0xC0000001u;   /* STATUS_UNSUCCESSFUL */
        return;
    }
    BRIDGE_MEM32(target_va) = bridge_handle_token_ex(dup, 1);
    /* NtCurrentThread() is no table token: its thread is the caller's. */
    thobj_attach_token(BRIDGE_MEM32(target_va),
                       STACK_ARG(0) == 0xFFFFFFFEu ? thobj_current()
                                                   : thobj_from_token(STACK_ARG(0)));
    if (opts & DUPLICATE_CLOSE_SOURCE)
        thobj_detach_token(STACK_ARG(0));
    g_eax = 0;
}

/* ── Dispatch table: ordinal → bridge function + stack arg bytes ── */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
/* ---- Section B: Rtl* bridge functions (ordinals 261-321) ----
 * Drawn from bridges_Rtl.c and spliced here at byte level. Macro and status
 * definitions inside remain #ifndef-guarded so the block still compiles
 * standalone. Routing lives in the two dispatch tables below. */
/* NTSTATUS values kernel.h does not define. */
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL    ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_INTEGER_OVERFLOW
#define STATUS_INTEGER_OVERFLOW    ((NTSTATUS)0xC0000095L)
#endif

/* ── Guest string helpers ─────────────────────────────────── */

/* Byte length (in UTF-16 bytes) of the guest C string at src_va, capped so the
 * caller can always add a terminator without overflowing USHORT Length. */
static uint32_t bridge_guest_wcslen_bytes(uint32_t src_va)
{
    const uint16_t *wp = (const uint16_t *)XBOX_TO_NATIVE(src_va);
    uint32_t bytes = 0;

    while (wp[bytes / 2]) {
        bytes += sizeof(uint16_t);
        if (bytes > 0xFFFE)
            return 0xFFFE;
    }
    return bytes;
}

/* ── Rtl Append (ordinals 261, 262, 263) ────────────────────
 * Each appends a source to a destination XBOX_*_STRING, honouring the
 * destination MaximumLength; the source bump and the destination length both
 * live in guest memory. Returns STATUS_SUCCESS or STATUS_BUFFER_TOO_SMALL. */

/* 261: NTSTATUS RtlAppendStringToString(PXBOX_ANSI_STRING, PXBOX_ANSI_STRING) */
static void bridge_RtlAppendStringToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (!d_buf || !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if ((uint32_t)d_len + s_len > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(s_buf), s_len);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + s_len);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 262: NTSTATUS RtlAppendUnicodeStringToString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING) */
static void bridge_RtlAppendUnicodeStringToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (!d_buf || !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if ((uint32_t)d_len + s_len > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(s_buf), s_len);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + s_len);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 263: NTSTATUS RtlAppendUnicodeToString(PXBOX_UNICODE_STRING, PCWSTR) */
static void bridge_RtlAppendUnicodeToString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_len = 0, d_max = 0;
    uint32_t d_buf = 0, add_bytes;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_len = BRIDGE_MEM16(dst_va + 0);
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    if (!d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    add_bytes = bridge_guest_wcslen_bytes(src_va);
    if ((uint32_t)d_len + add_bytes > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
    memcpy((char *)XBOX_TO_NATIVE(d_buf) + d_len, XBOX_TO_NATIVE(src_va), add_bytes);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)(d_len + add_bytes);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Compare (ordinals 270, 271, 280) ───────────────────
 * <0 / 0 / >0 against the string bodies (not just the prefix), matching the
 * NT RtlCompare* contract: compare up to the common length, then lengths. */

/* 270: LONG RtlCompareString(PXBOX_ANSI_STRING, PXBOX_ANSI_STRING, BOOLEAN) */
static void bridge_RtlCompareString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t l1, l2, common, i;
    const uint8_t *p1, *p2;
    int diff;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    l1 = BRIDGE_MEM16(s1_va + 0);
    l2 = BRIDGE_MEM16(s2_va + 0);
    p1 = (const uint8_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint8_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if (!p1 || !p2) { g_eax = 0; return; }

    common = l1 < l2 ? l1 : l2;
    diff = 0;
    for (i = 0; i < common; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) { a = tolower(a); b = tolower(b); }
        if (a != b) { diff = a - b; break; }
    }
    if (!diff)
        diff = (int)l1 - (int)l2;
    g_eax = (uint32_t)(int32_t)diff;
}

/* 271: LONG RtlCompareUnicodeString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING, BOOLEAN) */
static void bridge_RtlCompareUnicodeString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t b1, b2, common, i;
    const uint16_t *p1, *p2;
    int diff;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    b1 = BRIDGE_MEM16(s1_va + 0);
    b2 = BRIDGE_MEM16(s2_va + 0);
    p1 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if (!p1 || !p2) { g_eax = 0; return; }

    common = b1 < b2 ? b1 : b2;
    diff = 0;
    for (i = 0; i < common / 2; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) { a = towlower((wint_t)a); b = towlower((wint_t)b); }
        if (a != b) { diff = a - b; break; }
    }
    if (!diff)
        diff = (int)b1 - (int)b2;
    g_eax = (uint32_t)(int32_t)diff;
}

/* 280: BOOLEAN RtlEqualUnicodeString(PXBOX_UNICODE_STRING, PXBOX_UNICODE_STRING, BOOLEAN) */
static void bridge_RtlEqualUnicodeString(void)
{
    uint32_t s1_va = STACK_ARG(0);
    uint32_t s2_va = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    uint32_t b1, b2, i;
    const uint16_t *p1, *p2;

    if (!s1_va || !s2_va) { g_eax = 0; return; }
    b1 = BRIDGE_MEM16(s1_va + 0);
    b2 = BRIDGE_MEM16(s2_va + 0);
    if (b1 != b2) { g_eax = 0; return; }
    p1 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    p2 = (const uint16_t *)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));
    if ((!p1 || !p2) && b1) { g_eax = 0; return; }

    for (i = 0; i < b1 / 2; i++) {
        int a = p1[i], b = p2[i];
        if (nocase) {
            a = towlower((wint_t)a);
            b = towlower((wint_t)b);
        }
        if (a != b) { g_eax = 0; return; }
    }
    g_eax = 1;
}

/* ── Rtl Copy (ordinals 272, 273, 317) ────────────────────── */

/* 272: VOID RtlCopyString(PXBOX_ANSI_STRING Dest, PXBOX_ANSI_STRING Src) */
static void bridge_RtlCopyString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf)
        memcpy(XBOX_TO_NATIVE(d_buf), XBOX_TO_NATIVE(s_buf), copy);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* 273: VOID RtlCopyUnicodeString(PXBOX_UNICODE_STRING Dest, PXBOX_UNICODE_STRING Src) */
static void bridge_RtlCopyUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf)
        memcpy(XBOX_TO_NATIVE(d_buf), XBOX_TO_NATIVE(s_buf), copy);
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* 317: VOID RtlUpperString(PXBOX_ANSI_STRING Dest, PXBOX_ANSI_STRING Src) */
static void bridge_RtlUpperString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint16_t d_max = 0, s_len = 0;
    uint32_t d_buf = 0, s_buf = 0, copy, i;
    const uint8_t *sp;
    uint8_t *dp;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    d_max = BRIDGE_MEM16(dst_va + 2);
    d_buf = BRIDGE_MEM32(dst_va + 4);
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    copy = s_len < d_max ? s_len : d_max;
    if (copy && d_buf && s_buf) {
        sp = (const uint8_t *)XBOX_TO_NATIVE(s_buf);
        dp = (uint8_t *)XBOX_TO_NATIVE(d_buf);
        for (i = 0; i < copy; i++)
            dp[i] = (uint8_t)toupper(sp[i]);
    }
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)copy;
    g_eax = 0;
}

/* ── Rtl Create / Free string (ordinals 274, 287) ─────────── */

/* 274: BOOLEAN RtlCreateUnicodeString(PXBOX_UNICODE_STRING Dest, PCWSTR Src)
 * The buffer is carved from the guest heap so the Buffer field stays a guest
 * VA the title can dereference.  Returns TRUE on success. */
static void bridge_RtlCreateUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t bytes, buf_va;

    if (!dst_va || !src_va) { g_eax = 0; return; }
    bytes = bridge_guest_wcslen_bytes(src_va);
    buf_va = xbox_HeapAlloc(bytes + sizeof(uint16_t), 16);
    if (!buf_va) { g_eax = 0; return; }
    memcpy(XBOX_TO_NATIVE(buf_va), XBOX_TO_NATIVE(src_va), bytes);
    ((uint16_t *)XBOX_TO_NATIVE(buf_va))[bytes / 2] = 0;
    BRIDGE_MEM16(dst_va + 0) = (uint16_t)bytes;
    BRIDGE_MEM16(dst_va + 2) = (uint16_t)(bytes + sizeof(uint16_t));
    BRIDGE_MEM32(dst_va + 4) = buf_va;
    g_eax = 1;
}

/* 287: VOID RtlFreeUnicodeString(PXBOX_UNICODE_STRING Str) */
static void bridge_RtlFreeUnicodeString(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t buf_va;

    if (!str_va) { g_eax = 0; return; }
    buf_va = BRIDGE_MEM32(str_va + 4);
    if (buf_va)
        xbox_HeapFree(buf_va);
    BRIDGE_MEM16(str_va + 0) = 0;
    BRIDGE_MEM16(str_va + 2) = 0;
    BRIDGE_MEM32(str_va + 4) = 0;
    g_eax = 0;
}

/* ── Rtl Control characters (ordinals 275, 296, 313, 316) ── */

/* 275: WCHAR RtlDowncaseUnicodeChar(WCHAR Ch) */
static void bridge_RtlDowncaseUnicodeChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)towlower((wint_t)(uint16_t)ch);
}

/* 296: CHAR RtlLowerChar(CHAR Ch) */
static void bridge_RtlLowerChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint8_t)tolower((int)(uint8_t)ch);
}

/* 313: WCHAR RtlUpcaseUnicodeChar(WCHAR Ch) */
static void bridge_RtlUpcaseUnicodeChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)towupper((wint_t)(uint16_t)ch);
}

/* 316: CHAR RtlUpperChar(CHAR Ch) */
static void bridge_RtlUpperChar(void)
{
    uint32_t ch = STACK_ARG(0);
    g_eax = (uint32_t)(uint8_t)toupper((int)(uint8_t)ch);
}

/* ── Rtl Upcase/Downcase string (ordinals 276, 314) ─────────
 * NT name suggests (2 args) but the real signature takes a third
 * BOOLEAN AllocateDestString (12 stack bytes).  If AllocateDest is set the
 * destination buffer is carved from the guest heap and its guest VA stored in
 * Dest->Buffer; otherwise the caller's existing buffer must fit. */

/* 276: NTSTATUS RtlDowncaseUnicodeString(Dest, Src, AllocDest) */
static void bridge_RtlDowncaseUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint16_t s_len, d_max = 0;
    uint32_t s_buf, d_buf = 0, i;
    const uint16_t *sp;
    uint16_t *dp;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (s_len && !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (do_alloc) {
        d_buf = xbox_HeapAlloc((uint32_t)s_len + sizeof(uint16_t), 16);
        if (!d_buf) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)(s_len + sizeof(uint16_t));
        BRIDGE_MEM32(dst_va + 4) = d_buf;
    } else {
        d_max = BRIDGE_MEM16(dst_va + 2);
        d_buf = BRIDGE_MEM32(dst_va + 4);
        if (d_max < s_len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
        if (s_len && !d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    }

    sp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    dp = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    for (i = 0; i < (uint32_t)(s_len / 2); i++)
        dp[i] = (uint16_t)towlower((wint_t)sp[i]);
    dp[(uint32_t)(s_len / 2)] = 0;
    BRIDGE_MEM16(dst_va + 0) = s_len;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 314: NTSTATUS RtlUpcaseUnicodeString(Dest, Src, AllocDest) */
static void bridge_RtlUpcaseUnicodeString(void)
{
    uint32_t dst_va = STACK_ARG(0);
    uint32_t src_va = STACK_ARG(1);
    uint32_t do_alloc = STACK_ARG(2);
    uint16_t s_len, d_max = 0;
    uint32_t s_buf, d_buf = 0, i;
    const uint16_t *sp;
    uint16_t *dp;

    if (!dst_va || !src_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    s_len = BRIDGE_MEM16(src_va + 0);
    s_buf = BRIDGE_MEM32(src_va + 4);
    if (s_len && !s_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (do_alloc) {
        d_buf = xbox_HeapAlloc((uint32_t)s_len + sizeof(uint16_t), 16);
        if (!d_buf) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        BRIDGE_MEM16(dst_va + 2) = (uint16_t)(s_len + sizeof(uint16_t));
        BRIDGE_MEM32(dst_va + 4) = d_buf;
    } else {
        d_max = BRIDGE_MEM16(dst_va + 2);
        d_buf = BRIDGE_MEM32(dst_va + 4);
        if (d_max < s_len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }
        if (s_len && !d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    }

    sp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    dp = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    for (i = 0; i < (uint32_t)(s_len / 2); i++)
        dp[i] = (uint16_t)towupper((wint_t)sp[i]);
    dp[(uint32_t)(s_len / 2)] = 0;
    BRIDGE_MEM16(dst_va + 0) = s_len;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Integer conversion (ordinals 292, 293) ─────────────
 * NT pads RtlIntegerToChar's output to Length with leading zeros and expects
 * Length+1 bytes of buffer (the NUL goes at [Length]). */

/* 292: NTSTATUS RtlIntegerToChar(ULONG Value, ULONG Base, ULONG Length, PCHAR String) */
static void bridge_RtlIntegerToChar(void)
{
    static const char digs[] = "0123456789ABCDEF";
    uint32_t value = STACK_ARG(0);
    uint32_t base  = STACK_ARG(1);
    uint32_t len   = STACK_ARG(2);
    uint32_t str_va = STACK_ARG(3);
    char tmp[40];
    uint32_t i, n;

    if (!str_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base < 2 || base > 16) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    n = 0;
    {
        uint32_t v = value;
        if (v == 0)
            tmp[n++] = '0';
        while (v) {
            tmp[n++] = digs[v % base];
            v /= base;
        }
        for (i = 0; i < n / 2; i++) {
            char t = tmp[i];
            tmp[i] = tmp[n - 1 - i];
            tmp[n - 1 - i] = t;
        }
    }
    if (n > len) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }

    for (i = 0; i < len - n; i++)
        BRIDGE_MEM8(str_va + i) = (uint8_t)'0';
    for (i = 0; i < n; i++)
        BRIDGE_MEM8(str_va + (len - n + i)) = (uint8_t)tmp[i];
    BRIDGE_MEM8(str_va + len) = 0;
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* 293: NTSTATUS RtlIntegerToUnicodeString(ULONG Value, ULONG Base, PXBOX_UNICODE_STRING String) */
static void bridge_RtlIntegerToUnicodeString(void)
{
    static const uint16_t digs[] = { '0','1','2','3','4','5','6','7','8','9',
                                     'A','B','C','D','E','F' };
    uint32_t value = STACK_ARG(0);
    uint32_t base  = STACK_ARG(1);
    uint32_t str_va = STACK_ARG(2);
    uint16_t body[32], d_max;
    uint32_t d_buf, i, n_body = 0, total;
    uint32_t v;
    int neg = 0;
    uint16_t *dst;

    if (!str_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base < 2 || base > 16) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    d_max = BRIDGE_MEM16(str_va + 2);
    d_buf = BRIDGE_MEM32(str_va + 4);
    if (!d_buf) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }

    if (base == 10 && (LONG)value < 0) {
        neg = 1;
        v = (uint32_t)(0u - value);
    } else {
        v = value;
    }
    if (v == 0)
        body[n_body++] = digs[0];
    while (v) {
        body[n_body++] = digs[v % base];
        v /= base;
    }
    for (i = 0; i < n_body / 2; i++) {
        uint16_t t = body[i];
        body[i] = body[n_body - 1 - i];
        body[n_body - 1 - i] = t;
    }

    total = n_body + (uint32_t)neg;
    if (total * 2 > d_max) { g_eax = (uint32_t)STATUS_BUFFER_TOO_SMALL; return; }

    dst = (uint16_t *)XBOX_TO_NATIVE(d_buf);
    {
        uint32_t k = 0;
        if (neg)
            dst[k++] = (uint16_t)L'-';
        for (i = 0; i < n_body; i++)
            dst[k++] = body[i];
        dst[k] = 0;
    }
    BRIDGE_MEM16(str_va + 0) = (uint16_t)(total * 2);
    g_eax = 0;   /* STATUS_SUCCESS */
}

/* ── Rtl Memory (ordinals 284, 298, 320) ──────────────────── */

/* 284: VOID RtlFillMemory(PVOID Dest, SIZE_T Len, UCHAR Fill) */
static void bridge_RtlFillMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t len     = STACK_ARG(1);
    uint32_t fill    = STACK_ARG(2);

    if (dest_va && len)
        memset(XBOX_TO_NATIVE(dest_va), (int)(UCHAR)fill, (size_t)len);
    g_eax = 0;
}

/* 298: VOID RtlMoveMemory(PVOID Dest, PVOID Src, SIZE_T Len) */
static void bridge_RtlMoveMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);
    uint32_t len     = STACK_ARG(2);

    if (dest_va && src_va && len)
        memmove(XBOX_TO_NATIVE(dest_va), XBOX_TO_NATIVE(src_va), (size_t)len);
    g_eax = 0;
}

/* 320: VOID RtlZeroMemory(PVOID Dest, SIZE_T Len) */
static void bridge_RtlZeroMemory(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t len     = STACK_ARG(1);

    if (dest_va && len)
        memset(XBOX_TO_NATIVE(dest_va), 0, (size_t)len);
    g_eax = 0;
}

/* ── Rtl MultiByte <-> Unicode (ordinals 299, 300, 310, 311, 315) ──
 * All buffers are guest VAs.  MultiByteToWideChar / WideCharToMultiByte are
 * available on both host platforms.  UNICODE_STRING lengths are byte counts. */

/* 299: NTSTATUS RtlMultiByteToUnicodeN(PWCH UnicodeString, ULONG MaxBytesInUnicodeString,
 *       PULONG BytesInUnicodeString, PCCH MultiByteString, ULONG BytesInMultiByteString) */
static void bridge_RtlMultiByteToUnicodeN(void)
{
    uint32_t uni_va    = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va  = STACK_ARG(2);
    uint32_t mb_va     = STACK_ARG(3);
    uint32_t mb_bytes  = STACK_ARG(4);
    int chars, result;

    if (!uni_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    chars = (int)(max_bytes / sizeof(WCHAR));
    result = MultiByteToWideChar(CP_ACP, 0,
        (const char *)XBOX_TO_NATIVE(mb_va), (int)mb_bytes,
        uni_va ? (WCHAR *)XBOX_TO_NATIVE(uni_va) : NULL, chars);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)(result * sizeof(WCHAR)) : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 300: NTSTATUS RtlMultiByteToUnicodeSize(PULONG BytesInUnicodeString,
 *       PCCH MultiByteString, ULONG BytesInMultiByteString) */
static void bridge_RtlMultiByteToUnicodeSize(void)
{
    uint32_t bytes_va = STACK_ARG(0);
    uint32_t mb_va    = STACK_ARG(1);
    uint32_t mb_bytes = STACK_ARG(2);
    int result;

    result = MultiByteToWideChar(CP_ACP, 0,
        (const char *)XBOX_TO_NATIVE(mb_va), (int)mb_bytes, NULL, 0);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)(result * sizeof(WCHAR)) : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 310: NTSTATUS RtlUnicodeToMultiByteN(PCHAR MultiByteString, ULONG MaxBytesInMultiByteString,
 *       PULONG BytesInMultiByteString, PCWCH UnicodeString, ULONG BytesInUnicodeString) */
static void bridge_RtlUnicodeToMultiByteN(void)
{
    uint32_t mb_va    = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va = STACK_ARG(2);
    uint32_t uni_va   = STACK_ARG(3);
    uint32_t uni_bytes = STACK_ARG(4);
    int result;

    if (!mb_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR *)XBOX_TO_NATIVE(uni_va), (int)(uni_bytes / sizeof(WCHAR)),
        mb_va ? (char *)XBOX_TO_NATIVE(mb_va) : NULL, (int)max_bytes,
        NULL, NULL);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 311: NTSTATUS RtlUnicodeToMultiByteSize(PULONG BytesInMultiByteString,
 *       PCWCH UnicodeString, ULONG BytesInUnicodeString) */
static void bridge_RtlUnicodeToMultiByteSize(void)
{
    uint32_t bytes_va = STACK_ARG(0);
    uint32_t uni_va   = STACK_ARG(1);
    uint32_t uni_bytes = STACK_ARG(2);
    int result;

    result = WideCharToMultiByte(CP_ACP, 0,
        (const WCHAR *)XBOX_TO_NATIVE(uni_va), (int)(uni_bytes / sizeof(WCHAR)),
        NULL, 0, NULL, NULL);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* 315: NTSTATUS RtlUpcaseUnicodeToMultiByteN(PCHAR MultiByteString,
 *       ULONG MaxBytesInUnicodeString, PULONG BytesInMultiByteString,
 *       PCWCH UnicodeString, ULONG BytesInUnicodeString)
 * WideCharToMultiByte has no case flag, so the source is upcased into a
 * temporary host buffer first. */
static void bridge_RtlUpcaseUnicodeToMultiByteN(void)
{
    uint32_t mb_va     = STACK_ARG(0);
    uint32_t max_bytes = STACK_ARG(1);
    uint32_t bytes_va  = STACK_ARG(2);
    uint32_t uni_va    = STACK_ARG(3);
    uint32_t uni_bytes = STACK_ARG(4);
    uint32_t wc = uni_bytes / sizeof(WCHAR);
    uint16_t *tmp = NULL;
    int result;
    uint32_t i;

    if (!mb_va && max_bytes) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (wc) {
        tmp = (uint16_t *)malloc(((size_t)wc + 1) * sizeof(uint16_t));
        if (!tmp) { g_eax = (uint32_t)STATUS_NO_MEMORY; return; }
        {
            const uint16_t *sp = (const uint16_t *)XBOX_TO_NATIVE(uni_va);
            for (i = 0; i < wc; i++)
                tmp[i] = (uint16_t)towupper((wint_t)sp[i]);
            tmp[wc] = 0;
        }
    }
    result = WideCharToMultiByte(CP_ACP, 0,
        tmp, (int)wc,
        mb_va ? (char *)XBOX_TO_NATIVE(mb_va) : NULL, (int)max_bytes,
        NULL, NULL);
    free(tmp);
    if (bytes_va)
        BRIDGE_MEM32(bytes_va) = result > 0 ? (uint32_t)result : 0;
    g_eax = result > 0 ? 0 : (uint32_t)STATUS_UNSUCCESSFUL;
}

/* ── Rtl String -> Integer (ordinals 267, 309) ────────────── */

/* 267: NTSTATUS RtlCharToInteger(PCSTR Str, ULONG Base, PULONG Value)
 * Base 0 autodetects (0x -> 16, 0 -> 8, else 10), mirroring strtoul. */
static void bridge_RtlCharToInteger(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t base   = STACK_ARG(1);
    uint32_t val_va = STACK_ARG(2);
    const char *p;
    unsigned long long acc = 0;
    uint32_t base_ = base;
    int neg = 0;
    uint32_t st = 0;   /* STATUS_SUCCESS */

    if (!val_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base_ != 0 && (base_ < 2 || base_ > 16)) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (!str_va) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    p = (const char *)XBOX_TO_NATIVE(str_va);

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\f' || *p == '\v')
        p++;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    if (base_ == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base_ = 16; p += 2; }
        else if (p[0] == '0') { base_ = 8; }
        else base_ = 10;
    }

    for (; *p; p++) {
        int d, c = (unsigned char)*p;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= (int)base_) break;
        if (acc > (0xFFFFFFFFULL - (unsigned)d) / base_) {
            st = (uint32_t)STATUS_INTEGER_OVERFLOW;
            acc = 0xFFFFFFFFULL;
            break;
        }
        acc = acc * base_ + (unsigned)d;
    }
    if (neg)
        acc = (unsigned long long)(uint32_t)(0u - (uint32_t)acc);
    BRIDGE_MEM32(val_va) = (uint32_t)acc;
    g_eax = st;
}

/* 309: NTSTATUS RtlUnicodeStringToInteger(PXBOX_UNICODE_STRING Str, ULONG Base, PULONG Value) */
static void bridge_RtlUnicodeStringToInteger(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t base   = STACK_ARG(1);
    uint32_t val_va = STACK_ARG(2);
    uint16_t s_len;
    uint32_t s_buf;
    const uint16_t *wp;
    unsigned long long acc = 0;
    uint32_t base_ = base;
    int neg = 0, i, nch;
    uint32_t st = 0;   /* STATUS_SUCCESS */

    if (!val_va) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (base_ != 0 && (base_ < 2 || base_ > 16)) { g_eax = (uint32_t)STATUS_INVALID_PARAMETER; return; }
    if (!str_va) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    s_len = BRIDGE_MEM16(str_va + 0);
    s_buf = BRIDGE_MEM32(str_va + 4);
    if (!s_buf) { BRIDGE_MEM32(val_va) = 0; g_eax = 0; return; }
    wp = (const uint16_t *)XBOX_TO_NATIVE(s_buf);
    nch = s_len / 2;

    i = 0;
    while (i < nch && (wp[i] == (uint16_t)' ' || wp[i] == (uint16_t)'\t' ||
                       wp[i] == (uint16_t)'\n' || wp[i] == (uint16_t)'\r'))
        i++;
    if (i < nch && wp[i] == (uint16_t)'-') { neg = 1; i++; }
    else if (i < nch && wp[i] == (uint16_t)'+') i++;

    if (base_ == 0) {
        if (i + 1 < nch && wp[i] == (uint16_t)'0' &&
            (wp[i + 1] == (uint16_t)'x' || wp[i + 1] == (uint16_t)'X')) {
            base_ = 16;
            i += 2;
        } else if (i < nch && wp[i] == (uint16_t)'0') {
            base_ = 8;
        } else {
            base_ = 10;
        }
    }

    for (; i < nch; i++) {
        uint16_t c = wp[i];
        int d;
        if (c >= (uint16_t)'0' && c <= (uint16_t)'9') d = c - (uint16_t)'0';
        else if (c >= (uint16_t)'a' && c <= (uint16_t)'f') d = c - (uint16_t)'a' + 10;
        else if (c >= (uint16_t)'A' && c <= (uint16_t)'F') d = c - (uint16_t)'A' + 10;
        else break;
        if (d >= (int)base_) break;
        if (acc > (0xFFFFFFFFULL - (unsigned)d) / base_) {
            st = (uint32_t)STATUS_INTEGER_OVERFLOW;
            acc = 0xFFFFFFFFULL;
            break;
        }
        acc = acc * base_ + (unsigned)d;
    }
    if (neg)
        acc = (unsigned long long)(uint32_t)(0u - (uint32_t)acc);
    BRIDGE_MEM32(val_va) = (uint32_t)acc;
    g_eax = st;
}

/* ── Rtl Byte swaps (ordinals 307, 318) ─────────────────────
 * Hand-rolled rather than MSVC _byteswap_* so the file stays portable. */

/* 307: ULONG RtlUlongByteSwap(ULONG Value) */
static void bridge_RtlUlongByteSwap(void)
{
    uint32_t v = STACK_ARG(0);
    g_eax = ((v & 0x000000FFu) << 24) |
            ((v & 0x0000FF00u) << 8)  |
            ((v & 0x00FF0000u) >> 8)  |
            ((v & 0xFF000000u) >> 24);
}

/* 318: USHORT RtlUshortByteSwap(USHORT Value) */
static void bridge_RtlUshortByteSwap(void)
{
    uint32_t v = STACK_ARG(0);
    g_eax = (uint32_t)(uint16_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
}

/* ── Rtl Critical sections (ordinals 278, 295, 306) ─────────
 * 277/294 (plain Enter/Leave) are routed via the xbox_Rtl* wrappers already,
 * and TryEnter goes through the same shadow (kernel_rtl.c). */

/* 278: VOID RtlEnterCriticalSectionAndRegion(PRTL_CRITICAL_SECTION) */
static void bridge_RtlEnterCriticalSectionAndRegion(void)
{
    xbox_RtlEnterCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

/* 295: VOID RtlLeaveCriticalSectionAndRegion(PRTL_CRITICAL_SECTION) */
static void bridge_RtlLeaveCriticalSectionAndRegion(void)
{
    xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

/* 306: BOOLEAN RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION) */
static void bridge_RtlTryEnterCriticalSection(void)
{
    g_eax = xbox_RtlTryEnterCriticalSection(
                (PRTL_CRITICAL_SECTION)XBOX_TO_NATIVE(STACK_ARG(0))) ? 1 : 0;
}

/* ── Rtl Misc stubs (ordinals 288, 297, 319, 321) ─────────── */

/* 288: VOID RtlGetCallersAddress(PVOID *CallersAddress, PVOID *CallersCaller) */
static void bridge_RtlGetCallersAddress(void)
{
    uint32_t a_va = STACK_ARG(0);
    uint32_t c_va = STACK_ARG(1);

    if (a_va) BRIDGE_MEM32(a_va) = 0;
    if (c_va) BRIDGE_MEM32(c_va) = 0;
    g_eax = 0;
}

/* 297: VOID RtlMapGenericMask(PACCESS_MASK AccessMask, PRTL_GENERIC_MAPPING) */
/* --- RtlMapGenericMask (ordinal 297, 2 args = 8 bytes)
 * VOID RtlMapGenericMask(PACCESS_MASK AccessMask,
 *                        PGENERIC_MAPPING GenericMapping);
 * GENERIC_READ / WRITE / EXECUTE / ALL (bits 31-28) are replaced by the
 * mapping's GenericRead, GenericWrite, GenericExecute, GenericAll, as NT
 * does (XBA-114). */
static void bridge_RtlMapGenericMask(void)
{
    uint32_t mask_va = STACK_ARG(0), map_va = STACK_ARG(1), m;

    if (!bridge_buf_ok(mask_va, 4, "RtlMapGenericMask") || !bridge_buf_ok(map_va, 16, "RtlMapGenericMask")) {
        g_eax = 0;
        return;
    }
    m = BRIDGE_MEM32(mask_va);
    if (m & 0x80000000u) m |= BRIDGE_MEM32(map_va + 0);
    if (m & 0x40000000u) m |= BRIDGE_MEM32(map_va + 4);
    if (m & 0x20000000u) m |= BRIDGE_MEM32(map_va + 8);
    if (m & 0x10000000u) m |= BRIDGE_MEM32(map_va + 12);
    BRIDGE_MEM32(mask_va) = m & 0x0FFFFFFFu;
    g_eax = 0;
}

/* 319: ULONG RtlWalkFrameChain(PVOID *Callers, ULONG Count, ULONG Flags) */
static uint32_t bridge_walk_frames(uint32_t *out, uint32_t max);   /* below */

/* --- RtlWalkFrameChain (ordinal 319, 3 args = 12 bytes)
 * ULONG RtlWalkFrameChain(PVOID *Callers, ULONG Count, ULONG Flags): up to
 * Count return addresses, their number back. Flags (user-mode frames on NT)
 * means nothing on the console. */
static void bridge_RtlWalkFrameChain(void)
{
    uint32_t out = STACK_ARG(0), want = STACK_ARG(1), frames[64], n, i;

    if (want > 64)
        want = 64;
    n = bridge_walk_frames(frames, want);
    if (!out || !bridge_buf_ok(out, n * 4, "RtlWalkFrameChain")) {
        g_eax = 0;
        return;
    }
    for (i = 0; i < n; i++)
        BRIDGE_MEM32(out + i * 4) = frames[i];
    g_eax = n;
}

/* 321: XboxEEPROMKey (data export, 0 args)
 * Not in the kernel_data_va_for_ordinal table, so it needs a bridge that hands
 * the caller a guest-addressable 16-byte key.  Carved from the guest heap once. */
static void bridge_XboxEEPROMKey(void)
{
    static uint32_t eeprom_va = 0;

    if (!eeprom_va) {
        eeprom_va = xbox_HeapAlloc(16, 16);
        if (eeprom_va)
            memset(XBOX_TO_NATIVE(eeprom_va), 0, 16);
    }
    g_eax = eeprom_va;
}

/* ── Rtl Exception (ordinals 264, 265, 266, 303) ──────────── */

/* 264: VOID RtlAssert(PVOID FailedAssertion, PVOID FileName, ULONG LineNumber) */
static void bridge_RtlAssert(void)
{
    uint32_t assert_va = STACK_ARG(0);
    uint32_t file_va   = STACK_ARG(1);
    uint32_t line      = STACK_ARG(2);
    const char *a = assert_va ? (const char *)XBOX_TO_NATIVE(assert_va) : NULL;
    const char *f = file_va   ? (const char *)XBOX_TO_NATIVE(file_va) : NULL;

    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlAssert: %s:%u %s",
             f ? f : "?", line, a ? a : "?");
#ifdef _DEBUG
    DebugBreak();
#endif
    g_eax = 0;
}

/* 265: VOID RtlCaptureContext(PCONTEXT Context) - stub: zero the x86 CONTEXT. */
static void bridge_RtlCaptureContext(void)
{
    uint32_t ctx_va = STACK_ARG(0);

    if (ctx_va)
        memset(XBOX_TO_NATIVE(ctx_va), 0, 0x2CC);
    g_eax = 0;
}

/* 266: ULONG RtlCaptureStackBackTrace(FramesToSkip, FramesToCapture,
 *       BackTrace, BackTraceHash) - stub, returns 0 frames. Stack is cleaned
 *       up per stdcall_args_for_ordinal (20 bytes in kernel_bridge.c). */
/* ── Guest stack walks (XBA-114) ──
 *
 * The return addresses up the guest's frame-pointer chain, as the
 * console's x86 RtlWalkFrameChain finds them: the first is where the call
 * into the kernel returns, then [ebp+4] for each saved ebp. A frame whose
 * ebp is unmapped, misaligned or not above the last ends the walk -- a
 * function built without a frame pointer ends it early, as on the console. */
static uint32_t bridge_walk_frames(uint32_t *out, uint32_t max)
{
    uint32_t n = 0, ebp = g_ebp, prev = 0;

    if (max && g_xbox_kernel_caller)
        out[n++] = g_xbox_kernel_caller;
    while (n < max && ebp > prev && !(ebp & 3u) && bridge_va_mapped(ebp, 8)) {
        uint32_t ret = BRIDGE_MEM32(ebp + 4);
        if (!ret)
            break;
        out[n++] = ret;
        prev = ebp;
        ebp = BRIDGE_MEM32(ebp);
    }
    return n;
}

/* --- RtlCaptureStackBackTrace (ordinal 266, 4 args = 16 bytes)
 * USHORT RtlCaptureStackBackTrace(ULONG FramesToSkip, ULONG FramesToCapture,
 *                                 PVOID *BackTrace, PULONG BackTraceHash);
 * Four arguments (nxdk xboxkrnl.h); the table said five. The frames after
 * the first FramesToSkip, their count back, and their sum as the hash. */
static void bridge_RtlCaptureStackBackTrace(void)
{
    uint32_t skip = STACK_ARG(0), want = STACK_ARG(1), out = STACK_ARG(2), hash_va = STACK_ARG(3);
    uint32_t frames[64], n, i, hash = 0, got = 0;

    if (want > 64)
        want = 64;
    n = bridge_walk_frames(frames, skip + want < 64 ? skip + want : 64);
    if (out && bridge_buf_ok(out, want * 4, "RtlCaptureStackBackTrace"))
        for (i = skip; i < n && got < want; i++, got++) {
            BRIDGE_MEM32(out + got * 4) = frames[i];
            hash += frames[i];
        }
    if (hash_va && bridge_buf_ok(hash_va, 4, "RtlCaptureStackBackTrace"))
        BRIDGE_MEM32(hash_va) = hash;
    g_eax = got;
}


/* --- Ke Object Shadow Table ---
 *
 * Guest dispatcher object VA -> host object. Every KeInitializeEvent /
 * Mutant / Semaphore / TimerEx, every event a title builds itself
 * (ke_guest_event, in_use 2) and every spawned thread's KTHREAD has one.
 *
 * It was 4096 slots scanned from index 0 under one lock, and only threads
 * were ever removed (thobj_unref). XDK code puts KEVENTs on the stack and
 * in heap objects (overlapped requests, USB requests, stream buffers), so
 * the distinct addresses grow with play time; after 4096 ke_guest_event
 * returned NULL without a word, the guest address went to the host as a
 * HANDLE, SetEvent failed silently and the wait failed at once -- the
 * KOTOR / T()NY failures described below, reached by a long session. Every
 * KeSetEvent / KeWaitFor* walked the array, a miss all 4096 slots (KRN-2).
 *
 * Now an open-addressed hash on the VA (linear probing, deletion by
 * backward shift, so no tombstones), doubled at half load. Before it grows
 * past KE_SHADOW_SWEEP entries it drops the shadows whose guest memory no
 * longer holds the object they were made for: the header's Type or Size
 * byte changed since the insert (a title-built event's stack frame or heap
 * block was reused). `hdr` is that snapshot, Type | Size << 8, taken once
 * the header is written (ke_shadow_note_header, or at the insert of a
 * title-built event); 0 means the shadow is never dropped this way: a
 * thread's KTHREAD (thobj_unref removes it) and a slot restored by a save
 * state. */
#define KE_SHADOW_SWEEP 4096u
typedef struct { uint32_t guest_va; HANDLE host_handle; uint32_t in_use; uint32_t hdr; } KE_SHADOW_SLOT;
static KE_SHADOW_SLOT *g_ke_shadow;
static uint32_t g_ke_shadow_cap;            /* a power of two, 0 before first use */
static uint32_t g_ke_shadow_count;
static CRITICAL_SECTION g_ke_shadow_cs;
static int g_ke_shadow_init;

static void ke_shadow_init(void)
{
    if (g_ke_shadow_init)
        return;
    InitializeCriticalSection(&g_ke_shadow_cs);
    g_ke_shadow_init = 1;
}

static uint32_t ke_shadow_home(uint32_t va, uint32_t cap)
{
    va ^= va >> 13;
    va *= 0x9E3779B1u;
    return (va >> 8) & (cap - 1u);
}

/* The header bytes that identify the object at va: Type | Size << 8. */
static uint32_t ke_shadow_hdr(uint32_t va)
{
    if (!bridge_va_mapped(va, 4))
        return 0xFFFFFFFFu;
    return (uint32_t)BRIDGE_MEM8(va) | (uint32_t)BRIDGE_MEM8(va + 2) << 8;
}

/* The slot index holding va, or -1. Under g_ke_shadow_cs. */
static int ke_shadow_find_locked(uint32_t va)
{
    uint32_t i;
    if (!g_ke_shadow_cap)
        return -1;
    for (i = ke_shadow_home(va, g_ke_shadow_cap); g_ke_shadow[i].in_use;
         i = (i + 1) & (g_ke_shadow_cap - 1u))
        if (g_ke_shadow[i].guest_va == va)
            return (int)i;
    return -1;
}

/* Empty slot i and shift the run after it back, so lookups need no
 * tombstones. The host object is the caller's. Under g_ke_shadow_cs. */
static void ke_shadow_erase_locked(uint32_t i)
{
    uint32_t mask = g_ke_shadow_cap - 1u, j = i;
    for (;;) {
        uint32_t k;
        j = (j + 1) & mask;
        if (!g_ke_shadow[j].in_use)
            break;
        k = ke_shadow_home(g_ke_shadow[j].guest_va, g_ke_shadow_cap);
        /* Move j back to i unless its home lies cyclically in (i, j]. */
        if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
            continue;
        g_ke_shadow[i] = g_ke_shadow[j];
        i = j;
    }
    memset(&g_ke_shadow[i], 0, sizeof g_ke_shadow[i]);
    g_ke_shadow_count--;
}

static int ke_shadow_stale_locked(uint32_t i)
{
    KE_SHADOW_SLOT *sl = &g_ke_shadow[i];
    return sl->hdr && ke_shadow_hdr(sl->guest_va) != sl->hdr;
}

/* Drop the shadows whose object is gone. Returns how many. */
static uint32_t ke_shadow_sweep_locked(void)
{
    uint32_t i = 0, n = 0;
    while (i < g_ke_shadow_cap) {
        if (g_ke_shadow[i].in_use && ke_shadow_stale_locked(i)) {
            if (g_ke_shadow[i].host_handle)
                CloseHandle(g_ke_shadow[i].host_handle);
            ke_shadow_erase_locked(i);  /* may pull a later entry into i */
            n++;
            continue;
        }
        i++;
    }
    return n;
}

/* Room for one more entry. 0 only when out of memory. */
static int ke_shadow_reserve_locked(void)
{
    KE_SHADOW_SLOT *grown, *old = g_ke_shadow;
    uint32_t cap, i, old_cap = g_ke_shadow_cap;

    if (old_cap && (g_ke_shadow_count + 1u) * 2u <= old_cap)
        return 1;
    if (g_ke_shadow_count >= KE_SHADOW_SWEEP) {
        uint32_t freed = ke_shadow_sweep_locked();
        static int said;
        if (freed && said++ < 4)
            fprintf(stderr, "  [KERNEL] Ke shadow table: %u stale object(s) dropped, %u live\n",
                    freed, g_ke_shadow_count);
        if ((g_ke_shadow_count + 1u) * 2u <= old_cap)
            return 1;
    }
    cap = old_cap ? old_cap * 2u : 8192u;
    grown = (KE_SHADOW_SLOT *)calloc(cap, sizeof *grown);
    if (!grown)
        return 0;
    for (i = 0; i < old_cap; i++) {
        uint32_t j;
        if (!old[i].in_use)
            continue;
        for (j = ke_shadow_home(old[i].guest_va, cap); grown[j].in_use; j = (j + 1) & (cap - 1u))
            ;
        grown[j] = old[i];
    }
    g_ke_shadow = grown;
    g_ke_shadow_cap = cap;
    free(old);
    return 1;
}

/* Add va -> host (absent from the table). Returns 0 when out of memory;
 * the host object is then the caller's. Under g_ke_shadow_cs. */
static int ke_shadow_add_locked(uint32_t va, HANDLE host, uint32_t in_use)
{
    uint32_t j;
    if (!ke_shadow_reserve_locked())
        return 0;
    for (j = ke_shadow_home(va, g_ke_shadow_cap); g_ke_shadow[j].in_use;
         j = (j + 1) & (g_ke_shadow_cap - 1u))
        ;
    g_ke_shadow[j].guest_va = va;
    g_ke_shadow[j].host_handle = host;
    g_ke_shadow[j].in_use = in_use;
    g_ke_shadow[j].hdr = ke_shadow_hdr(va);
    g_ke_shadow_count++;
    return 1;
}

/* Insert a mapping guest_va -> host HANDLE. Replaces any prior mapping for
 * the same guest_va (closing the old handle and inserting the new one).
 * The shadow is never swept until ke_shadow_note_header, which the
 * KeInitialize* bridges call once they have written the guest header. */
static void ke_shadow_insert(uint32_t guest_va, HANDLE host)
{
    int i;

    if (!guest_va || !host)
        return;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    i = ke_shadow_find_locked(guest_va);
    if (i >= 0) {
        if (g_ke_shadow[i].host_handle && g_ke_shadow[i].host_handle != host)
            CloseHandle(g_ke_shadow[i].host_handle);
        g_ke_shadow[i].host_handle = host;
        g_ke_shadow[i].in_use = 1;
        g_ke_shadow[i].hdr = 0;                 /* until ke_shadow_note_header */
        LeaveCriticalSection(&g_ke_shadow_cs);
        return;
    }
    if (ke_shadow_add_locked(guest_va, host, 1)) {
        g_ke_shadow[ke_shadow_find_locked(guest_va)].hdr = 0;
        LeaveCriticalSection(&g_ke_shadow_cs);
        return;
    }

    /* Out of memory -- log and drop the new object. The caller's subsequent
     * waits will fall back to XBOX_TO_NATIVE and fail gracefully. */
    fprintf(stderr, "  [KERNEL] Ke shadow table: out of memory; dropping VA 0x%08X\n",
            guest_va);
    LeaveCriticalSection(&g_ke_shadow_cs);
    CloseHandle(host);
}

/* The header of guest_va is written: it is what the shadow was made for. */
static void ke_shadow_note_header(uint32_t guest_va)
{
    int i;
    ke_shadow_init();
    EnterCriticalSection(&g_ke_shadow_cs);
    i = ke_shadow_find_locked(guest_va);
    if (i >= 0)
        g_ke_shadow[i].hdr = ke_shadow_hdr(guest_va);
    LeaveCriticalSection(&g_ke_shadow_cs);
}

/* Look up the host HANDLE for a guest object VA. Returns NULL if not found. */
static HANDLE ke_shadow_lookup(uint32_t guest_va)
{
    HANDLE h = NULL;
    int i;

    if (!guest_va)
        return NULL;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    i = ke_shadow_find_locked(guest_va);
    if (i >= 0)
        h = g_ke_shadow[i].host_handle;
    LeaveCriticalSection(&g_ke_shadow_cs);
    return h;
}

/* Remove (and close) the mapping for a guest object VA. */
static void ke_shadow_remove(uint32_t guest_va)
{
    int i;

    if (!guest_va)
        return;

    ke_shadow_init();

    EnterCriticalSection(&g_ke_shadow_cs);
    i = ke_shadow_find_locked(guest_va);
    if (i >= 0) {
        if (g_ke_shadow[i].host_handle)
            CloseHandle(g_ke_shadow[i].host_handle);
        ke_shadow_erase_locked((uint32_t)i);
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
}

/* Events a title initialises itself (on by default, RECOMP_TITLE_KEVENTS=0 turns it off).
 *
 * Shadows are made by KeInitializeEvent, but XDK code can build a KEVENT by
 * writing its header directly: one title (T()NY) does not even import
 * KeInitializeEvent, and its D3D waits on the event at miniport + 0x1A4, which
 * the graphics interrupt sets for software method 5. With no shadow, the guest
 * address itself went to Win32 as a HANDLE: SetEvent failed silently, and every
 * wait failed at once with STATUS_UNSUCCESSFUL, which BlockOnTime's
 * "while (KeWaitForSingleObject(...))" retried forever -- the menu froze.
 *
 * So on first use, an object whose header reads as an event (Type 0 notification
 * or 1 synchronization, Size 4 dwords) gets a host event with the same type and
 * state, marked in_use = 2. For those the bridges keep SignalState in guest
 * memory up to date, because XDK code resets it by writing 0 there.
 * Returns NULL for anything else (the old paths apply).
 *
 * On by default (K-5): XDK libraries build most of their events this way
 * (KOTOR, Black and Burnout 3 do not import KeInitializeEvent at all: XAPI,
 * XPP and D3D write the headers). With guest events off, KOTOR's synchronous
 * USB request saw its wait fail at once, returned, and the URB on its stack
 * was later completed into garbage ([ICALL] Failed to resolve VA 0x003067DF,
 * inside LIBCMT free) -- one failed wait, which the wait-spin promotion below
 * cannot catch. The cost: a wait that failed at once now blocks until the
 * event is set, so on a title whose setter the runtime does not model yet (a
 * GPU interrupt, say) a spin turns into a hang. RECOMP_TITLE_KEVENTS=0 brings
 * the old behaviour back (the guest address goes to the host as a HANDLE, so
 * the wait fails); the wait-spin promotion and xbox_KeAdoptGuestEvent still
 * work then. */
/* Objects promoted one by one by the wait-spin detector (XBA-93, see
 * bridge_wait_spin): the RECOMP_TITLE_KEVENTS behaviour for an event a thread
 * keeps failing to wait on, without turning it on for every object. */
#define KE_PROMOTED_MAX 64
static volatile uint32_t g_ke_promoted[KE_PROMOTED_MAX];
static volatile LONG g_ke_promoted_n;

static int ke_is_promoted(uint32_t guest_va)
{
    LONG i, n = g_ke_promoted_n;
    for (i = 0; i < n && i < KE_PROMOTED_MAX; i++)
        if (g_ke_promoted[i] == guest_va)
            return 1;
    return 0;
}

/* Objects something tried to set while they were not host events yet
 * (KeSetEvent on a title-built header, a software method 5): the evidence a
 * spinning wait on one of them will be woken once it blocks (XBA-93). */
static volatile uint32_t g_ke_set_seen[KE_PROMOTED_MAX];
static volatile LONG g_ke_set_seen_n;

static void ke_note_set_attempt(uint32_t guest_va)
{
    LONG i, n = g_ke_set_seen_n;
    for (i = 0; i < n && i < KE_PROMOTED_MAX; i++)
        if (g_ke_set_seen[i] == guest_va)
            return;
    i = InterlockedIncrement(&g_ke_set_seen_n) - 1;
    if (i < KE_PROMOTED_MAX)
        g_ke_set_seen[i] = guest_va;
}

static int ke_set_seen(uint32_t guest_va)
{
    LONG i, n = g_ke_set_seen_n;
    for (i = 0; i < n && i < KE_PROMOTED_MAX; i++)
        if (g_ke_set_seen[i] == guest_va)
            return 1;
    return 0;
}

static int ke_promote(uint32_t guest_va)
{
    LONG i;
    if (ke_is_promoted(guest_va))
        return 1;
    i = InterlockedIncrement(&g_ke_promoted_n) - 1;
    if (i >= KE_PROMOTED_MAX)
        return 0;
    g_ke_promoted[i] = guest_va;
    return 1;
}

static int ke_title_kevents_on(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_TITLE_KEVENTS");
        enabled = !(e && *e == '0');    /* on unless turned off */
    }
    return enabled;
}

static HANDLE ke_guest_event(uint32_t guest_va, int *type)
{
    uint32_t hdr;
    HANDLE h = NULL;

    if (!ke_title_kevents_on() && !ke_is_promoted(guest_va))
        return NULL;
    if (guest_va < 0x10000u || (guest_va & 3u))
        return NULL;
    if (guest_va >= 0x04000000u && (guest_va < 0x80000000u || guest_va >= 0x84000000u))
        return NULL;                               /* RAM or the contiguous window */
    hdr = BRIDGE_MEM32(guest_va);
    if (((hdr & 0xFFu) != 0 && (hdr & 0xFFu) != 1) || ((hdr >> 16) & 0xFFu) != 4)
        return NULL;
    *type = (int)(hdr & 0xFFu);

    ke_shadow_init();
    EnterCriticalSection(&g_ke_shadow_cs);
    {
        int k = ke_shadow_find_locked(guest_va);
        if (k >= 0 && g_ke_shadow[k].in_use != 2) {
            LeaveCriticalSection(&g_ke_shadow_cs);
            return NULL;                           /* a KeInitializeEvent shadow: old path */
        }
        if (k >= 0 && !g_ke_shadow[k].hdr)            /* restored by a save state */
            g_ke_shadow[k].hdr = ke_shadow_hdr(guest_va);
        if (k >= 0 && (g_ke_shadow[k].hdr & 0xFFu) == (uint32_t)*type) {
            h = g_ke_shadow[k].host_handle;
            LeaveCriticalSection(&g_ke_shadow_cs);
            return h;
        }
        if (k >= 0) {
            /* The address now holds an event of the other type (its
             * memory was reused): a host event of the right reset kind,
             * from the guest's state. */
            if (g_ke_shadow[k].host_handle)
                CloseHandle(g_ke_shadow[k].host_handle);
            ke_shadow_erase_locked((uint32_t)k);
        }
    }
    h = CreateEventA(NULL, *type == 0, BRIDGE_MEM32(guest_va + 4) != 0, NULL);
    if (h && !ke_shadow_add_locked(guest_va, h, 2)) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [KERNEL] Ke shadow table: out of memory; title-built event "
                            "0x%08X left without a host event\n", guest_va);
        CloseHandle(h);
        h = NULL;
    }
    LeaveCriticalSection(&g_ke_shadow_cs);
    if (h) {
        static int shown;
        if (shown++ < 16)
            fprintf(stderr, "  [KERNEL] event 0x%08X built by the title: host event created "
                            "(%s)\n", guest_va, *type ? "synchronization" : "notification");
    }
    return h;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Exports (102, 120, 154, 240, 245, 249)
 *
 * These ordinals are DATA exports, not functions. The bridge returns a guest
 * VA inside the kernel data page so the title can dereference it. The offsets
 * below (0x510-0x558) are free in xbox_memory_layout.h's KDATA block (existing
 * slots end at 0x4B0+64, KiBugCheckData uses 0x500, page is 4 KB). Prefer
 * moving them into xbox_memory_layout.h and dropping the guarded defines here.
 * ═══════════════════════════════════════════════════════════════════════════
 */
#ifndef KDATA_MMGLOBAL
#define KDATA_MMGLOBAL          0x510  /* MmGlobalData (4 bytes) */
#endif
#ifndef KDATA_INTERRUPT_TIME
#define KDATA_INTERRUPT_TIME    0x520  /* KeInterruptTime (LARGE_INTEGER, 8 bytes) */
#endif
#ifndef KDATA_SYSTEM_TIME
#define KDATA_SYSTEM_TIME       0x530  /* KeSystemTime (LARGE_INTEGER, 8 bytes) */
#endif
#ifndef KDATA_OBJ_DIR_TYPE
#define KDATA_OBJ_DIR_TYPE      0x540  /* ObDirectoryObjectType (4 bytes) */
#endif
#ifndef KDATA_OBJ_HANDLE_TABLE
#define KDATA_OBJ_HANDLE_TABLE  0x548  /* ObpObjectHandleTable (4 bytes) */
#endif
#ifndef KDATA_OBJ_SYM_LINK_TYPE
#define KDATA_OBJ_SYM_LINK_TYPE 0x550  /* ObSymbolicLinkObjectType (4 bytes) */
#endif

/* --- MmGlobalData (ordinal 102, 0 args) --- */
static void bridge_MmGlobalData(void)
{
    /* Titles rarely dereference this; give it a stable, mapped address that
     * reads back the address itself (self-referential placeholder). */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL) =
        XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_MMGLOBAL;
}

/* --- KeInterruptTime (ordinal 120, 0 args) --- */
static void bridge_KeInterruptTime(void)
{
    /* Refresh the 100ns-since-boot counter before handing out the pointer. */
    ULONGLONG t = (ULONGLONG)xbox_GuestTickCount64() * 10000ull;   /* XBA-240 */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME + 0) = (uint32_t)t;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME + 4) = (uint32_t)(t >> 32);
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_INTERRUPT_TIME;
}

/* --- KeSystemTime (ordinal 154, 0 args) --- */
static void bridge_KeSystemTime(void)
{
    /* Refresh the 100ns-since-1601 clock before handing out the pointer. */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    if (xbox_GuestClockActive()) {   /* fast-forward's gain (XBA-240) */
        ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime)
                      + xbox_GuestClockExtraUnits(10000000u);
        ft.dwLowDateTime = (DWORD)t;
        ft.dwHighDateTime = (DWORD)(t >> 32);
    }
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME + 0) = ft.dwLowDateTime;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME + 4) = ft.dwHighDateTime;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_SYSTEM_TIME;
}

/* --- ObDirectoryObjectType (ordinal 240, 0 args) --- */
static void bridge_ObDirectoryObjectType(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_DIR_TYPE;
}

/* --- ObpObjectHandleTable (ordinal 245, 0 args) --- */
static void bridge_ObpObjectHandleTable(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_HANDLE_TABLE;
}

/* --- ObSymbolicLinkObjectType (ordinal 249, 0 args) --- */
static void bridge_ObSymbolicLinkObjectType(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE) =
        XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE;
    g_eax = XBOX_KERNEL_DATA_BASE + KDATA_OBJ_SYM_LINK_TYPE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Dbg (6, 7, 8, 10, 11)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- DbgBreakPointWithStatus (ordinal 6, 1 arg = 4 bytes) --- */
static void bridge_DbgBreakPointWithStatus(void)
{
    (void)STACK_ARG(0);
    DebugBreak();
    g_eax = 0;
}

/* --- DbgLoadImageSymbols (ordinal 7, 3 args = 12 bytes) --- */
static void bridge_DbgLoadImageSymbols(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* --- DbgPrint (ordinal 8, __cdecl varargs, 0 fixed args) --- */
static void bridge_DbgPrint(void)
{
    /* First vararg is a guest format string. Log it verbatim (never treat it
     * as a format, it is guest-owned); caller cleans the stack (__cdecl). */
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(0));
    if (fmt && KERNEL_LOG_ON()) {
        size_t i, n = 0;
        for (i = 0; i < 512 && fmt[i]; i++)
            n++;
        fwrite(fmt, 1, n, stderr);
        fputc('\n', stderr);
        fflush(stderr);
    }
    g_eax = 0;
}

/* --- DbgPrompt (ordinal 10, 2 args = 8 bytes) --- */
static void bridge_DbgPrompt(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- DbgUnLoadImageSymbols (ordinal 11, 3 args = 12 bytes) --- */
static void bridge_DbgUnLoadImageSymbols(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ex (12, 13, 18, 19, 20, 21, 25, 26, 27, 28)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ExAcquireReadWriteLockExclusive (ordinal 12, 1 arg = 4 bytes) --- */
static void bridge_ExAcquireReadWriteLockExclusive(void)
{
    ks_rwlock_acquire_exclusive(STACK_ARG(0));   /* kernel_ksync.c */
    g_eax = 0;
}

/* --- ExAcquireReadWriteLockShared (ordinal 13, 1 arg = 4 bytes) --- */
static void bridge_ExAcquireReadWriteLockShared(void)
{
    ks_rwlock_acquire_shared(STACK_ARG(0));
    g_eax = 0;
}

/* --- ExInitializeReadWriteLock (ordinal 18, 1 arg = 4 bytes) --- */
static void bridge_ExInitializeReadWriteLock(void)
{
    ks_rwlock_init(STACK_ARG(0));   /* the 0x34-byte ERWLOCK, LockCount -1 */
    g_eax = 0;
}

/* --- ExInterlockedAddLargeInteger (ordinal 19, 3 args = 12 bytes)
 * LARGE_INTEGER ExInterlockedAddLargeInteger(PLARGE_INTEGER Addend,
 *                                            LARGE_INTEGER Increment);
 * Addend (4) + Increment.Low (4) + Increment.High (4). Returns the PREVIOUS
 * value of Addend in edx:eax. */
static void bridge_ExInterlockedAddLargeInteger(void)
{
    uint32_t addend_va = STACK_ARG(0);
    LONGLONG increment = ((LONGLONG)(uint32_t)STACK_ARG(2) << 32)
                       | (LONGLONG)(uint32_t)STACK_ARG(1);
    volatile LONG *p = (volatile LONG *)XBOX_TO_NATIVE(addend_va);
    LONGLONG old = 0;

    if (p) {
        if (((uintptr_t)p & 7) == 0) {
            volatile LONGLONG *qp = (volatile LONGLONG *)p;
            LONGLONG cmp = *qp;
            for (;;) {
                LONGLONG desired = cmp + increment;
                LONGLONG actual = InterlockedCompareExchange64(qp, desired, cmp);
                if (actual == cmp)
                    break;
                cmp = actual;
            }
            old = cmp;
        } else {
            /* Unaligned: single-threaded guest, plain RMW is good enough. */
            old = ((LONGLONG)(uint32_t)p[1] << 32) | (uint32_t)p[0];
            {
                LONGLONG newv = old + increment;
                p[0] = (LONG)(uint32_t)newv;
                p[1] = (LONG)(uint32_t)((uint64_t)newv >> 32);
            }
        }
    }

    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* --- ExInterlockedAddLargeStatistic (ordinal 20, 2 args = 8 bytes)
 * LARGE_INTEGER ExInterlockedAddLargeStatistic(PLARGE_INTEGER Addend,
 *                                              ULONG Increment);
 * Addend (4) + Increment (4). Atomic add; per task spec, returns 0. */
static void bridge_ExInterlockedAddLargeStatistic(void)
{
    uint32_t addend_va = STACK_ARG(0);
    LONG increment = (LONG)STACK_ARG(1);
    volatile LONG *p = (volatile LONG *)XBOX_TO_NATIVE(addend_va);

    if (p) {
        if (((uintptr_t)p & 7) == 0) {
            volatile LONGLONG *qp = (volatile LONGLONG *)p;
            LONGLONG cmp = *qp;
            for (;;) {
                LONGLONG desired = cmp + (LONGLONG)increment;
                LONGLONG actual = InterlockedCompareExchange64(qp, desired, cmp);
                if (actual == cmp)
                    break;
                cmp = actual;
            }
        } else {
            LONGLONG val = ((LONGLONG)(uint32_t)p[1] << 32) | (uint32_t)p[0];
            LONGLONG newv = val + (LONGLONG)increment;
            p[0] = (LONG)(uint32_t)newv;
            p[1] = (LONG)(uint32_t)((uint64_t)newv >> 32);
        }
    }
    g_eax = 0;
}

/* --- ExInterlockedCompareExchange64 (ordinal 21, 4 args = 16 bytes)
 * LONGLONG ExInterlockedCompareExchange64(PLONGLONG Destination,
 *                                         LONGLONG Exchange,
 *                                         PLONGLONG Comparand);
 * Arg layout on a 32-bit stack is Destination (4) + Exchange (8) +
 * Comparand.Low (4) -- the Comparand high dword is not passed, so the
 * comparison is against a zero-extended 32-bit value. Returns the old value
 * in edx:eax. */
static void bridge_ExInterlockedCompareExchange64(void)
{
    LONGLONG old = 0;
    volatile LONGLONG *qp;
    LONGLONG exchange;
    LONGLONG comparand;

    exchange  = ((LONGLONG)(uint32_t)STACK_ARG(2) << 32)
              | (LONGLONG)(uint32_t)STACK_ARG(1);
    comparand = (LONGLONG)(uint32_t)STACK_ARG(3);
    qp = (volatile LONGLONG *)XBOX_TO_NATIVE(STACK_ARG(0));

    if (qp)
        old = InterlockedCompareExchange64(qp, exchange, comparand);

    g_eax = (uint32_t)old;
    g_edx = (uint32_t)((uint64_t)old >> 32);
}

/* --- ExReadWriteRefurbInfo (ordinal 25, 3 args = 12 bytes)
 * NTSTATUS ExReadWriteRefurbInfo(XBOX_REFURB_INFO *RefurbInfo,
 *                                ULONG ValueLength, BOOLEAN DoWrite);
 * The console keeps XBOX_REFURB_INFO { Signature, PowerCycleCount,
 * FirstSetTime } (16 bytes) in a sector of the hard disk; here it lives in
 * memory for the run: a write is read back, a first read is all zeros
 * (XBA-114). Another length is STATUS_INVALID_PARAMETER. */
static uint8_t s_refurb_info[16];

static void bridge_ExReadWriteRefurbInfo(void)
{
    uint32_t va = STACK_ARG(0);

    if (STACK_ARG(1) != sizeof s_refurb_info) {
        g_eax = 0xC000000Du;
        return;
    }
    if (!bridge_buf_ok(va, sizeof s_refurb_info, "ExReadWriteRefurbInfo")) {
        g_eax = 0xC0000005u;
        return;
    }
    if (STACK_ARG(2) & 0xFF)
        memcpy(s_refurb_info, XBOX_TO_NATIVE(va), sizeof s_refurb_info);
    else
        memcpy(XBOX_TO_NATIVE(va), s_refurb_info, sizeof s_refurb_info);
    g_eax = 0;
}

/* --- ExRaiseException (ordinal 26, 1 arg = 4 bytes) ---
 * The kernel-mode RtlRaiseException; the same dispatch. */
static void bridge_ExRaiseException(void)
{
    bridge_raise_record("ExRaiseException", STACK_ARG(0));
}

/* --- ExRaiseStatus (ordinal 27, 1 arg = 4 bytes) ---
 * Raises a noncontinuable exception with no parameters for the status. */
static void bridge_ExRaiseStatus(void)
{
    uint32_t status = STACK_ARG(0);

    /* Noncontinuable: a handler can only resume elsewhere (which does not
     * return here), so either way the call returns as it used to. */
    (void)xbox_RaiseGuestException(status, XBOX_EXCEPTION_NONCONTINUABLE,
                                   g_xbox_kernel_caller, 0, NULL);
    g_eax = 0;
}

/* --- ExReleaseReadWriteLock (ordinal 28, 1 arg = 4 bytes) --- */
static void bridge_ExReleaseReadWriteLock(void)
{
    ks_rwlock_release(STACK_ARG(0));
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ExfInterlocked linked-list operations (32, 33, 34)
 *
 * The Xbox ABI keeps a spin-lock around these; with a single-threaded guest
 * the two-pointer fix-up after the atomic head swap is sufficient.
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- ExfInterlockedInsertHeadList (ordinal 32, 2 args = 8 bytes)
 * PLIST_ENTRY ExfInterlockedInsertHeadList(ListHead, Entry);
 * Returns the entry's old Flink (the previous first entry, or ListHead
 * itself for an empty list). */
static void bridge_ExfInterlockedInsertHeadList(void)
{
    uint32_t entry_va = STACK_ARG(0);
    uint32_t head_va  = STACK_ARG(1);
    volatile LONG *h_flink = (volatile LONG *)XBOX_TO_NATIVE(head_va);
    uint32_t old_flink = 0;

    if (h_flink) {
        /* Atomically seat the new head; remember what used to be first. */
        old_flink = (uint32_t)InterlockedExchange(h_flink, (LONG)entry_va);
        BRIDGE_MEM32(entry_va + 0) = old_flink;  /* Entry->Flink = old first */
        BRIDGE_MEM32(entry_va + 4) = head_va;    /* Entry->Blink = head       */
        if (old_flink)
            BRIDGE_MEM32(old_flink + 4) = entry_va; /* old first->Blink = Entry */
    }
    g_eax = old_flink;
}

/* --- ExfInterlockedInsertTailList (ordinal 33, 2 args = 8 bytes)
 * PLIST_ENTRY ExfInterlockedInsertTailList(ListHead, Entry);
 * Returns the previous tail. NT semantics return the old Blink (the entry
 * that was last, or ListHead for an empty list); note the task brief said
 * "old Flink" but the canonical Xbox/NT behaviour is the old Blink. */
static void bridge_ExfInterlockedInsertTailList(void)
{
    uint32_t entry_va = STACK_ARG(0);
    uint32_t head_va  = STACK_ARG(1);
    volatile LONG *h_blink = (volatile LONG *)XBOX_TO_NATIVE(head_va + 4);
    uint32_t old_blink = 0;

    if (h_blink) {
        /* Atomically seat the new tail; remember what used to be last. */
        old_blink = (uint32_t)InterlockedExchange(h_blink, (LONG)entry_va);
        BRIDGE_MEM32(entry_va + 0) = head_va;     /* Entry->Flink = head      */
        BRIDGE_MEM32(entry_va + 4) = old_blink;   /* Entry->Blink = old last  */
        if (old_blink)
            BRIDGE_MEM32(old_blink + 0) = entry_va; /* old last->Flink = Entry */
    }
    g_eax = old_blink;
}

/* --- ExfInterlockedRemoveHeadList (ordinal 34, 1 arg = 4 bytes)
 * PLIST_ENTRY ExfInterlockedRemoveHeadList(ListHead);
 * Returns the removed first entry, or 0 when the list is empty. */
static void bridge_ExfInterlockedRemoveHeadList(void)
{
    uint32_t head_va = STACK_ARG(0);
    volatile LONG *h_flink = (volatile LONG *)XBOX_TO_NATIVE(head_va);
    uint32_t removed = 0;

    if (h_flink) {
        for (;;) {
            uint32_t first = (uint32_t)*h_flink;
            if (first == head_va) {   /* empty list */
                removed = 0;
                break;
            }
            /* Pull Entry = first; Entry's Flink becomes the new head, but only
             * if the head still points at first (single-threaded: it does). */
            {
                uint32_t entry_flink = BRIDGE_MEM32(first + 0);
                if (InterlockedCompareExchange(h_flink,
                                              (LONG)entry_flink,
                                              (LONG)first) == (LONG)first) {
                    if (entry_flink)
                        BRIDGE_MEM32(entry_flink + 4) = head_va;
                    removed = first;
                    break;
                }
            }
        }
    }
    g_eax = removed;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Fsc (36)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- FscInvalidateIdleBlocks (ordinal 36, 0 args = 0 bytes) --- */
static void bridge_FscInvalidateIdleBlocks(void)
{
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Hal (43, 365, 366)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- HalEnableSystemInterrupt (ordinal 43, 2 args = 8 bytes) --- */
static void bridge_HalEnableSystemInterrupt(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- HalEnableSecureTrayEject (ordinal 365, 0 args = 0 bytes) --- */
static void bridge_HalEnableSecureTrayEject(void)
{
    g_eax = 0;
}

/* --- HalWriteSMCScratchRegister (ordinal 366, 1 arg = 4 bytes)
 * NTSTATUS HalWriteSMCScratchRegister(ULONG ScratchRegister): the SMC's
 * scratch register (command 0x1B), which a later SMBus read of it returns
 * (kernel_hal.c, XBA-114). */
static void bridge_HalWriteSMCScratchRegister(void)
{
    g_eax = (uint32_t)xbox_HalWriteSMBusValue(SMC_SLAVE_ADDRESS, SMC_CMD_SCRATCH,
                                              FALSE, STACK_ARG(0));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Interlocked (51-58) -- __fastcall: args travel in ecx/edx/eax, never on
 * the guest stack, so the arg-size entries are all 0 for these. The guest VA
 * in ecx/edx must go through XBOX_TO_NATIVE, exactly like the existing
 * bridge_ObfDereferenceObject.
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- InterlockedCompareExchange (ordinal 51)
 * LONG InterlockedCompareExchange(PLONG Destination, LONG Exchange,
 *                                 LONG Comparand);
 * fastcall: ecx=Destination, edx=Exchange, eax=Comparand-in/old-out. */
static void bridge_InterlockedCompareExchange(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedCompareExchange(dst, (LONG)g_edx, (LONG)g_eax);
}

/* --- InterlockedDecrement (ordinal 52)
 * fastcall: ecx=Destination. */
static void bridge_InterlockedDecrement(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedDecrement(dst);
}

/* --- InterlockedIncrement (ordinal 53)
 * fastcall: ecx=Destination. */
static void bridge_InterlockedIncrement(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedIncrement(dst);
}

/* --- InterlockedExchange (ordinal 54)
 * fastcall: ecx=Destination, edx=Value. */
static void bridge_InterlockedExchange(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedExchange(dst, (LONG)g_edx);
}

/* --- InterlockedExchangeAdd (ordinal 55)
 * fastcall: ecx=Destination, edx=Value. */
static void bridge_InterlockedExchangeAdd(void)
{
    LONG *dst = (LONG *)XBOX_TO_NATIVE(g_ecx);
    if (!dst) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)InterlockedExchangeAdd(dst, (LONG)g_edx);
}

/* --- InterlockedFlushSList (ordinal 56, fastcall: ecx=ListHead)
 * SLists are kernel_ksync.c: a real 64-bit CAS on the header (XBA-113). */
static void bridge_InterlockedFlushSList(void)
{
    g_eax = ks_slist_flush(g_ecx);
}

/* --- InterlockedPopEntrySList (ordinal 57, fastcall: ecx=ListHead) --- */
static void bridge_InterlockedPopEntrySList(void)
{
    g_eax = ks_slist_pop(g_ecx);
}

/* --- InterlockedPushEntrySList (ordinal 58, fastcall: ecx=ListHead,
 * edx=ListEntry) --- */
static void bridge_InterlockedPushEntrySList(void)
{
    g_eax = ks_slist_push(g_ecx, g_edx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Io (59, 60, 63, 72, 75, 76, 77, 78, 80, 90, 91)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- IoAllocateIrp (ordinal 59, 1 arg = 4 bytes)
 * PIRP IoAllocateIrp(CCHAR StackSize) -- one argument (nxdk xboxkrnl.h; the
 * NT ChargeQuota is gone, and popping it leaked 4 bytes of the caller's
 * stack). An initialised IRP plus StackSize stack locations from the guest
 * heap (layout above bridge_IoBuildDeviceIoControlRequest); IoFreeIrp
 * (ordinal 72) frees it through the same heap. */
static void bridge_IoAllocateIrp(void)
{
    g_eax = irp_alloc(STACK_ARG(0));
}

/* --- IoBuildAsynchronousFsdRequest (ordinal 60, 6 args = 24 bytes)
 * PIRP IoBuildAsynchronousFsdRequest(MajorFunction, DeviceObject, Buffer,
 * Length, StartingOffset, IoStatusBlock) (nxdk xboxkrnl.h). NT's, from
 * memory: an IRP for DeviceObject->StackSize locations, the caller's thread,
 * UserIosb, and the next stack location (the one IofCallDriver makes
 * current) holding the major function; for a read or write also UserBuffer
 * (the console's IRP has no MDL), Parameters.Read.Length and ByteOffset
 * (StartingOffset, or 0). NULL when the heap refuses or with no device.
 * RECOMP_IRP_DISPATCH=0: NULL always, as before XBA-138. */
static void bridge_IoBuildAsynchronousFsdRequest(void)
{
    uint32_t major = STACK_ARG(0), dev = STACK_ARG(1), buf = STACK_ARG(2);
    uint32_t len = STACK_ARG(3), off = STACK_ARG(4), iosb = STACK_ARG(5);
    uint32_t irp, sl;

    g_eax = 0;
    if (!irp_dispatch_on() || !dev)
        return;
    irp = irp_alloc(BRIDGE_MEM8(dev + DEVOBJ_O_STACKSIZE));
    if (!irp)
        return;
    BRIDGE_MEM32(irp + IRP_O_THREAD) = thobj_current();
    BRIDGE_MEM32(irp + IRP_O_USERIOSB) = iosb;
    sl = BRIDGE_MEM32(irp + IRP_O_CURSTACK) - IRP_SL_SIZE;
    BRIDGE_MEM8(sl + SL_O_MAJOR) = (uint8_t)major;
    if (major == IRP_MJ_READ_ || major == IRP_MJ_WRITE_) {
        BRIDGE_MEM32(irp + IRP_O_USERBUFFER) = buf;
        BRIDGE_MEM32(sl + SL_O_LENGTH) = len;
        BRIDGE_MEM32(sl + SL_O_BYTEOFFSET) = off ? BRIDGE_MEM32(off) : 0;
        BRIDGE_MEM32(sl + SL_O_BYTEOFFSET + 4) = off ? BRIDGE_MEM32(off + 4) : 0;
    }
    g_eax = irp;
}

/* ── Share access (XBA-114) ──
 *
 * NT's rules, on the console's structures (nxdk xboxkrnl.h): a FILE_OBJECT
 * carries the access it was opened for and what it shares as bits in its
 * byte at +2 (DeletePending 0, ReadAccess 1, WriteAccess 2, DeleteAccess 3,
 * SharedRead 4, SharedWrite 5, SharedDelete 6), and a SHARE_ACCESS counts
 * opens as seven UCHARs (OpenCount, Readers, Writers, Deleters, SharedRead,
 * SharedWrite, SharedDelete). Read access is FILE_READ_DATA or
 * FILE_EXECUTE, write FILE_WRITE_DATA or FILE_APPEND_DATA, delete DELETE;
 * the share bits are FILE_SHARE_READ / WRITE / DELETE. A title's own file
 * system driver keeps these; the host files of NtCreateFile do not. */
static uint32_t share_bits(uint32_t access, uint32_t share)
{
    uint32_t r = (access & 0x21u) != 0, w = (access & 0x06u) != 0, d = (access & 0x10000u) != 0;
    uint32_t bits = (r << 1) | (w << 2) | (d << 3);
    if (r || w || d)
        bits |= ((share & 1u) << 4) | ((share & 2u) << 4) | ((share & 4u) << 4);
    return bits;
}

/* The FILE_OBJECT's byte at +2 takes `bits` for its access and share bits. */
static void share_set_fo(uint32_t fo, uint32_t bits)
{
    BRIDGE_MEM8(fo + 2) = (uint8_t)((BRIDGE_MEM8(fo + 2) & 0x81u) | bits);
}

/* --- IoCheckShareAccess (ordinal 63, 5 args = 20 bytes)
 * NTSTATUS IoCheckShareAccess(ACCESS_MASK DesiredAccess,
 *                             ULONG DesiredShareAccess,
 *                             PFILE_OBJECT FileObject,
 *                             PSHARE_ACCESS ShareAccess, BOOLEAN Update);
 * STATUS_SHARING_VIOLATION when the open would conflict with the opens
 * counted; otherwise the file object takes its bits and, with Update, the
 * counts take the open. */
static void bridge_IoCheckShareAccess(void)
{
    uint32_t bits = share_bits(STACK_ARG(0), STACK_ARG(1));
    uint32_t fo = STACK_ARG(2), sa = STACK_ARG(3), update = STACK_ARG(4) & 0xFF;
    uint32_t r = (bits >> 1) & 1, w = (bits >> 2) & 1, d = (bits >> 3) & 1;
    uint32_t sr = (bits >> 4) & 1, sw = (bits >> 5) & 1, sd = (bits >> 6) & 1;
    uint8_t open, readers, writers, deleters, shr, shw, shd;

    if (!bridge_buf_ok(fo, 4, "IoCheckShareAccess") || !bridge_buf_ok(sa, 7, "IoCheckShareAccess")) {
        g_eax = 0xC0000005u;
        return;
    }
    share_set_fo(fo, bits);
    if (!(r || w || d)) {
        g_eax = 0;
        return;
    }
    open = BRIDGE_MEM8(sa + 0); readers = BRIDGE_MEM8(sa + 1); writers = BRIDGE_MEM8(sa + 2);
    deleters = BRIDGE_MEM8(sa + 3); shr = BRIDGE_MEM8(sa + 4); shw = BRIDGE_MEM8(sa + 5);
    shd = BRIDGE_MEM8(sa + 6);
    if ((r && shr < open) || (w && shw < open) || (d && shd < open)
            || (readers && !sr) || (writers && !sw) || (deleters && !sd)) {
        g_eax = 0xC0000043u;                 /* STATUS_SHARING_VIOLATION */
        return;
    }
    if (update) {
        BRIDGE_MEM8(sa + 0) = (uint8_t)(open + 1);
        BRIDGE_MEM8(sa + 1) = (uint8_t)(readers + r);
        BRIDGE_MEM8(sa + 2) = (uint8_t)(writers + w);
        BRIDGE_MEM8(sa + 3) = (uint8_t)(deleters + d);
        BRIDGE_MEM8(sa + 4) = (uint8_t)(shr + sr);
        BRIDGE_MEM8(sa + 5) = (uint8_t)(shw + sw);
        BRIDGE_MEM8(sa + 6) = (uint8_t)(shd + sd);
    }
    g_eax = 0;
}

/* --- IoFreeIrp (ordinal 72, 1 arg = 4 bytes) --- */
static void bridge_IoFreeIrp(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* --- IoQueryFileInformation (ordinal 75, 5 args = 20 bytes) --- */
static void bridge_IoQueryFileInformation(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0xC000000D;  /* STATUS_INVALID_PARAMETER */
}

/* --- IoQueryVolumeInformation (ordinal 76, 5 args = 20 bytes) --- */
static void bridge_IoQueryVolumeInformation(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    (void)STACK_ARG(3);
    (void)STACK_ARG(4);
    g_eax = 0xC000000D;  /* STATUS_INVALID_PARAMETER */
}

/* --- IoQueueThreadIrp (ordinal 77, 1 arg = 4 bytes) --- */
static void bridge_IoQueueThreadIrp(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- IoRemoveShareAccess (ordinal 78, 2 args = 8 bytes)
 * VOID IoRemoveShareAccess(PFILE_OBJECT FileObject, PSHARE_ACCESS):
 * takes the file object's open out of the counts. */
static void bridge_IoRemoveShareAccess(void)
{
    uint32_t fo = STACK_ARG(0), sa = STACK_ARG(1), b;
    int k;

    g_eax = 0;
    if (!bridge_buf_ok(fo, 4, "IoRemoveShareAccess") || !bridge_buf_ok(sa, 7, "IoRemoveShareAccess"))
        return;
    b = BRIDGE_MEM8(fo + 2);
    if (!(b & 0x0Eu))
        return;
    BRIDGE_MEM8(sa + 0) = (uint8_t)(BRIDGE_MEM8(sa + 0) - 1);
    for (k = 1; k <= 6; k++)                 /* Readers..SharedDelete <- bits 1..6 */
        BRIDGE_MEM8(sa + (uint32_t)k) = (uint8_t)(BRIDGE_MEM8(sa + (uint32_t)k) - ((b >> k) & 1u));
}

/* --- IoSetShareAccess (ordinal 80, 4 args = 16 bytes)
 * VOID IoSetShareAccess(ACCESS_MASK DesiredAccess, ULONG DesiredShareAccess,
 *                       PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess);
 * Four arguments (nxdk xboxkrnl.h); the table said five. The first open:
 * the counts become this open alone (or none, for an open without data
 * access). */
static void bridge_IoSetShareAccess(void)
{
    uint32_t bits = share_bits(STACK_ARG(0), STACK_ARG(1));
    uint32_t fo = STACK_ARG(2), sa = STACK_ARG(3);
    int k, any = (bits & 0x0Eu) != 0;

    g_eax = 0;
    if (!bridge_buf_ok(fo, 4, "IoSetShareAccess") || !bridge_buf_ok(sa, 7, "IoSetShareAccess"))
        return;
    share_set_fo(fo, bits);
    BRIDGE_MEM8(sa + 0) = (uint8_t)any;
    for (k = 1; k <= 6; k++)
        BRIDGE_MEM8(sa + (uint32_t)k) = (uint8_t)(any ? (bits >> k) & 1u : 0u);
}

/* --- IoDismountVolume (ordinal 90, 1 arg = 4 bytes) --- */
static void bridge_IoDismountVolume(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* --- IoDismountVolumeByName (ordinal 91, 1 arg = 4 bytes) --- */
static void bridge_IoDismountVolumeByName(void)
{
    (void)STACK_ARG(0);
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Kd (88, 89)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* KdDebuggerEnabled and KdDebuggerNotPresent (88, 89) are data exports:
 * see kernel_data_va_for_ordinal. */

/* ═══════════════════════════════════════════════════════════════════════════
 * Ke -- shadow-table object bridges (92-163)
 *
 * The KeInitialize* family creates a real Win32 object, records guest-VA ->
 * host-HANDLE in the shadow table, and zeroes/initialises the guest dispatcher
 * header. The KePulse/Release/Reset/Set family resolves back through the
 * shadow table (with XBOX_TO_NATIVE as a fallback for legacy callers).
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- KeInitializeEvent (ordinal 108, 3 args = 12 bytes) --- */
static void bridge_KeInitializeEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t event_type = STACK_ARG(1); /* 0=Notification(manual-reset), 1=Synchronization(auto-reset) */
    uint32_t initial_state = STACK_ARG(2);

    /* The header the console's KeInitializeEvent writes (KRN-10): Size 4
     * (in LONGs), Inserted 0, a self-linked wait list, and SignalState as
     * the event's state from then on. The shadow below wrote Size 16, so
     * ke_guest_event (which wants 4) never treated the event as
     * header-owned and its SignalState was written once and never again:
     * an inline KeReadStateEvent read the initial state for ever, and XDK
     * code clearing the event by storing 0 there left the host event set.
     * With guest events on (the default) the event is now made exactly
     * like one XDK code builds inline; RECOMP_TITLE_KEVENTS=0 keeps the
     * shadow, as does an address guest events do not cover. */
    if (ke_title_kevents_on()) {
        int type;
        BRIDGE_MEM8(guest_va + 0)  = (uint8_t)(event_type ? 1 : 0);
        BRIDGE_MEM8(guest_va + 1)  = 0;
        BRIDGE_MEM8(guest_va + 2)  = 4;
        BRIDGE_MEM8(guest_va + 3)  = 0;
        BRIDGE_MEM32(guest_va + 4) = initial_state ? 1 : 0;
        BRIDGE_MEM32(guest_va + 8) = guest_va + 8;
        BRIDGE_MEM32(guest_va + 12) = guest_va + 8;
        ke_shadow_remove(guest_va);        /* an earlier object at this address */
        if (ke_guest_event(guest_va, &type)) {
            g_eax = 0;
            return;
        }
    }

    BOOL manual_reset = (event_type == 0) ? TRUE : FALSE;
    HANDLE h = CreateEventW(NULL, manual_reset, initial_state ? TRUE : FALSE, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeEvent: CreateEventW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    /* Also zero the guest KEVENT struct (16 bytes) and set SignalState.
     * KEVENT layout: +0x00 Type(UCHAR), +0x01 Absolute(UCHAR),
     *                +0x02 Size(UCHAR), +0x03 Inserted(UCHAR)
     *                +0x04 SignalState(LONG), +0x08 WaitListHead.Flink,
     *                +0x0C WaitListHead.Blink */
    BRIDGE_MEM8(guest_va + 0)  = (uint8_t)(event_type ? 1 : 0);
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = initial_state ? 1 : 0; /* SignalState */
    BRIDGE_MEM32(guest_va + 8) = 0;  /* WaitListHead.Flink (self/empty) */
    BRIDGE_MEM32(guest_va + 12) = 0; /* WaitListHead.Blink (self/empty) */
    ke_shadow_note_header(guest_va);

    g_eax = 0;
}

/* --- KeInitializeMutant (ordinal 110, 2 args = 8 bytes) --- */
static void bridge_KeInitializeMutant(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t initial_owner = STACK_ARG(1);

    HANDLE h = CreateMutexW(NULL, initial_owner ? TRUE : FALSE, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeMutant: CreateMutexW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    /* Mutant dispatcher header. SignalState semantics differ from events:
     * a mutant is SIGNALLED (owned count 0) when its SignalState is 0.
     * Win32 CreateMutex(initialOwner=TRUE) is owned, so SignalState=1. */
    BRIDGE_MEM8(guest_va + 0)  = 19;   /* DispatcherObjectType */
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = initial_owner ? 1 : 0; /* SignalState */
    BRIDGE_MEM32(guest_va + 8) = 0;   /* WaitListHead */
    BRIDGE_MEM32(guest_va + 12) = 0;
    ke_shadow_note_header(guest_va);

    g_eax = 0;
}

/* --- KeInitializeSemaphore (ordinal 112, 3 args = 12 bytes) --- */
static void bridge_KeInitializeSemaphore(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t count = STACK_ARG(1);
    uint32_t limit = STACK_ARG(2);

    HANDLE h = CreateSemaphoreW(NULL, (LONG)count, (LONG)limit, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] KeInitializeSemaphore: CreateSemaphoreW failed (error %u)\n",
                GetLastError());
        g_eax = 0;
        return;
    }

    ke_shadow_insert(guest_va, h);

    BRIDGE_MEM8(guest_va + 0)  = 18;   /* DispatcherObjectType */
    BRIDGE_MEM8(guest_va + 1)  = 0;
    BRIDGE_MEM8(guest_va + 2)  = 16;
    BRIDGE_MEM8(guest_va + 3)  = 1;
    BRIDGE_MEM32(guest_va + 4) = (LONG)count; /* SignalState = count */
    BRIDGE_MEM32(guest_va + 8) = 0;   /* WaitListHead */
    BRIDGE_MEM32(guest_va + 12) = 0;
    ke_shadow_note_header(guest_va);

    g_eax = 0;
}

/* --- KePulseEvent (ordinal 123, 3 args = 12 bytes) --- */
static void bridge_KePulseEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    HANDLE h;

    (void)increment;
    (void)wait;

    xbox_wait_stats_set(guest_va);
    {
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            PulseEvent(ge);
            BRIDGE_MEM32(guest_va + 4) = 0;
            g_eax = 0;
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        PulseEvent(h);

    g_eax = 0; /* previous state unknown */
}

/* --- KeReleaseMutant (ordinal 131, 4 args = 16 bytes) --- */
static void bridge_KeReleaseMutant(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);
    uint32_t abandoned = STACK_ARG(3);
    HANDLE h;

    (void)increment;
    (void)wait;
    (void)abandoned;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ReleaseMutex(h);

    g_eax = 0;
}

/* --- KeReleaseSemaphore (ordinal 132, 4 args = 16 bytes) --- */
static void bridge_KeReleaseSemaphore(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t adjustment = STACK_ARG(2);  /* LONG */
    uint32_t wait = STACK_ARG(3);
    HANDLE h;

    (void)increment;
    (void)wait;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ReleaseSemaphore(h, (LONG)adjustment, NULL);

    g_eax = 0;
}

/* --- KeResetEvent (ordinal 138, 1 arg = 4 bytes) --- */
static void bridge_KeResetEvent(void)
{
    uint32_t guest_va = STACK_ARG(0);
    HANDLE h;

    {
        int type;
        HANDLE ge = ke_guest_event(guest_va, &type);
        if (ge) {
            BRIDGE_MEM32(guest_va + 4) = 0;
            ResetEvent(ge);
            g_eax = 0;
            return;
        }
    }
    h = ke_shadow_lookup(guest_va);
    if (!h)
        ke_lost_signal(guest_va, 0);     /* the matching reset (see KeSetEvent) */
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        ResetEvent(h);

    g_eax = 0;
}

/* --- KeResumeThread (ordinal 140, 1 arg = 4 bytes) --- */
static void bridge_KeResumeThread(void)
{
    HANDLE hThread = bridge_resolve_handle(STACK_ARG(0));
    if (!hThread)
        hThread = XBOX_TO_NATIVE(STACK_ARG(0));

    if (hThread)
        g_eax = (uint32_t)ResumeThread(hThread);
    else
        g_eax = 0;
}

/* --- KeSuspendThread (ordinal 152, 1 arg = 4 bytes) --- */
static void bridge_KeSuspendThread(void)
{
    HANDLE hThread = bridge_resolve_handle(STACK_ARG(0));
    if (!hThread)
        hThread = XBOX_TO_NATIVE(STACK_ARG(0));

    if (hThread)
        g_eax = (uint32_t)SuspendThread(hThread);
    else
        g_eax = 0;
}

/* --- KeAlertResumeThread (ordinal 92, 1 arg = 4 bytes)
 * ULONG KeAlertResumeThread(PKTHREAD Thread): a kernel-mode alert (it ends
 * an alertable wait of either mode, else sets Alerted[KernelMode]), then a
 * resume; the previous suspend count back (XBA-133). The host thread is the
 * one the KE shadow keeps for the thread object. */
static void bridge_KeAlertResumeThread(void)
{
    uint32_t kt = STACK_ARG(0);
    HANDLE th = ke_shadow_lookup(kt);

    thalert_alert(kt, 0);
    g_eax = th ? (uint32_t)ResumeThread(th) : 0;
    if (g_eax == (uint32_t)-1)
        g_eax = 0;
}

/* --- KeBoostPriorityThread (ordinal 94, 2 args = 8 bytes) --- */
static void bridge_KeBoostPriorityThread(void)
{
    /* KTHREAD.Priority (+0x32) rises to BasePriority (+0x70) + Increment,
     * never into the real-time range. A boost is transient on the console
     * and the host scheduler has no counterpart: the field moves, the host
     * thread does not. */
    uint32_t t = STACK_ARG(0);
    int32_t inc = (int32_t)STACK_ARG(1);

    if (t && (int8_t)BRIDGE_MEM8(t + 0x32) < 16) {
        int32_t p = (int8_t)BRIDGE_MEM8(t + 0x70) + inc;
        if (p > 15) p = 15;
        if (p > (int8_t)BRIDGE_MEM8(t + 0x32))
            BRIDGE_MEM8(t + 0x32) = (uint8_t)p;
    }
    g_eax = 0;
}

/* --- KeEnterCriticalRegion (ordinal 101, 0 args = 0 bytes) --- */
static void bridge_KeEnterCriticalRegion(void)
{
    g_eax = 0;
}

/* --- KeInitializeApc (ordinal 105, 7 args = 28 bytes)
 * VOID KeInitializeApc(PRKAPC Apc, PRKTHREAD Thread,
 *                      PKKERNEL_ROUTINE KernelRoutine,
 *                      PKRUNDOWN_ROUTINE RundownRoutine,
 *                      PKNORMAL_ROUTINE NormalRoutine,
 *                      KPROCESSOR_MODE ProcessorMode, PVOID NormalContext);
 * Seven arguments (nxdk xboxkrnl.h; NT's ApcStateIndex is not there). The
 * table said six, which left one dword on the caller's stack. */
static void bridge_KeInitializeApc(void)
{
    ks_apc_init(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                STACK_ARG(4), STACK_ARG(5) & 0xFF, STACK_ARG(6));
    g_eax = 0;
}

/* --- KeInitializeDeviceQueue (ordinal 106, 1 arg = 4 bytes) --- */
static void bridge_KeInitializeDeviceQueue(void)
{
    ks_devq_init(STACK_ARG(0));   /* kernel_ksync.c (XBA-113) */
    g_eax = 0;
}

/* --- KeInitializeQueue (ordinal 111, 2 args = 8 bytes) --- */
static void bridge_KeInitializeQueue(void)
{
    ks_queue_init(STACK_ARG(0), STACK_ARG(1));   /* kernel_ksync.c */
    g_eax = 0;
}

/* --- KeInsertByKeyDeviceQueue (ordinal 114, 3 args = 12 bytes) --- */
static void bridge_KeInsertByKeyDeviceQueue(void)
{
    g_eax = (uint32_t)ks_devq_insert_by_key(STACK_ARG(0), STACK_ARG(1),
                                            STACK_ARG(2));
}

/* --- KeInsertDeviceQueue (ordinal 115, 2 args = 8 bytes) --- */
static void bridge_KeInsertDeviceQueue(void)
{
    g_eax = (uint32_t)ks_devq_insert(STACK_ARG(0), STACK_ARG(1));
}

/* --- KeInsertHeadQueue (ordinal 116, 2 args = 8 bytes) --- */
static void bridge_KeInsertHeadQueue(void)
{
    g_eax = (uint32_t)ks_queue_insert(STACK_ARG(0), STACK_ARG(1), 1);
}

/* --- KeInsertQueue (ordinal 117, 2 args = 8 bytes) --- */
static void bridge_KeInsertQueue(void)
{
    g_eax = (uint32_t)ks_queue_insert(STACK_ARG(0), STACK_ARG(1), 0);
}

/* --- KeInsertQueueApc (ordinal 118, 4 args = 16 bytes) --- */
static void bridge_apc_run_kernel(void);

static void bridge_KeInsertQueueApc(void)
{
    uint32_t apc = STACK_ARG(0);
    int ok = ks_apc_insert(apc, STACK_ARG(1), STACK_ARG(2));

    /* A kernel-mode APC for the calling thread runs before the call returns,
     * as on the console when the insert drops back below APC_LEVEL. One for
     * another thread runs at that thread's next kernel call; a user-mode one
     * at its next alertable user-mode wait (bridge_apc_safe_point), or ends
     * the one it is blocked in now (thalert_wake_for_apc, XBA-133). */
    if (ok && BRIDGE_MEM8(apc + 2) == 0 && BRIDGE_MEM32(apc + 4) == thobj_current())
        bridge_apc_run_kernel();
    else if (ok && BRIDGE_MEM8(apc + 2) == 1)
        thalert_wake_for_apc(BRIDGE_MEM32(apc + 4));
    g_eax = (uint32_t)ok;
}

/* Run a guest stdcall routine on this thread with `n` arguments, putting the
 * caller's registers back afterwards (see callback_regs_save). */
static void bridge_call_guest(uint32_t va, const uint32_t *args, int n)
{
    recomp_func_t fn = recomp_lookup(va);
    CallbackRegs saved;
    int i;

    if (!fn) fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] APC routine 0x%08X unresolved\n", va);
        fflush(stderr);
        return;
    }
    callback_regs_save(&saved);
    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4; BRIDGE_MEM32(g_esp) = args[i];
    }
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;          /* dummy return address */
    {
        int took = xbox_GuestCpuEnterCallback();
        fn();
        xbox_GuestCpuLeaveCallback(took);
    }
    callback_regs_restore(&saved, 0);
}

/* Deliver one APC taken off its thread's queue: KernelRoutine(Apc,
 * &NormalRoutine, &NormalContext, &SystemArgument1, &SystemArgument2), which
 * may change or clear what follows, then NormalRoutine(NormalContext,
 * SystemArgument1, SystemArgument2). The four values live just below esp
 * for the length of the delivery. */
static int apc_pool_owned(uint32_t apc);

static void bridge_apc_deliver(uint32_t apc)
{
    uint32_t save_esp = g_esp, area;
    uint32_t kr = BRIDGE_MEM32(apc + 0x10);

    g_esp -= 16;
    area = g_esp;
    BRIDGE_MEM32(area + 0)  = BRIDGE_MEM32(apc + 0x18);   /* NormalRoutine */
    BRIDGE_MEM32(area + 4)  = BRIDGE_MEM32(apc + 0x1C);   /* NormalContext */
    BRIDGE_MEM32(area + 8)  = BRIDGE_MEM32(apc + 0x20);   /* SystemArgument1 */
    BRIDGE_MEM32(area + 12) = BRIDGE_MEM32(apc + 0x24);   /* SystemArgument2 */
    if (kr) {
        uint32_t a[5] = { apc, area, area + 4, area + 8, area + 12 };
        bridge_call_guest(kr, a, 5);
    }
    if (BRIDGE_MEM32(area + 0)) {
        uint32_t a[3] = { BRIDGE_MEM32(area + 4), BRIDGE_MEM32(area + 8),
                          BRIDGE_MEM32(area + 12) };
        bridge_call_guest(BRIDGE_MEM32(area + 0), a, 3);
    }
    g_esp = save_esp;
    if (apc_pool_owned(apc))              /* NtQueueApcThread's own KAPC */
        xbox_HeapFree(apc);
}

static void bridge_apc_run_kernel(void)
{
    uint32_t kt = thobj_current(), apc;
    while ((apc = ks_apc_take(kt, 0)) != 0)
        bridge_apc_deliver(apc);
}

/* At the top of every kernel call while any APC is queued (cheap gate:
 * ks_apc_pending). Kernel-mode APCs for this thread run first. If the call
 * is an alertable user-mode wait and user-mode APCs are queued for this
 * thread, they run and the wait returns STATUS_USER_APC without waiting, as
 * on the console. Returns 1 when the call is answered that way. esp points
 * at the arguments.
 *
 * The file-I/O completions queued on this thread run in the same wait, first
 * (bridge_run_user_apcs).
 *
 * A user-mode APC queued while its thread is already blocked in an
 * alertable wait wakes it (XBA-133): see bridge_run_io_apcs. */
static int bridge_apc_safe_point(ULONG ordinal)
{
    int alertable = 0;

    bridge_apc_run_kernel();
    switch (ordinal) {
    case  99: alertable = (STACK_ARG(0) & 0xFF) == 1 && (STACK_ARG(1) & 0xFF); break;
    case 158: alertable = (STACK_ARG(4) & 0xFF) == 1 && (STACK_ARG(5) & 0xFF); break;
    case 159: alertable = (STACK_ARG(2) & 0xFF) == 1 && (STACK_ARG(3) & 0xFF); break;
    case 233: alertable = (STACK_ARG(1) & 0xFF) != 0; break;
    case 234: alertable = (STACK_ARG(1) & 0xFF) == 1 && (STACK_ARG(2) & 0xFF); break;
    case 235: alertable = (STACK_ARG(3) & 0xFF) == 1 && (STACK_ARG(4) & 0xFF); break;
    default: break;
    }
    if (!alertable)
        return 0;
    return bridge_run_user_apcs();
}

/* --- KeInsertQueueDpc (ordinal 119) ---
 * BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1,
 *                          PVOID SystemArgument2)
 *
 * Enqueues a DPC for the timer thread to drain. See the queue above. */

/* --- KeIsExecutingDpc (ordinal 121, 0 args = 0 bytes) --- */
static void bridge_KeIsExecutingDpc(void)
{
    g_eax = 0;
}

/* --- KeLeaveCriticalRegion (ordinal 122, 0 args = 0 bytes) --- */
static void bridge_KeLeaveCriticalRegion(void)
{
    g_eax = 0;
}

/* --- KeRaiseIrqlToSynchLevel (ordinal 130, 0 args = 0 bytes)
 * The console's kernel is uniprocessor (NT_UP), where SYNCH_LEVEL is
 * DISPATCH_LEVEL: the same raise as KeRaiseIrqlToDpcLevel, the old IRQL
 * back (XBA-114). */
static void bridge_KeRaiseIrqlToSynchLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

/* --- KeRemoveByKeyDeviceQueue (ordinal 133, 2 args = 8 bytes) --- */
static void bridge_KeRemoveByKeyDeviceQueue(void)
{
    g_eax = ks_devq_remove_by_key(STACK_ARG(0), STACK_ARG(1));
}

/* --- KeRemoveDeviceQueue (ordinal 134, 1 arg = 4 bytes) --- */
static void bridge_KeRemoveDeviceQueue(void)
{
    g_eax = ks_devq_remove(STACK_ARG(0));
}

/* --- KeRemoveEntryDeviceQueue (ordinal 135, 2 args = 8 bytes) --- */
static void bridge_KeRemoveEntryDeviceQueue(void)
{
    g_eax = (uint32_t)ks_devq_remove_entry(STACK_ARG(0), STACK_ARG(1));
}

/* --- KeRemoveQueue (ordinal 136, 3 args = 12 bytes)
 * PLIST_ENTRY KeRemoveQueue(PRKQUEUE Queue, KPROCESSOR_MODE WaitMode,
 *                           PLARGE_INTEGER Timeout);
 * Three arguments (nxdk xboxkrnl.h); the table said one. Returns the entry,
 * or STATUS_TIMEOUT. */
static void bridge_KeRemoveQueue(void)
{
    g_eax = ks_queue_remove(STACK_ARG(0), STACK_ARG(2));
}

/* --- KeRemoveQueueDpc (ordinal 137, 1 arg = 4 bytes) ---
 * BOOLEAN KeRemoveQueueDpc(PKDPC Dpc)
 *
 * Removes a queued DPC. See the queue above. */

/* --- KeRundownQueue (ordinal 141, 1 arg = 4 bytes) --- */
static void bridge_KeRundownQueue(void)
{
    g_eax = ks_queue_rundown(STACK_ARG(0));
}

/* --- KeSetEventBoostPriority (ordinal 146, 2 args = 8 bytes) --- */
static void bridge_KeSetEventBoostPriority(void)
{
    uint32_t guest_va = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    HANDLE h;

    (void)increment;

    h = ke_shadow_lookup(guest_va);
    if (!h)
        h = XBOX_TO_NATIVE(guest_va);
    if (h)
        SetEvent(h);

    g_eax = 0;
}

/* --- KeSetPriorityProcess (ordinal 147, 2 args = 8 bytes) --- */
static void bridge_KeSetPriorityProcess(void)
{
    /* KPRIORITY KeSetPriorityProcess(PKPROCESS, KPRIORITY BasePriority):
     * KPROCESS.BasePriority (+0x18), the old value back. Host threads keep
     * their own priorities, which are relative to it already. */
    uint32_t p = STACK_ARG(0);
    g_eax = 0;
    if (p) {
        g_eax = (uint32_t)(int32_t)(int8_t)BRIDGE_MEM8(p + 0x18);
        BRIDGE_MEM8(p + 0x18) = (uint8_t)STACK_ARG(1);
    }
}

/* --- KeSetPriorityThread (ordinal 148, 2 args = 8 bytes) --- */
static void bridge_KeSetPriorityThread(void)
{
    /* KPRIORITY KeSetPriorityThread(PKTHREAD, KPRIORITY): KTHREAD.Priority
     * (+0x32), the old value back, and the host thread follows relative to
     * the normal priority 8, as KeSetBasePriorityThread's increment does. */
    uint32_t t = STACK_ARG(0);
    int32_t pri = (int32_t)STACK_ARG(1);
    HANDLE h;

    g_eax = 0;
    if (!t)
        return;
    g_eax = (uint32_t)(int32_t)(int8_t)BRIDGE_MEM8(t + 0x32);
    BRIDGE_MEM8(t + 0x32) = (uint8_t)pri;
    h = bridge_thread_host(t);
    if (h)
        xbox_KeSetBasePriorityThread(h, pri - 8);
}

/* --- KeTestAlertThread (ordinal 155, 1 arg = 4 bytes)
 * BOOLEAN KeTestAlertThread(KPROCESSOR_MODE AlertMode): the calling
 * thread's Alerted[AlertMode], cleared (XBA-133). The console also marks a
 * user APC pending here when one is queued; kernel_ksync.c's insert has
 * marked it already. */
static void bridge_KeTestAlertThread(void)
{
    g_eax = (uint32_t)thalert_test(thalert_self_kt(), (int)(STACK_ARG(0) & 0xFF));
}

/* --- KiBugCheckData (ordinal 162, 0 args = 0 bytes) --- */
static void bridge_KiBugCheckData(void)
{
    g_eax = XBOX_KERNEL_DATA_BASE + 0x500;
}

/* --- KiUnlockDispatcherDatabase (ordinal 163, fastcall: OldIrql in cl)
 * Releases the dispatcher lock, which on a uniprocessor kernel is the IRQL
 * raise: lowers back to OldIrql as KfLowerIrql does, running what became
 * runnable (XBA-114). Fastcall, so nothing on the stack: the table said
 * four bytes. */
static void bridge_KiUnlockDispatcherDatabase(void)
{
    xbox_KfLowerIrql((UCHAR)g_ecx);
    g_eax = 0;
}

/* --- KeGetCurrentIrql (ordinal 103, 0 args = 0 bytes)
 * Stack-based with 0 args (not the Kf* fastcall form). IRQL is unmounted, so
 * report PASSIVE_LEVEL. */
static void bridge_KeGetCurrentIrql(void)
{
    g_eax = 0;  /* PASSIVE_LEVEL */
}

/* --- KeGetCurrentThread (ordinal 104, 0 args = 0 bytes) --- */
static void bridge_KeGetCurrentThread(void)
{
    /* The thread's KTHREAD, the one fs:[0x28] names (XBA-113). */
    g_eax = thobj_current();
}

/* --- KeSetDisableBoostThread (ordinal 144, 2 args = 8 bytes)
 * LOGICAL KeSetDisableBoostThread(PKTHREAD Thread, LOGICAL Disable): sets
 * KTHREAD.DisableBoost (+0x73, nxdk's layout) and returns the old value
 * (XAPI's SetThreadPriorityBoost). The host scheduler has no boost to turn
 * off, as for KeBoostPriorityThread; the field is what a title reads. */
static void bridge_KeSetDisableBoostThread(void)
{
    uint32_t t = STACK_ARG(0);

    if (!t || !bridge_va_mapped(t + 0x73, 1)) {
        g_eax = 0;
        return;
    }
    g_eax = BRIDGE_MEM8(t + 0x73);
    BRIDGE_MEM8(t + 0x73) = (uint8_t)(STACK_ARG(1) != 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Mm (174, 374, 375, 376, 377, 378)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- MmIsAddressValid (ordinal 174, 1 arg = 4 bytes)
 * BOOLEAN MmIsAddressValid(PVOID VirtualAddress): whether a read there
 * would not fault -- the guest window, a lazy reservation, contiguous
 * memory (bridge_va_mapped). It answered TRUE for every address,
 * page zero included (XBA-114). */
static void bridge_MmIsAddressValid(void)
{
    g_eax = (uint32_t)bridge_va_mapped(STACK_ARG(0), 1);
}

/* --- MmDbgAllocateMemory (ordinal 374, 2 args = 8 bytes)
 * PVOID MmDbgAllocateMemory(ULONG Size, ULONG AllocationTag);
 * Allocates zeroed guest memory. */
static void bridge_MmDbgAllocateMemory(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t va;

    (void)tag;
    if (!size) {
        g_eax = 0;
        return;
    }
    va = xbox_HeapAlloc(size, 16);
    if (va)
        memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, size);
    g_eax = va;
}

/* --- MmDbgFreeMemory (ordinal 375, 2 args = 8 bytes)
 * VOID MmDbgFreeMemory(PVOID BaseAddress, ULONG Size); */
static void bridge_MmDbgFreeMemory(void)
{
    (void)STACK_ARG(1);
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* --- MmDbgQueryAvailablePages (ordinal 376, 0 args = 0 bytes)
 * The pages the debug kit's extra memory has left; with no debug memory
 * here, the free pages MmQueryStatistics reports (XBA-114). */
static void bridge_MmDbgQueryAvailablePages(void)
{
    XBOX_MM_STATISTICS st;

    memset(&st, 0, sizeof st);
    st.Length = sizeof st;
    g_eax = xbox_MmQueryStatistics(&st) == STATUS_SUCCESS ? st.AvailablePages : 0;
}

/* --- MmDbgReleaseAddress (ordinal 377, 2 args = 8 bytes) --- */
static void bridge_MmDbgReleaseAddress(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    g_eax = 0;
}

/* --- MmDbgWriteCheck (ordinal 378, 2 args = 8 bytes)
 * Passthrough: returns the first argument (the guest VA). */
static void bridge_MmDbgWriteCheck(void)
{
    (void)STACK_ARG(1);
    g_eax = STACK_ARG(0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Nt (185, 191, 194, 201, 204, 206, 208, 209, 212, 213, 214, 216, 220, 223,
 *      227, 229, 230, 237)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- NtCancelTimer (ordinal 185, 2 args = 8 bytes)
 * NTSTATUS NtCancelTimer(HANDLE TimerHandle);
 * Resolves the handle token (the pair created by NtCreateTimer) and cancels
 * the waitable timer. (No xbox_NtCancelTimer() exists in kernel.h; done
 * inline with the Win32 API.) */
static void bridge_NtCancelTimer(void)
{
    HANDLE h = bridge_resolve_handle(STACK_ARG(0));
    (void)STACK_ARG(1);

    if (h)
        CancelWaitableTimer(h);
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* --- NtCreateIoCompletion (ordinal 191, 4 args = 16 bytes)
 * NTSTATUS NtCreateIoCompletion(PHANDLE IoCompletionHandle,
 *                               ACCESS_MASK DesiredAccess,
 *                               POBJECT_ATTRIBUTES ObjectAttributes,
 *                               ULONG Count);
 * XAPI's CreateIoCompletionPort. The object is kernel_ksync.c's (XBA-114);
 * the handle is a manual-reset event it keeps signalled while packets are
 * queued, so a wait on the handle answers as a wait on the console's
 * KQUEUE does. It used to succeed without writing a handle. */
static void bridge_NtCreateIoCompletion(void)
{
    uint32_t handle_va = STACK_ARG(0);
    HANDLE h;

    if (!handle_va || !bridge_buf_ok(handle_va, 4, "NtCreateIoCompletion")) {
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    h = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!h || !ks_port_create(h, STACK_ARG(3))) {
        if (h) CloseHandle(h);
        g_eax = 0xC000009Au;                 /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }
    bridge_write_handle(handle_va, h);
    g_eax = 0;
}

/* --- NtCreateTimer (ordinal 194, 3 args = 12 bytes)
 * NTSTATUS NtCreateTimer(PHANDLE TimerHandle, POBJECT_ATTRIBUTES
 *                        ObjectAttributes, TIMER_TYPE TimerType);
 * Creates a waitable timer of the asked type and hands its token out through
 * the handle table, matching the NtCreateEvent/NtCreateMutant pattern. */
static void bridge_NtCreateTimer(void)
{
    HANDLE h;

    (void)STACK_ARG(1);

    if (STACK_ARG(0) && !bridge_buf_ok(STACK_ARG(0), 4, "NtCreateTimer")) {
        g_eax = 0xC0000005u;                /* STATUS_ACCESS_VIOLATION */
        return;
    }
    /* TimerType 0 = NotificationTimer (stays signalled), 1 =
     * SynchronizationTimer (released waiter resets it): XAPI's
     * CreateWaitableTimer(bManualReset = FALSE) asks for the second. */
    h = CreateWaitableTimerW(NULL, STACK_ARG(2) == 0, NULL);
    if (!h) {
        fprintf(stderr, "  [KERNEL] NtCreateTimer: CreateWaitableTimerW failed (error %u)\n",
                GetLastError());
        g_eax = 0xC000009Au;  /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }
    if (STACK_ARG(0))
        bridge_write_handle(STACK_ARG(0), h);
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── The object namespace, as far as a title can see it (XBA-114) ──
 *
 * Directory objects are not kept as objects: the console's namespace holds
 * a fixed set of directories and what the title adds to them, so a handle
 * to a directory is a token naming one of these (0xD1A0000n, untagged:
 * NtClose leaves it alone) and its contents are made up on each query:
 *   \                   ?? and Device (directories)
 *   \??                 the DOS-device links: D: (the title's disc), and
 *                       every link IoCreateSymbolicLink made (kernel_io.c)
 *   \Device             CdRom0 (device), Harddisk0 (directory)
 *   \Device\Harddisk0   Partition0-5 (devices)
 * plus, in each, the named objects ObInsertObject put there. An entry's
 * Type is its object type's pool tag ('Dire', 'Symb', 'Devi', or the tag
 * of the title's OBJECT_TYPE): nxdk's OBJECT_DIRECTORY_INFORMATION only
 * says "ULONG Type", and the tag is what the Xbox object types carry. */
#define OBDIR_TOKEN      0xD1A00000u
#define OBDIR_COUNT      4
#define OBTAG_DIRECTORY  0x65726944u   /* 'Dire' */
#define OBTAG_SYMLINK    0x626D7953u   /* 'Symb' */
#define OBTAG_DEVICE     0x69766544u   /* 'Devi' */

static const char *const s_obdir_path[OBDIR_COUNT] = {
    "\\", "\\??", "\\Device", "\\Device\\Harddisk0",
};

static int obdir_from_token(uint32_t token)
{
    return (token & 0xFFFFFFF0u) == OBDIR_TOKEN && (token & 0xFu) < OBDIR_COUNT
         ? (int)(token & 0xFu) : -1;
}

/* The directory a full name (no trailing backslash) names, or -1. */
static int obdir_find(const char *name)
{
    int i;
    for (i = 0; i < OBDIR_COUNT; i++)
        if (!_stricmp(name, s_obdir_path[i]))
            return i;
    return -1;
}

/* The full name an OBJECT_ATTRIBUTES names, RootDirectory a directory token
 * or none; NULL when it has none. Trailing backslashes go, but "\" stays. */
static const char *ob_attr_name(uint32_t oa_va, char *out, size_t n)
{
    const char *rel = oa_va ? bridge_get_xbox_path(oa_va) : NULL;
    int root = oa_va ? obdir_from_token(BRIDGE_MEM32(oa_va)) : -1;
    size_t len;

    if (!rel)
        return NULL;
    if (root >= 0 && rel[0] != '\\')
        snprintf(out, n, "%s%s%s", s_obdir_path[root], root ? "\\" : "", rel);
    else
        snprintf(out, n, "%s", rel);
    len = strlen(out);
    while (len > 1 && out[len - 1] == '\\')
        out[--len] = '\0';
    return out;
}

static int ob_named_entry(int dir, uint32_t index, char *name, size_t n, uint32_t *tag);

/* Entry `index` of directory `dir`: its name and type tag, 0 past the end. */
static int obdir_entry(int dir, uint32_t index, char *name, size_t n, uint32_t *tag)
{
    static const struct { int dir; const char *name; uint32_t tag; } fixed[] = {
        { 0, "??", OBTAG_DIRECTORY }, { 0, "Device", OBTAG_DIRECTORY },
        { 2, "CdRom0", OBTAG_DEVICE }, { 2, "Harddisk0", OBTAG_DIRECTORY },
        { 3, "Partition0", OBTAG_DEVICE }, { 3, "Partition1", OBTAG_DEVICE },
        { 3, "Partition2", OBTAG_DEVICE }, { 3, "Partition3", OBTAG_DEVICE },
        { 3, "Partition4", OBTAG_DEVICE }, { 3, "Partition5", OBTAG_DEVICE },
    };
    uint32_t k = 0;
    size_t i;
    int s, r;

    for (i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        if (fixed[i].dir == dir && k++ == index) {
            snprintf(name, n, "%s", fixed[i].name);
            *tag = fixed[i].tag;
            return 1;
        }
    if (dir == 1) {
        const char *link, *target;
        if (!xbox_LookupSymbolicLink("\\??\\D:") && k++ == index) {
            snprintf(name, n, "D:");
            *tag = OBTAG_SYMLINK;
            return 1;
        }
        for (s = 0; (r = xbox_SymbolicLinkAt(s, &link, &target)) >= 0; s++)
            if (r && !_strnicmp(link, "\\??\\", 4) && !strchr(link + 4, '\\') && k++ == index) {
                snprintf(name, n, "%s", link + 4);
                *tag = OBTAG_SYMLINK;
                return 1;
            }
    }
    return ob_named_entry(dir, index - k, name, n, tag);
}

/* --- NtOpenDirectoryObject (ordinal 201, 2 args = 8 bytes)
 * NTSTATUS NtOpenDirectoryObject(PHANDLE DirectoryHandle,
 *                                POBJECT_ATTRIBUTES ObjectAttributes);
 * Two arguments (nxdk xboxkrnl.h); the table said three. */
static void bridge_NtOpenDirectoryObject(void)
{
    uint32_t handle_va = STACK_ARG(0);
    char full[256];
    const char *name = ob_attr_name(STACK_ARG(1), full, sizeof full);
    int dir = name ? obdir_find(name) : -1;

    if (!handle_va || !bridge_buf_ok(handle_va, 4, "NtOpenDirectoryObject")) {
        g_eax = 0xC0000005u;
        return;
    }
    if (dir < 0) {
        g_eax = name ? 0xC0000034u           /* STATUS_OBJECT_NAME_NOT_FOUND */
                     : 0xC0000033u;          /* STATUS_OBJECT_NAME_INVALID */
        return;
    }
    BRIDGE_MEM32(handle_va) = OBDIR_TOKEN | (uint32_t)dir;
    g_eax = 0;
}

/* --- NtProtectVirtualMemory (ordinal 204, 4 args = 16 bytes)
 * NTSTATUS NtProtectVirtualMemory(PVOID* BaseAddress, PSIZE_T RegionSize,
 *                                 ULONG NewProtect, PULONG OldProtect);
 * Both pointers live in guest memory; translate and let VirtualProtect do the
 * work. (No xbox_NtProtectVirtualMemory() exists in kernel.h; done inline.) */
static void bridge_NtProtectVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t new_prot = STACK_ARG(2);
    uint32_t old_ptr  = STACK_ARG(3);
    uint32_t base_va = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;
    uint32_t size    = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    DWORD old = 0;

    if (VirtualProtect(XBOX_TO_NATIVE(base_va), size, (DWORD)new_prot, &old)) {
        if (old_ptr)
            BRIDGE_MEM32(old_ptr) = (uint32_t)old;
        g_eax = 0;  /* STATUS_SUCCESS */
    } else {
        g_eax = 0xC0000005u;  /* STATUS_ACCESS_VIOLATION */
    }
}

/* --- NtQueueApcThread (ordinal 206, 5 args = 20 bytes)
 * NTSTATUS NtQueueApcThread(HANDLE ThreadHandle, PPS_APC_ROUTINE ApcRoutine,
 *                           PVOID ApcArgument1, PVOID ApcArgument2,
 *                           PVOID ApcArgument3);
 * XAPI's QueueUserAPC (XBA-133). As on the console: a KAPC from the pool,
 * initialised for the thread with ApcRoutine as its user-mode NormalRoutine
 * and ApcArgument1 as its context, and inserted with Arguments 2 and 3, so
 * it runs as ApcRoutine(Argument1, Argument2, Argument3) at the thread's
 * alertable user-mode wait -- the one it is blocked in now, if any. The
 * console frees the KAPC from its KernelRoutine; here bridge_apc_deliver
 * frees the ones this made (apc_pool_owned). */
#define APC_POOL_MAX 256
static uint32_t s_apc_pool[APC_POOL_MAX];
static volatile LONG s_apc_pool_lock;

static void apc_pool_note(uint32_t apc, int add)
{
    int i;
    while (InterlockedCompareExchange(&s_apc_pool_lock, 1, 0) != 0)
        SwitchToThread();
    for (i = 0; i < APC_POOL_MAX; i++)
        if (add ? !s_apc_pool[i] : s_apc_pool[i] == apc) {
            s_apc_pool[i] = add ? apc : 0;
            break;
        }
    InterlockedExchange(&s_apc_pool_lock, 0);
}

/* 1 when bridge_NtQueueApcThread made this KAPC (and it is now forgotten). */
static int apc_pool_owned(uint32_t apc)
{
    int i, owned = 0;
    while (InterlockedCompareExchange(&s_apc_pool_lock, 1, 0) != 0)
        SwitchToThread();
    for (i = 0; i < APC_POOL_MAX && !owned; i++)
        if (s_apc_pool[i] == apc) {
            s_apc_pool[i] = 0;
            owned = 1;
        }
    InterlockedExchange(&s_apc_pool_lock, 0);
    return owned;
}

static void bridge_NtQueueApcThread(void)
{
    uint32_t token = STACK_ARG(0), routine = STACK_ARG(1);
    uint32_t kt = token == 0xFFFFFFFEu ? thobj_current() : thobj_from_token(token);
    uint32_t apc;

    if (!kt) {
        g_eax = 0xC0000008u;                 /* STATUS_INVALID_HANDLE */
        return;
    }
    apc = xbox_HeapAlloc(0x28, 16);
    if (!apc) {
        g_eax = 0xC000009Au;                 /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }
    apc_pool_note(apc, 1);
    ks_apc_init(apc, kt, 0, 0, routine, 1 /* UserMode */, STACK_ARG(2));
    if (!ks_apc_insert(apc, STACK_ARG(3), STACK_ARG(4))) {
        apc_pool_note(apc, 0);
        xbox_HeapFree(apc);
        g_eax = 0xC0000001u;                 /* STATUS_UNSUCCESSFUL */
        return;
    }
    thalert_wake_for_apc(kt);
    g_eax = 0;
}

/* --- NtQueryDirectoryObject (ordinal 208, 6 args = 24 bytes)
 * NTSTATUS NtQueryDirectoryObject(HANDLE DirectoryHandle, PVOID Buffer,
 *                                 ULONG Length, BOOLEAN RestartScan,
 *                                 PULONG Context, PULONG ReturnLength);
 * Six arguments (nxdk xboxkrnl.h); the table said seven. One entry per
 * call, as the console's has no ReturnSingleEntry: an
 * OBJECT_DIRECTORY_INFORMATION { OBJECT_STRING Name; ULONG Type; } (12
 * bytes) with the name's characters right after it. Context is the index
 * of the next entry; STATUS_NO_MORE_ENTRIES past the last, and
 * STATUS_BUFFER_TOO_SMALL with the size needed in ReturnLength. */
static void bridge_NtQueryDirectoryObject(void)
{
    int dir = obdir_from_token(STACK_ARG(0));
    uint32_t buf = STACK_ARG(1), length = STACK_ARG(2);
    uint32_t restart = STACK_ARG(3) & 0xFF, ctx_va = STACK_ARG(4), ret_va = STACK_ARG(5);
    uint32_t index, need, tag = 0;
    char name[128];
    size_t len;

    if (dir < 0) {
        g_eax = 0xC0000008u;                 /* STATUS_INVALID_HANDLE */
        return;
    }
    if (!ctx_va || !bridge_buf_ok(ctx_va, 4, "NtQueryDirectoryObject")) {
        g_eax = 0xC0000005u;
        return;
    }
    index = restart ? 0 : BRIDGE_MEM32(ctx_va);
    if (!obdir_entry(dir, index, name, sizeof name, &tag)) {
        g_eax = 0x8000001Au;                 /* STATUS_NO_MORE_ENTRIES */
        return;
    }
    len = strlen(name);
    need = 12u + (uint32_t)len + 1u;
    if (ret_va && bridge_buf_ok(ret_va, 4, "NtQueryDirectoryObject"))
        BRIDGE_MEM32(ret_va) = need;
    if (length < need || !bridge_buf_ok(buf, need, "NtQueryDirectoryObject")) {
        g_eax = 0xC0000023u;                 /* STATUS_BUFFER_TOO_SMALL */
        return;
    }
    BRIDGE_MEM16(buf + 0) = (uint16_t)len;
    BRIDGE_MEM16(buf + 2) = (uint16_t)(len + 1);
    BRIDGE_MEM32(buf + 4) = buf + 12;
    BRIDGE_MEM32(buf + 8) = tag;
    memcpy(XBOX_TO_NATIVE(buf + 12), name, len + 1);
    BRIDGE_MEM32(ctx_va) = index + 1;
    g_eax = 0;
}

/* ── NtQueryEvent / NtQueryMutant / NtQuerySemaphore / NtQueryTimer ──
 *
 * Each takes a handle and a basic-information structure (nxdk xboxkrnl.h:
 * two arguments each; the table said four or five). The object's state is
 * the host object's: the POSIX shim answers w32_query_object, Windows its
 * own ntdll NtQuery* on the same handle (the host objects are real NT
 * objects there, with the same structures). A handle that is no object is
 * STATUS_INVALID_HANDLE, one of another kind STATUS_OBJECT_TYPE_MISMATCH. */
#ifndef W32_QUERY_EVENT                     /* Windows: no shim header */
#define W32_QUERY_EVENT     1
#define W32_QUERY_SEMAPHORE 2
#define W32_QUERY_MUTEX     3
#define W32_QUERY_TIMER     4
#endif

static uint32_t bridge_query_host(uint32_t token, int want, LONG info[3])
{
    HANDLE h = bridge_resolve_handle(token);
#ifdef _WIN32
    typedef LONG (NTAPI *nt_query_fn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const char *const names[5] = { NULL, "NtQueryEvent", "NtQuerySemaphore",
                                          "NtQueryMutant", "NtQueryTimer" };
    LONG buf[4] = { 0, 0, 0, 0 };
    ULONG ret = 0;
    nt_query_fn f = (nt_query_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), names[want]);
    LONG st;
    if (!h || !f)
        return 0xC0000008u;
    st = f(h, 0, buf, sizeof buf, &ret);
    if (st < 0)
        return (uint32_t)st;
    info[0] = buf[0];
    info[1] = want == W32_QUERY_MUTEX ? ((UCHAR *)&buf[1])[0] : buf[1];
    info[2] = want == W32_QUERY_TIMER ? ((UCHAR *)&buf[2])[0] : 0;
    return 0;
#else
    int kind = h ? w32_query_object(h, info) : 0;
    if (!kind)
        return 0xC0000008u;                  /* STATUS_INVALID_HANDLE */
    return kind == want ? 0 : 0xC0000024u;   /* STATUS_OBJECT_TYPE_MISMATCH */
#endif
}

/* --- NtQueryEvent (ordinal 209, 2 args = 8 bytes)
 * EVENT_BASIC_INFORMATION { EVENT_TYPE EventType; LONG EventState; } */
static void bridge_NtQueryEvent(void)
{
    LONG info[3];
    uint32_t out = STACK_ARG(1);

    if (!out || !bridge_buf_ok(out, 8, "NtQueryEvent")) {
        g_eax = 0xC0000005u;
        return;
    }
    g_eax = bridge_query_host(STACK_ARG(0), W32_QUERY_EVENT, info);
    if (g_eax == 0) {
        BRIDGE_MEM32(out) = (uint32_t)info[0];
        BRIDGE_MEM32(out + 4) = (uint32_t)info[1];
    }
}

/* --- NtQueryIoCompletion (ordinal 212, 2 args = 8 bytes)
 * NTSTATUS NtQueryIoCompletion(HANDLE, PIO_COMPLETION_BASIC_INFORMATION);
 * The basic information is one LONG, Depth: the packets queued. */
static void bridge_NtQueryIoCompletion(void)
{
    int32_t depth = ks_port_depth(bridge_resolve_handle(STACK_ARG(0)));
    uint32_t info_va = STACK_ARG(1);

    if (depth < 0) {
        g_eax = 0xC0000008u;                 /* STATUS_INVALID_HANDLE */
        return;
    }
    if (!info_va || !bridge_buf_ok(info_va, 4, "NtQueryIoCompletion")) {
        g_eax = 0xC0000005u;
        return;
    }
    BRIDGE_MEM32(info_va) = (uint32_t)depth;
    g_eax = 0;
}

/* --- NtQueryMutant (ordinal 213, 2 args = 8 bytes)
 * MUTANT_BASIC_INFORMATION { LONG CurrentCount; UCHAR OwnedByCaller;
 * UCHAR AbandonedState; }: 1 when free, 1 - recursion when owned. */
static void bridge_NtQueryMutant(void)
{
    LONG info[3];
    uint32_t out = STACK_ARG(1);

    if (!out || !bridge_buf_ok(out, 6, "NtQueryMutant")) {
        g_eax = 0xC0000005u;
        return;
    }
    g_eax = bridge_query_host(STACK_ARG(0), W32_QUERY_MUTEX, info);
    if (g_eax == 0) {
        BRIDGE_MEM32(out) = (uint32_t)info[0];
        BRIDGE_MEM8(out + 4) = (uint8_t)(info[1] != 0);
        BRIDGE_MEM8(out + 5) = 0;            /* a thread exiting owning it is not tracked */
    }
}

/* --- NtQuerySemaphore (ordinal 214, 2 args = 8 bytes)
 * SEMAPHORE_BASIC_INFORMATION { LONG CurrentCount; LONG MaximumCount; } */
static void bridge_NtQuerySemaphore(void)
{
    LONG info[3];
    uint32_t out = STACK_ARG(1);

    if (!out || !bridge_buf_ok(out, 8, "NtQuerySemaphore")) {
        g_eax = 0xC0000005u;
        return;
    }
    g_eax = bridge_query_host(STACK_ARG(0), W32_QUERY_SEMAPHORE, info);
    if (g_eax == 0) {
        BRIDGE_MEM32(out) = (uint32_t)info[0];
        BRIDGE_MEM32(out + 4) = (uint32_t)info[1];
    }
}

/* --- NtQueryTimer (ordinal 216, 2 args = 8 bytes)
 * TIMER_BASIC_INFORMATION { LARGE_INTEGER RemainingTime; BOOLEAN
 * TimerState; }: the 100 ns left until it is due, and whether it is
 * signalled. */
static void bridge_NtQueryTimer(void)
{
    LONG info[3];
    uint32_t out = STACK_ARG(1);

    if (!out || !bridge_buf_ok(out, 9, "NtQueryTimer")) {
        g_eax = 0xC0000005u;
        return;
    }
    g_eax = bridge_query_host(STACK_ARG(0), W32_QUERY_TIMER, info);
    if (g_eax == 0) {
        BRIDGE_MEM32(out) = (uint32_t)info[0];
        BRIDGE_MEM32(out + 4) = (uint32_t)info[1];
        BRIDGE_MEM8(out + 8) = (uint8_t)(info[2] != 0);
    }
}

/* --- NtReadFileScatter (ordinal 220, 8 args = 32 bytes)
 * NTSTATUS NtReadFileScatter(HANDLE FileHandle, HANDLE Event,
 *                            PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
 *                            PIO_STATUS_BLOCK IoStatusBlock,
 *                            PFILE_SEGMENT_ELEMENT SegmentArray,
 *                            ULONG Length, PLARGE_INTEGER ByteOffset);
 * XAPI's ReadFileScatter (XBA-114). SegmentArray is one 4-byte element per
 * page of the transfer (FILE_SEGMENT_ELEMENT is a PVOID on the console):
 * page i of the Length bytes lands at element i. The pieces are read in
 * order from ByteOffset (the current position when NULL); a short piece
 * ends the request. Completion is NtReadFile's: status block, event, APC or
 * completion packet, none of the last two when it fails synchronously. */
static void bridge_rw_segments(int write)
{
    HANDLE   handle  = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va  = STACK_ARG(4), seg_va = STACK_ARG(5);
    uint32_t length  = STACK_ARG(6), off_va = STACK_ARG(7);
    const char *who  = write ? "NtWriteFileGather" : "NtReadFileScatter";
    uint32_t done = 0, n = (length + 0xFFFu) / 0x1000u, i;
    NTSTATUS st = STATUS_SUCCESS;
    LARGE_INTEGER off = {0};
    int have_off = 0;
    long long t0_us = bridge_clock_us();

    if (n && !bridge_buf_ok(seg_va, n * 4, who)) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (off_va) {
        off.LowPart  = BRIDGE_MEM32(off_va);
        off.HighPart = (LONG)BRIDGE_MEM32(off_va + 4);
        have_off = off.QuadPart >= 0;        /* -1/-2: current position / end, as NtReadFile */
    }
    for (i = 0; i < n; i++) {
        uint32_t buf = BRIDGE_MEM32(seg_va + i * 4);
        uint32_t want = length - done < 0x1000u ? length - done : 0x1000u;
        XBOX_IO_STATUS_BLOCK ios;
        LARGE_INTEGER at;

        if (!bridge_buf_ok(buf, want, who)) {
            st = (NTSTATUS)0xC0000005;
            break;
        }
        memset(&ios, 0, sizeof ios);
        at.QuadPart = off.QuadPart + done;
        st = write
           ? xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios, XBOX_TO_NATIVE(buf), want,
                              have_off ? &at : (off_va ? &off : NULL))
           : xbox_NtReadFile(handle, NULL, NULL, NULL, &ios, XBOX_TO_NATIVE(buf), want,
                             have_off ? &at : (off_va ? &off : NULL));
        done += (uint32_t)ios.Information;
        if (st != STATUS_SUCCESS || (uint32_t)ios.Information < want)
            break;
    }
    /* End of file part-way is a short transfer, not a failure. */
    if (!write && done && st == (NTSTATUS)0xC0000011)
        st = STATUS_SUCCESS;
    if (!write)
        bridge_read_done(t0_us, length, done);
    bridge_write_iostatus(ios_va, st, done);
    g_eax = (uint32_t)st;
    bridge_complete_file_io(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            ios_va, g_eax, done, 0);
}

static void bridge_NtReadFileScatter(void)
{
    bridge_rw_segments(0);
}

/* --- NtRemoveIoCompletion (ordinal 223, 5 args = 20 bytes)
 * NTSTATUS NtRemoveIoCompletion(HANDLE IoCompletionHandle, PVOID *KeyContext,
 *                               PVOID *ApcContext,
 *                               PIO_STATUS_BLOCK IoStatusBlock,
 *                               PLARGE_INTEGER Timeout);
 * XAPI's GetQueuedCompletionStatus: the oldest packet, or STATUS_TIMEOUT
 * once Timeout (NULL = forever) runs out. Not alertable, as on the console.
 * A blocking wait, filed with the others (kernel_wait_stats.h). */
static void bridge_NtRemoveIoCompletion(void)
{
    uint32_t token = STACK_ARG(0), key_va = STACK_ARG(1), ctx_va = STACK_ARG(2);
    uint32_t ios_va = STACK_ARG(3), timeout_va = STACK_ARG(4);
    uint32_t out[4] = { 0, 0, 0, 0 };
    uint64_t t0 = xbox_wait_stats_begin(bridge_timeout_is_poll(timeout_va));

    g_eax = ks_port_remove(bridge_resolve_handle(token), timeout_va, out);
    xbox_wait_stats_wait(token, g_xbox_kernel_caller, g_eax, t0);
    if (g_eax != 0)
        return;
    if (key_va && bridge_buf_ok(key_va, 4, "NtRemoveIoCompletion"))
        BRIDGE_MEM32(key_va) = out[0];
    if (ctx_va && bridge_buf_ok(ctx_va, 4, "NtRemoveIoCompletion"))
        BRIDGE_MEM32(ctx_va) = out[1];
    if (ios_va && bridge_buf_ok(ios_va, 8, "NtRemoveIoCompletion"))
        bridge_write_iostatus(ios_va, (NTSTATUS)out[2], out[3]);
}

/* --- NtSetIoCompletion (ordinal 227, 5 args = 20 bytes)
 * NTSTATUS NtSetIoCompletion(HANDLE IoCompletionHandle, PVOID KeyContext,
 *                            PVOID ApcContext, NTSTATUS IoStatus,
 *                            ULONG_PTR IoStatusInformation);
 * XAPI's PostQueuedCompletionStatus. */
static void bridge_NtSetIoCompletion(void)
{
    g_eax = ks_port_post(bridge_resolve_handle(STACK_ARG(0)), STACK_ARG(1),
                         STACK_ARG(2), STACK_ARG(3), STACK_ARG(4))
          ? 0xC0000008u : 0;                 /* STATUS_INVALID_HANDLE */
}

/* --- NtSetTimerEx (ordinal 229, 8 args = 32 bytes) --- */
/* NTSTATUS NtSetTimerEx(HANDLE TimerHandle, PLARGE_INTEGER DueTime,
 *                       PTIMER_APC_ROUTINE TimerApcRoutine, KPROCESSOR_MODE
 *                       ApcMode, PVOID TimerContext, BOOLEAN WakeTimer,
 *                       LONG Period, PBOOLEAN PreviousState);
 * XAPI's SetWaitableTimer. It used to succeed without arming anything, so a
 * title clocked by a waitable timer never ticked: SSX On Tour's 60 Hz frame
 * clock thread (guest 0x00222E71) waits on one and re-arms it each time,
 * and the "Saving..." screen waits on that clock for its 3 s minimum, so
 * every save hung. A timer APC is not delivered (none seen yet). */
static void bridge_NtSetTimerEx(void)
{
    HANDLE   h       = bridge_resolve_handle(STACK_ARG(0));
    uint32_t due_va  = STACK_ARG(1);
    uint32_t apc     = STACK_ARG(2);
    LONG     period  = (LONG)STACK_ARG(6);
    uint32_t prev_va = STACK_ARG(7);
    LARGE_INTEGER due;

    if (!h) {
        g_eax = 0xC0000008u;                /* STATUS_INVALID_HANDLE */
        return;
    }
    if (!bridge_buf_ok(due_va, 8, "NtSetTimerEx")
            || (prev_va && !bridge_buf_ok(prev_va, 1, "NtSetTimerEx"))) {
        g_eax = 0xC0000005u;                /* STATUS_ACCESS_VIOLATION */
        return;
    }
    due.QuadPart = (LONGLONG)((uint64_t)BRIDGE_MEM32(due_va)
                              | ((uint64_t)BRIDGE_MEM32(due_va + 4) << 32));
    if (apc) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [KERNEL] NtSetTimerEx: timer APC 0x%08X not delivered\n", apc);
    }
    if (prev_va)                    /* the prior state is not tracked: "was not signalled" */
        BRIDGE_MEM8(prev_va) = 0;
    g_eax = SetWaitableTimer(h, &due, period, NULL, NULL, FALSE) ? 0 : 0xC000000Du;
}

/* --- NtSignalAndWaitForSingleObjectEx (ordinal 230, 5 args = 20 bytes)
 * NTSTATUS NtSignalAndWaitForSingleObjectEx(HANDLE SignalHandle,
 *                                           HANDLE WaitHandle,
 *                                           KPROCESSOR_MODE WaitMode,
 *                                           BOOLEAN Alertable,
 *                                           PLARGE_INTEGER Timeout);
 * XAPI's SignalObjectAndWait (XBA-114): set the event, release the
 * semaphore by one or the mutant, then wait on WaitHandle as
 * NtWaitForSingleObjectEx does. A signal that fails ends the call with its
 * status and no wait, as on the console: STATUS_MUTANT_NOT_OWNED for a
 * mutant the caller does not own, STATUS_OBJECT_TYPE_MISMATCH for an
 * object that cannot be signalled. The console does both under one
 * dispatcher lock; here another thread may run between the two, which only
 * a title that polls the signalled object could see. It used to return
 * STATUS_SUCCESS without either. */
static void bridge_NtSignalAndWaitForSingleObjectEx(void)
{
    HANDLE   sig         = bridge_resolve_handle(STACK_ARG(0));
    uint32_t token       = STACK_ARG(1);
    HANDLE   wait_h      = bridge_resolve_handle(token);
    uint32_t wait_mode   = STACK_ARG(2) & 0xFF;
    uint32_t alertable   = STACK_ARG(3) & 0xFF;
    uint32_t timeout_ptr = STACK_ARG(4);
    uint32_t site        = g_xbox_kernel_caller;
    uint64_t t0;

    if (!sig || !wait_h) {
        g_eax = 0xC0000008u;                 /* STATUS_INVALID_HANDLE */
        return;
    }
    xbox_wait_stats_set(STACK_ARG(0));
    if (!SetEvent(sig) && !ReleaseSemaphore(sig, 1, NULL)) {
        if (!ReleaseMutex(sig)) {
            g_eax = GetLastError() == ERROR_NOT_OWNER
                  ? 0xC0000046u               /* STATUS_MUTANT_NOT_OWNED */
                  : 0xC0000024u;              /* STATUS_OBJECT_TYPE_MISMATCH */
            return;
        }
    }
    if (bridge_run_io_apcs(alertable, wait_mode))
        return;
    t0 = xbox_wait_stats_begin(bridge_timeout_is_poll(timeout_ptr));
    g_eax = (uint32_t)xbox_NtWaitForSingleObjectEx(
        wait_h, (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
    bridge_wait_done(alertable, wait_mode);
    xbox_wait_stats_wait(token, site, g_eax, t0);
}

/* --- NtWriteFileGather (ordinal 237, 8 args = 32 bytes)
 * The gathering sibling of NtReadFileScatter, same arguments: page i of the
 * Length bytes comes from element i (XAPI's WriteFileGather). nxdk declares
 * it BOOLEAN; it returns an NTSTATUS like the read. */
static void bridge_NtWriteFileGather(void)
{
    bridge_rw_segments(1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ob (239, 241, 242, 243, 244, 248)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* ── Objects with a body in guest memory (XBA-114) ──
 *
 * ObCreateObject lays an object out as the console does: an OBJECT_HEADER
 * { PointerCount, HandleCount, Type, Flags } right before the body, and for
 * a named object an OBJECT_HEADER_NAME_INFO { ChainLink, Directory,
 * OBJECT_STRING Name } before that (Flags 1 = named, 2 = permanent). The
 * memory comes from the pool (xbox_HeapAlloc) whatever the type's
 * Allocate/FreeProcedure say: the console's own types use ExAllocatePool
 * there anyway, and calling a title's allocator from here would need its
 * result back across the guest call. The type's CloseProcedure(Object,
 * HandleCount) and DeleteProcedure(Object) are called, on the calling
 * thread, as on the console.
 *
 * A handle to such an object is a fresh token (its host side a spare
 * event); s_ob_token maps it to the body. Closing the last handle of a
 * temporary object takes its name out of the namespace; the last reference
 * deletes it. */
#define OB_MAX       256
#define OB_NAME_MAX  128
#define OB_PERMANENT 2u

static struct {
    uint32_t body, base;        /* 0: free */
    int      inserted;          /* its name is in the namespace */
    char     name[OB_NAME_MAX]; /* full name, "" when unnamed */
} s_ob[OB_MAX];
static uint32_t s_ob_token[BRIDGE_HANDLE_MAX];
static volatile LONG s_ob_lock;

static void ob_lock(void)
{
    while (InterlockedCompareExchange(&s_ob_lock, 1, 0) != 0)
        SwitchToThread();
}

static void ob_unlock(void) { InterlockedExchange(&s_ob_lock, 0); }

static int ob_slot(uint32_t body)    /* lock held */
{
    int i;
    if (!body)
        return -1;
    for (i = 0; i < OB_MAX; i++)
        if (s_ob[i].body == body)
            return i;
    return -1;
}

static int ob_is(uint32_t body)
{
    int s;
    ob_lock();
    s = ob_slot(body);
    ob_unlock();
    return s >= 0;
}

#define OB_HDR(body)   ((body) - 0x10u)
#define OB_PTRS(body)  (*(volatile LONG *)XBOX_TO_NATIVE(OB_HDR(body) + 0))
#define OB_HANDLES(body) (*(volatile LONG *)XBOX_TO_NATIVE(OB_HDR(body) + 4))
#define OB_TYPE(body)  BRIDGE_MEM32(OB_HDR(body) + 8)
#define OB_FLAGS(body) BRIDGE_MEM32(OB_HDR(body) + 12)

/* A type procedure (OBJECT_TYPE +8 Close, +0xC Delete), or 0. */
static uint32_t ob_type_proc(uint32_t body, uint32_t off)
{
    uint32_t type = OB_TYPE(body);
    return type && bridge_va_mapped(type, 0x1C) ? BRIDGE_MEM32(type + off) : 0;
}

static int ob_type_ok(uint32_t body, uint32_t type)
{
    return !type || OB_TYPE(body) == type;
}

static void ob_unname(uint32_t body)
{
    int s;
    ob_lock();
    s = ob_slot(body);
    if (s >= 0)
        s_ob[s].inserted = 0;
    ob_unlock();
}

/* Drop one reference; the last deletes the object. */
static void ob_deref(uint32_t body)
{
    uint32_t del, base = 0;
    int s;

    if (InterlockedDecrement(&OB_PTRS(body)) > 0)
        return;
    del = ob_type_proc(body, 0xC);
    if (del) {
        uint32_t a[1] = { body };
        bridge_call_guest(del, a, 1);
    }
    ob_lock();
    s = ob_slot(body);
    if (s >= 0) {
        base = s_ob[s].base;
        memset(&s_ob[s], 0, sizeof s_ob[s]);
    }
    ob_unlock();
    if (base)
        xbox_HeapFree(base);
}

/* A handle for the body: one more handle and pointer reference. */
static uint32_t ob_new_token(uint32_t body)
{
    HANDLE h = CreateEventA(NULL, TRUE, FALSE, NULL);
    uint32_t token = h ? bridge_handle_token_ex(h, 1) : 0;

    if (!token) {
        if (h) CloseHandle(h);
        return 0;
    }
    s_ob_token[token & BRIDGE_HANDLE_MASK] = body;
    InterlockedIncrement(&OB_HANDLES(body));
    return token;
}

static uint32_t ob_from_token(uint32_t token)
{
    if ((token & 0xFF000000u) != BRIDGE_HANDLE_TAG || !(token & BRIDGE_HANDLE_MASK)
            || (token & BRIDGE_HANDLE_MASK) >= BRIDGE_HANDLE_MAX)
        return 0;
    return s_ob_token[token & BRIDGE_HANDLE_MASK];
}

/* NtClose of an object's handle: 1 when the token was one. The host event
 * behind it is closed by NtClose as usual. */
static int ob_close_token(uint32_t token)
{
    uint32_t body = ob_from_token(token), close_proc;
    LONG left;

    if (!body)
        return 0;
    s_ob_token[token & BRIDGE_HANDLE_MASK] = 0;
    close_proc = ob_type_proc(body, 0x8);
    if (close_proc) {
        uint32_t a[2] = { body, (uint32_t)OB_HANDLES(body) };
        bridge_call_guest(close_proc, a, 2);
    }
    left = InterlockedDecrement(&OB_HANDLES(body));
    if (left <= 0 && !(OB_FLAGS(body) & OB_PERMANENT))
        ob_unname(body);
    ob_deref(body);
    return 1;
}

/* The inserted object of that full name, or 0. */
static uint32_t ob_lookup(const char *name)
{
    uint32_t body = 0;
    int i;
    ob_lock();
    for (i = 0; i < OB_MAX; i++)
        if (s_ob[i].body && s_ob[i].inserted && !_stricmp(s_ob[i].name, name)) {
            body = s_ob[i].body;
            break;
        }
    ob_unlock();
    return body;
}

/* Entry `index` among the named objects directly in directory `dir`. */
static int ob_named_entry(int dir, uint32_t index, char *name, size_t n, uint32_t *tag)
{
    const char *path = s_obdir_path[dir];
    size_t plen = strlen(path);
    uint32_t k = 0;
    int i, found = 0;

    ob_lock();
    for (i = 0; i < OB_MAX && !found; i++) {
        const char *nm = s_ob[i].name, *leaf;
        if (!s_ob[i].body || !s_ob[i].inserted)
            continue;
        if (dir == 0 ? nm[0] != '\\'
                     : (_strnicmp(nm, path, plen) || nm[plen] != '\\'))
            continue;
        leaf = nm + (dir == 0 ? 1 : plen + 1);
        if (!*leaf || strchr(leaf, '\\') || k++ != index)
            continue;
        snprintf(name, n, "%s", leaf);
        {
            uint32_t type = OB_TYPE(s_ob[i].body);
            *tag = type && bridge_va_mapped(type, 0x1C) ? BRIDGE_MEM32(type + 0x18) : 0;
        }
        found = 1;
    }
    ob_unlock();
    return found;
}

/* --- ObCreateObject (ordinal 239, 4 args = 16 bytes)
 * NTSTATUS ObCreateObject(POBJECT_TYPE ObjectType,
 *                         POBJECT_ATTRIBUTES ObjectAttributes,
 *                         ULONG ObjectBodySize, PVOID *Object);
 * Four arguments (nxdk xboxkrnl.h); the table said seven. */
static void bridge_ObCreateObject(void)
{
    uint32_t type = STACK_ARG(0), oa = STACK_ARG(1), size = STACK_ARG(2);
    uint32_t out_va = STACK_ARG(3), base, hdr, body;
    char full[OB_NAME_MAX];
    const char *name = ob_attr_name(oa, full, sizeof full);
    size_t len = name ? strlen(name) : 0;
    int i;

    if (!out_va || !bridge_buf_ok(out_va, 4, "ObCreateObject") || size > 0x01000000u) {
        g_eax = 0xC000000Du;                 /* STATUS_INVALID_PARAMETER */
        return;
    }
    base = xbox_HeapAlloc((name ? 0x10u : 0u) + 0x10u + size + (uint32_t)len + 1u, 16);
    if (!base) {
        g_eax = 0xC000009Au;                 /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }
    hdr = base + (name ? 0x10u : 0u);
    body = hdr + 0x10u;
    memset(XBOX_TO_NATIVE(base), 0, (name ? 0x10u : 0u) + 0x10u + size + len + 1u);
    BRIDGE_MEM32(hdr + 0)  = 1;              /* PointerCount */
    BRIDGE_MEM32(hdr + 4)  = 0;              /* HandleCount */
    BRIDGE_MEM32(hdr + 8)  = type;
    BRIDGE_MEM32(hdr + 12) = name ? 1u : 0u; /* OB_FLAG_NAMED_OBJECT */
    if (name) {
        memcpy(XBOX_TO_NATIVE(body + size), name, len + 1);
        BRIDGE_MEM16(base + 8)  = (uint16_t)len;
        BRIDGE_MEM16(base + 10) = (uint16_t)(len + 1);
        BRIDGE_MEM32(base + 12) = body + size;
    }
    ob_lock();
    for (i = 0; i < OB_MAX; i++)
        if (!s_ob[i].body) {
            s_ob[i].body = body;
            s_ob[i].base = base;
            s_ob[i].inserted = 0;
            snprintf(s_ob[i].name, sizeof s_ob[i].name, "%s", name ? name : "");
            break;
        }
    ob_unlock();
    if (i == OB_MAX) {
        xbox_HeapFree(base);
        g_eax = 0xC000009Au;
        return;
    }
    BRIDGE_MEM32(out_va) = body;
    g_eax = 0;
}

/* --- ObInsertObject (ordinal 241, 4 args = 16 bytes)
 * NTSTATUS ObInsertObject(PVOID Object, POBJECT_ATTRIBUTES ObjectAttributes,
 *                         ULONG ObjectPointerBias, PHANDLE Handle);
 * Four arguments (nxdk xboxkrnl.h); the table said six. Puts a named
 * object's name in the namespace (STATUS_OBJECT_NAME_COLLISION, and the
 * object dereferenced, when the name is taken), adds ObjectPointerBias
 * references, and opens a handle that owns the creation reference. */
static void bridge_ObInsertObject(void)
{
    uint32_t body = STACK_ARG(0), oa = STACK_ARG(1), bias = STACK_ARG(2);
    uint32_t handle_va = STACK_ARG(3), token;
    int s, taken = 0, i;

    if (handle_va && !bridge_buf_ok(handle_va, 4, "ObInsertObject")) {
        g_eax = 0xC0000005u;
        return;
    }
    ob_lock();
    s = ob_slot(body);
    if (s >= 0 && s_ob[s].name[0]) {
        for (i = 0; i < OB_MAX; i++)
            if (i != s && s_ob[i].body && s_ob[i].inserted
                    && !_stricmp(s_ob[i].name, s_ob[s].name))
                taken = 1;
        if (!taken)
            s_ob[s].inserted = 1;
    }
    ob_unlock();
    if (s < 0) {
        g_eax = 0xC000000Du;                 /* STATUS_INVALID_PARAMETER: not ours */
        return;
    }
    if (taken) {
        ob_deref(body);
        g_eax = 0xC0000035u;                 /* STATUS_OBJECT_NAME_COLLISION */
        return;
    }
    if (oa && (BRIDGE_MEM32(oa + 8) & 0x10u))   /* OBJ_PERMANENT */
        OB_FLAGS(body) |= OB_PERMANENT;
    InterlockedExchangeAdd(&OB_PTRS(body), (LONG)bias);
    if (!handle_va) {
        g_eax = 0;
        return;
    }
    token = ob_new_token(body);
    if (!token) {
        g_eax = 0xC000009Au;
        return;
    }
    BRIDGE_MEM32(handle_va) = token;
    g_eax = 0;
}

/* --- ObMakeTemporaryObject (ordinal 242, 1 arg = 4 bytes)
 * Clears the permanent flag; with no handle left the name goes at once. */
static void bridge_ObMakeTemporaryObject(void)
{
    uint32_t body = STACK_ARG(0);

    if (ob_is(body)) {
        OB_FLAGS(body) &= ~OB_PERMANENT;
        if (OB_HANDLES(body) <= 0)
            ob_unname(body);
    }
    g_eax = 0;
}

/* --- ObOpenObjectByName (ordinal 243, 4 args = 16 bytes)
 * NTSTATUS ObOpenObjectByName(POBJECT_ATTRIBUTES ObjectAttributes,
 *                             POBJECT_TYPE ObjectType, PVOID ParseContext,
 *                             PHANDLE Handle);
 * A directory of the namespace above, or an object ObInsertObject named.
 * Device and file names are NtOpenFile's business and are not parsed here. */
static void bridge_ObOpenObjectByName(void)
{
    uint32_t type = STACK_ARG(1), handle_va = STACK_ARG(3), body, token;
    char full[OB_NAME_MAX];
    const char *name = ob_attr_name(STACK_ARG(0), full, sizeof full);
    int dir;

    if (!handle_va || !bridge_buf_ok(handle_va, 4, "ObOpenObjectByName")) {
        g_eax = 0xC0000005u;
        return;
    }
    if (!name) {
        g_eax = 0xC0000033u;                 /* STATUS_OBJECT_NAME_INVALID */
        return;
    }
    dir = obdir_find(name);
    if (dir >= 0) {
        BRIDGE_MEM32(handle_va) = OBDIR_TOKEN | (uint32_t)dir;
        g_eax = 0;
        return;
    }
    body = ob_lookup(name);
    if (!body) {
        g_eax = 0xC0000034u;                 /* STATUS_OBJECT_NAME_NOT_FOUND */
        return;
    }
    if (!ob_type_ok(body, type)) {
        g_eax = 0xC0000024u;                 /* STATUS_OBJECT_TYPE_MISMATCH */
        return;
    }
    InterlockedIncrement(&OB_PTRS(body));
    token = ob_new_token(body);
    if (!token) {
        ob_deref(body);
        g_eax = 0xC000009Au;
        return;
    }
    BRIDGE_MEM32(handle_va) = token;
    g_eax = 0;
}

/* --- ObOpenObjectByPointer (ordinal 244, 3 args = 12 bytes)
 * NTSTATUS ObOpenObjectByPointer(PVOID Object, POBJECT_TYPE ObjectType,
 *                                PHANDLE Handle);
 * Three arguments (nxdk xboxkrnl.h); the table said five. An object of
 * ObCreateObject's gets a new handle; so does a guest thread object, a
 * duplicate of its host thread like PsCreateSystemThreadEx's. Anything else
 * has no handle to give: STATUS_OBJECT_TYPE_MISMATCH. */
static void bridge_ObOpenObjectByPointer(void)
{
    uint32_t body = STACK_ARG(0), type = STACK_ARG(1), handle_va = STACK_ARG(2), token;

    if (!handle_va || !bridge_buf_ok(handle_va, 4, "ObOpenObjectByPointer")) {
        g_eax = 0xC0000005u;
        return;
    }
    if (ob_is(body)) {
        if (!ob_type_ok(body, type)) {
            g_eax = 0xC0000024u;
            return;
        }
        InterlockedIncrement(&OB_PTRS(body));
        token = ob_new_token(body);
        if (!token) {
            ob_deref(body);
            g_eax = 0xC000009Au;
            return;
        }
        BRIDGE_MEM32(handle_va) = token;
        g_eax = 0;
        return;
    }
    {
        HANDLE th = ke_shadow_lookup(body), dup = NULL;
        int is_thread;
        thobj_lock();
        is_thread = thobj_slot(body) >= 0;
        thobj_unlock();
        if (is_thread && th && DuplicateHandle(GetCurrentProcess(), th, GetCurrentProcess(),
                                               &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            BRIDGE_MEM32(handle_va) = bridge_handle_token_ex(dup, 1);
            thobj_attach_token(BRIDGE_MEM32(handle_va), body);
            g_eax = 0;
            return;
        }
    }
    g_eax = 0xC0000024u;                     /* STATUS_OBJECT_TYPE_MISMATCH */
}

/* --- ObReferenceObjectByPointer (ordinal 248, 2 args = 8 bytes)
 * NTSTATUS ObReferenceObjectByPointer(PVOID Object, POBJECT_TYPE ObjectType);
 * Two arguments (nxdk xboxkrnl.h); the table said one. One more reference
 * to an object of ObCreateObject's or a guest thread object; other objects
 * are not counted here (ObfDereferenceObject is a no-op for them too). */
static void bridge_ObReferenceObjectByPointer(void)
{
    uint32_t body = STACK_ARG(0), type = STACK_ARG(1);

    if (ob_is(body)) {
        if (!ob_type_ok(body, type)) {
            g_eax = 0xC0000024u;
            return;
        }
        InterlockedIncrement(&OB_PTRS(body));
    } else {
        thobj_ref(body);                     /* a no-op unless a thread object */
    }
    g_eax = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Ps (254, 256, 257)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- PsCreateSystemThread (ordinal 254, 7 args = 28 bytes)
 * NTSTATUS PsCreateSystemThread(PHANDLE ThreadHandle, ULONG KernelStackSize,
 *                               ULONG TlsDataSize, PULONG ThreadId,
 *                               PVOID StartContext1, PVOID StartContext2,
 *                               PXBOX_SYSTEM_ROUTINE StartRoutine);
 * Mirror of PsCreateSystemThreadEx (ordinal 255) with the ThreadExtraSize
 * argument dropped: same first-call-inline / worker-spawn behaviour.
 * Requires bridge_spawn_thread/bridge_run_thread_inline/g_thread_mode from
 * Section A of kernel_bridge.c. */
static void bridge_PsCreateSystemThread(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t xbox_thread_id_ptr = STACK_ARG(3);
    uint32_t start_context1  = STACK_ARG(4);
    uint32_t start_context2  = STACK_ARG(5);
    uint32_t start_routine   = STACK_ARG(6);
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);

    g_thread_call_count++;
    bridge_discover_thread_ctx(start_routine, start_context1);

    /* Fake handle until the worker path can write a real one. */
    if (xbox_handle_ptr)
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0002;
    if (xbox_thread_id_ptr) {   /* see PsCreateSystemThreadEx */
        uint32_t kt = thobj_current();
        BRIDGE_MEM32(xbox_thread_id_ptr) = kt ? BRIDGE_MEM32(kt + 0x12C) : 0;
    }

    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn)
            fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                xbox_DpcMarkGuestThread();
                xbox_GuestThreadPin("guest-main");
                recomp_status_thread_started();   /* the title's main thread, run here (XBA-79) */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                {   /* the main thread holds the guest CPU: see PsCreateSystemThreadEx */
                    int took = xbox_GuestCpuEnterCallback();
                    fn();
                    xbox_GuestCpuLeaveCallback(took);
                }
                g_esp += 12;
            } else {
                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    g_eax = 0;
                    return;
                }
                stack_top = xbox_AllocThreadStack();
                if (!stack_top
                    || !bridge_spawn_guest_thread(fn, start_context1,
                                                  start_context2, stack_top,
                                                  xbox_handle_ptr, 0, 0,
                                                  xbox_thread_id_ptr)) {
                    /* see PsCreateSystemThreadEx */
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThread: no %s"
                            " for worker 0x%08X: STATUS_INSUFFICIENT_RESOURCES\n",
                            stack_top ? "host thread" : "stack", start_routine);
                    fflush(stderr);
                    xbox_FreeThreadStack(stack_top);
                    if (xbox_handle_ptr)
                        BRIDGE_MEM32(xbox_handle_ptr) = 0;
                    g_eax = 0xC000009Au;
                    return;
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThread: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
            recomp_discovery_thread(start_routine, start_context1);
        }
    }
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* --- PsQueryStatistics (ordinal 256, 1 arg = 4 bytes)
 * Zeroes the caller's statistics structure. */
static void bridge_PsQueryStatistics(void)
{
    uint32_t stats_va = STACK_ARG(0);
    if (stats_va)
        memset(XBOX_TO_NATIVE(stats_va), 0, 0x80);
    g_eax = 0;
}

/* --- PsSetCreateThreadNotifyRoutine (ordinal 257, 1 arg = 4 bytes)
 * NTSTATUS PsSetCreateThreadNotifyRoutine(PCREATE_THREAD_NOTIFY_ROUTINE);
 * One argument (nxdk xboxkrnl.h); the table said two. Up to eight routines
 * (STATUS_INSUFFICIENT_RESOURCES past them), each called as
 * NotifyRoutine(PETHREAD Thread, HANDLE ThreadId, BOOLEAN Create): with
 * Create TRUE on the creating thread when PsCreateSystemThreadEx makes a
 * thread, before it runs; with FALSE on the thread itself as it ends
 * (XBA-114). The thread is its guest object, which is also its ThreadId
 * here. RECOMP_THREAD_NOTIFY=0 registers them without ever calling one, for
 * A/B. */
#define THREAD_NOTIFY_MAX 8
static uint32_t s_thread_notify[THREAD_NOTIFY_MAX];
static volatile LONG s_thread_notify_count;

static int thread_notify_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_THREAD_NOTIFY");
        on = !(e && *e == '0');
    }
    return on && s_thread_notify_count > 0;
}

static void thread_notify_run(uint32_t thread, int create)
{
    uint32_t routines[THREAD_NOTIFY_MAX];
    int i, n = 0;

    if (!thread || !thread_notify_on())
        return;
    for (i = 0; i < THREAD_NOTIFY_MAX; i++)
        if (s_thread_notify[i])
            routines[n++] = s_thread_notify[i];
    for (i = 0; i < n; i++) {
        uint32_t a[3] = { thread, thread, create ? 1u : 0u };
        bridge_call_guest(routines[i], a, 3);
    }
}

static void bridge_PsSetCreateThreadNotifyRoutine(void)
{
    uint32_t routine = STACK_ARG(0);
    int i;

    for (i = 0; i < THREAD_NOTIFY_MAX; i++)
        if (InterlockedCompareExchange((volatile LONG *)&s_thread_notify[i],
                                       (LONG)routine, 0) == 0) {
            InterlockedIncrement(&s_thread_notify_count);
            g_eax = 0;
            return;
        }
    g_eax = 0xC000009Au;                     /* STATUS_INSUFFICIENT_RESOURCES */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rtl (281, 282, 283, 285, 286, 303, 361, 362, 363, 364)
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- RtlExtendedIntegerMultiply (ordinal 281, 3 args = 12 bytes)
 * LARGE_INTEGER RtlExtendedIntegerMultiply(LARGE_INTEGER Multiplicand,
 *                                          LONG Multiplier);
 * Multiplicand.Low (4) + Multiplicand.High (4) + Multiplier (4). C's int64
 * multiply wraps mod 2^64, which is exactly what the x86 imul pair produces
 * for the low 64 bits the API returns. */
static void bridge_RtlExtendedIntegerMultiply(void)
{
    int32_t mult = (int32_t)STACK_ARG(2);
    int64_t multi = ((int64_t)(int32_t)STACK_ARG(1) << 32)
                  | (uint32_t)STACK_ARG(0);
    int64_t result = multi * (int64_t)mult;

    g_eax = (uint32_t)result;
    g_edx = (uint32_t)((uint64_t)result >> 32);
}

/* --- RtlExtendedLargeIntegerDivide (ordinal 282, 4 args = 16 bytes)
 * LARGE_INTEGER RtlExtendedLargeIntegerDivide(LARGE_INTEGER Dividend,
 *                                             ULONG Divisor,
 *                                             PULONG Remainder);
 * Dividend.Low (4) + Dividend.High (4) + Divisor (4) + Remainder-ptr (4). */
static void bridge_RtlExtendedLargeIntegerDivide(void)
{
    uint64_t dividend = ((uint64_t)(uint32_t)STACK_ARG(1) << 32)
                      | (uint32_t)STACK_ARG(0);
    uint32_t divisor = STACK_ARG(2);
    uint32_t rem_ptr = STACK_ARG(3);
    uint32_t remainder;
    uint64_t quotient;

    if (divisor == 0) {
        /* Division by zero: report 0/*; Real hardware faults here. */
        if (rem_ptr)
            BRIDGE_MEM32(rem_ptr) = 0;
        g_eax = 0;
        g_edx = 0;
        return;
    }

    quotient  = dividend / divisor;
    remainder = (uint32_t)(dividend % divisor);

    if (rem_ptr)
        BRIDGE_MEM32(rem_ptr) = remainder;
    g_eax = (uint32_t)quotient;
    g_edx = (uint32_t)(quotient >> 32);
}

/* --- RtlExtendedMagicDivide (ordinal 283, 4 args = 16 bytes)
 * RtlExtendedMagicDivide(Dividend.Low, Dividend.High, Divisor, Remainder-ptr)
 * per the 16-byte stack layout. The precomputed magic multiplier is not
 * available, so a real division produces the identical quotient. */
static void bridge_RtlExtendedMagicDivide(void)
{
    uint64_t dividend = ((uint64_t)(uint32_t)STACK_ARG(1) << 32)
                      | (uint32_t)STACK_ARG(0);
    uint32_t divisor = STACK_ARG(2);
    uint32_t rem_ptr = STACK_ARG(3);
    uint32_t remainder;
    uint64_t quotient;

    if (divisor == 0) {
        if (rem_ptr)
            BRIDGE_MEM32(rem_ptr) = 0;
        g_eax = 0;
        g_edx = 0;
        return;
    }

    quotient  = dividend / divisor;
    remainder = (uint32_t)(dividend % divisor);

    if (rem_ptr)
        BRIDGE_MEM32(rem_ptr) = remainder;
    g_eax = (uint32_t)quotient;
    g_edx = (uint32_t)(quotient >> 32);
}

/* --- RtlFillMemoryUlong (ordinal 285, 3 args = 12 bytes)
 * VOID RtlFillMemoryUlong(PVOID Destination, ULONG Length, ULONG Pattern);
 * Length is multiples of 4 by contract. */
static void bridge_RtlFillMemoryUlong(void)
{
    volatile uint32_t *dst = (volatile uint32_t *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t length = STACK_ARG(1);
    uint32_t pattern = STACK_ARG(2);
    uint32_t i;

    if (!dst)
        return;
    for (i = 0; i < length / 4; i++)
        dst[i] = pattern;
    g_eax = 0;
}

/* --- RtlFreeAnsiString (ordinal 286, 1 arg = 4 bytes)
 * VOID RtlFreeAnsiString(PANSI_STRING AnsiString);
 * Frees AnsiString->Buffer (a guest-heap allocation) and clears the struct.
 * ANSI_STRING layout: +0 Length(USHORT), +2 MaximumLength(USHORT), +4 Buffer. */
static void bridge_RtlFreeAnsiString(void)
{
    uint32_t str_va = STACK_ARG(0);
    uint32_t buf;

    if (!str_va) {
        g_eax = 0;
        return;
    }
    buf = BRIDGE_MEM32(str_va + 4);
    if (buf)
        xbox_HeapFree(buf);
    BRIDGE_MEM16(str_va + 0) = 0;
    BRIDGE_MEM16(str_va + 2) = 0;
    BRIDGE_MEM32(str_va + 4) = 0;
    g_eax = 0;
}

/* --- RtlRaiseStatus (ordinal 303, 1 arg = 4 bytes) --- */
static void bridge_RtlRaiseStatus(void)
{
    uint32_t status = STACK_ARG(0);
    fprintf(stderr, "  [KERNEL] RtlRaiseStatus: 0x%08X (swallowed)\n", status);
    g_eax = 0;
}

/* --- Rtl* printf family (ordinals 361-364) ---
 *
 * int RtlSnprintf(char*, size_t count, const char* fmt, ...)
 * int RtlSprintf(char*, const char* fmt, ...)
 * int RtlVsnprintf(char*, size_t count, const char* fmt, va_list)
 * int RtlVsprintf(char*, const char* fmt, va_list)
 *
 * All four are __cdecl on the console, so the stdcall table pops nothing
 * ("caller cleans", like DbgPrint) and the varargs stay readable on the guest
 * stack while the bridge runs. The V* variants take an x86 va_list, which is
 * just a guest pointer to where the argument slots begin.
 *
 * The x64 CRT's vsnprintf cannot consume an x86 va_list, so these bridges used
 * to copy the format string verbatim ("%d" came out as the literal text) and
 * read the wrong fixed arguments for the two non-counted variants: RtlSprintf's
 * "count" was the format pointer, and the vararg list started one slot early.
 * This walks the format itself and formats each conversion with the host CRT,
 * reading 32-bit argument slots from guest memory. A 64-bit argument (a
 * double, an __int64) is two consecutive slots: x86 cdecl aligns stack
 * arguments to 4 bytes, never to 8.
 */

/* Guest cursor over the variadic argument slots. */
static uint32_t bridge_va_next32(uint32_t *pos)
{
    uint32_t v = BRIDGE_MEM32(*pos);
    *pos += 4;
    return v;
}

static uint64_t bridge_va_next64(uint32_t *pos)
{
    uint32_t lo = BRIDGE_MEM32(*pos);
    uint32_t hi = BRIDGE_MEM32(*pos + 4);
    *pos += 8;
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* Format fmt, pulling varargs from guest memory at `pos`, into out (at most
 * cap-1 chars, NUL-terminated). Returns the number of characters written. */
static int bridge_format_printf(char *out, size_t cap, const char *fmt,
                                uint32_t pos)
{
    size_t used = 0;
    size_t i = 0;

    if (!out || cap == 0)
        return 0;

    while (fmt[i] && used + 1 < cap) {
        if (fmt[i] != '%') {
            out[used++] = fmt[i++];
            continue;
        }
        i++;
        if (fmt[i] == '%') {
            out[used++] = '%';
            i++;
            continue;
        }

        {
            char flags[8];
            int  nflags = 0;
            char conv[48];         /* %, 7 flags, width, '.', precision, "ll", the specifier: 35 at most */
            char *cp = conv;
            int  width = 0, have_width = 0, width_star = 0;
            int  prec  = 0, have_prec  = 0, prec_star  = 0;
            char len = 0;          /* 0 none, 1 h, 2 hh, 3 l, 4 ll */
            char spec;
            char tmp[512];
            size_t tlen;

            while (fmt[i] && strchr("-+ 0#", fmt[i]) && nflags < 7)
                flags[nflags++] = fmt[i++];
            flags[nflags] = 0;

            if (fmt[i] == '*') { width_star = 1; have_width = 1; i++; }
            else if (fmt[i] >= '0' && fmt[i] <= '9') {
                have_width = 1;
                while (fmt[i] >= '0' && fmt[i] <= '9') {
                    if (width < 100000)                /* tmp is 512 bytes anyway; no overflow */
                        width = width * 10 + (fmt[i] - '0');
                    i++;
                }
            }

            if (fmt[i] == '.') {
                i++;
                have_prec = 1;
                if (fmt[i] == '*') { prec_star = 1; i++; }
                else while (fmt[i] >= '0' && fmt[i] <= '9') {
                    if (prec < 100000)
                        prec = prec * 10 + (fmt[i] - '0');
                    i++;
                }
            }

            if (fmt[i] == 'h') { len = 1; i++; if (fmt[i] == 'h') { len = 2; i++; } }
            else if (fmt[i] == 'l') { len = 3; i++; if (fmt[i] == 'l') { len = 4; i++; } }
            else if (fmt[i] == 'q') { len = 4; i++; }
            else if (fmt[i] == 'I' && fmt[i+1] == '6' && fmt[i+2] == '4') {
                len = 4; i += 3;
            }

            spec = fmt[i];
            if (!spec)
                break;
            i++;

            *cp++ = '%';
            if (nflags) { memcpy(cp, flags, (size_t)nflags); cp += nflags; }

            if (width_star) {
                int w = (int32_t)bridge_va_next32(&pos);
                if (w < 0) { *cp++ = '-'; w = -w; }
                cp += sprintf(cp, "%d", w);
            } else if (have_width) {
                cp += sprintf(cp, "%d", width);
            }
            if (have_prec) {
                *cp++ = '.';
                if (prec_star) {
                    int p = (int32_t)bridge_va_next32(&pos);
                    if (p < 0) p = 0;
                    cp += sprintf(cp, "%d", p);
                } else {
                    cp += sprintf(cp, "%d", prec);
                }
            }
            /* Single 'l' is a 32-bit long on the console; the host long is 32
             * bits too, so 'l' is passed through for the d/i/u/o/x/X paths. */
            if (len == 2) { *cp++ = 'h'; *cp++ = 'h'; }
            else if (len == 1) { *cp++ = 'h'; }
            else if (len == 4) { *cp++ = 'l'; *cp++ = 'l'; }

            switch (spec) {
            case 'd': case 'i': {
                if (len == 4) {
                    long long v = (long long)bridge_va_next64(&pos);
                    *cp++ = 'd'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    int v = (int)(int32_t)bridge_va_next32(&pos);
                    *cp++ = 'd'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'u': {
                if (len == 4) {
                    unsigned long long v = bridge_va_next64(&pos);
                    *cp++ = 'u'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    unsigned int v = bridge_va_next32(&pos);
                    *cp++ = 'u'; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'o': case 'x': case 'X': {
                if (len == 4) {
                    unsigned long long v = bridge_va_next64(&pos);
                    *cp++ = spec; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                } else {
                    unsigned int v = bridge_va_next32(&pos);
                    *cp++ = spec; *cp = 0;
                    snprintf(tmp, sizeof tmp, conv, v);
                }
                break;
            }
            case 'c': {
                int v = (int)bridge_va_next32(&pos);
                *cp++ = 'c'; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, v);
                break;
            }
            case 's': {
                uint32_t sva = bridge_va_next32(&pos);
                const char *gs = sva ? (const char *)XBOX_TO_NATIVE(sva)
                                     : "(null)";
                *cp++ = 's'; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, gs);
                break;
            }
            case 'p': {
                /* 32-bit pointer as padded lower-case hex, like the x86 CRT */
                unsigned int v = bridge_va_next32(&pos);
                const char *pfx = strchr(flags, '#') ? "%#08x" : "%08x";
                snprintf(tmp, sizeof tmp, pfx, v);
                break;
            }
            case 'n': {
                uint32_t tva = bridge_va_next32(&pos);
                if (tva)
                    BRIDGE_MEM32(tva) = (uint32_t)used;   /* int written */
                tlen = 0;
                break;
            }
            case 'f': case 'e': case 'g': case 'E': case 'G': {
                /* The argument's bits are the double (not an integer to convert). */
                uint64_t bits = bridge_va_next64(&pos);
                double v;
                memcpy(&v, &bits, sizeof v);
                *cp++ = spec; *cp = 0;
                snprintf(tmp, sizeof tmp, conv, v);
                break;
            }
            default:
                /* Unsupported conversion: keep '%' and the specifier */
                out[used++] = '%';
                out[used++] = spec;
                continue;
            }

            tlen = (spec == 'n') ? 0 : strlen(tmp);
            {
                size_t k;
                for (k = 0; k < tlen && used + 1 < cap; k++)
                    out[used++] = tmp[k];
            }
        }
    }

    if (used + 1 >= cap)
        used = cap - 1;
    out[used] = 0;
    return (int)used;
}

/* --- RtlSnprintf (ordinal 361, __cdecl, caller cleans) --- */
static void bridge_RtlSnprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t  count = STACK_ARG(1);
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(2));

    if (!dst || !fmt) {
        g_eax = 0;
        return;
    }
    /* Varargs start after buffer/count/format on the guest stack. */
    g_eax = bridge_format_printf(dst, count, fmt, g_esp + 12);
}

/* --- RtlSprintf (ordinal 362, __cdecl, caller cleans) --- */
static void bridge_RtlSprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(1));

    if (!dst || !fmt) {
        g_eax = 0;
        return;
    }
    /* Unbounded like the CRT's vsprintf; a wide cap prevents a wild write. */
    g_eax = bridge_format_printf(dst, 65536, fmt, g_esp + 8);
}

/* --- RtlVsnprintf (ordinal 363, __cdecl, caller cleans) --- */
static void bridge_RtlVsnprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    uint32_t  count = STACK_ARG(1);
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(2));
    uint32_t  list = STACK_ARG(3);   /* x86 va_list = guest pointer to args */

    if (!dst || !fmt || !list) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, count, fmt, list);
}

/* --- RtlVsprintf (ordinal 364, __cdecl, caller cleans) --- */
static void bridge_RtlVsprintf(void)
{
    char     *dst  = (char *)XBOX_TO_NATIVE(STACK_ARG(0));
    const char *fmt = (const char *)XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t  list = STACK_ARG(2);   /* x86 va_list = guest pointer to args */

    if (!dst || !fmt || !list) {
        g_eax = 0;
        return;
    }
    g_eax = bridge_format_printf(dst, 65536, fmt, list);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Port I/O (329, 330, 331, 332)
 *
 * The port argument is an I/O window the title mapped with MmMapIoSpace, so
 * it is an ordinary guest VA in the memory model. The READ variants copy the
 * requested element count from the mapped window into the caller's buffer;
 * the WRITE variants are no-ops (mirroring the existing 333/334 bridges).
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* --- READ_PORT_BUFFER_UCHAR (ordinal 329, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_UCHAR(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count);
    g_eax = 0;
}

/* --- READ_PORT_BUFFER_USHORT (ordinal 330, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_USHORT(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count * sizeof(uint16_t));
    g_eax = 0;
}

/* --- READ_PORT_BUFFER_ULONG (ordinal 331, 3 args = 12 bytes) --- */
static void bridge_READ_PORT_BUFFER_ULONG(void)
{
    void *src = XBOX_TO_NATIVE(STACK_ARG(0));
    void *dst = XBOX_TO_NATIVE(STACK_ARG(1));
    uint32_t count = STACK_ARG(2);

    if (src && dst)
        memcpy(dst, src, count * sizeof(uint32_t));
    g_eax = 0;
}

/* --- WRITE_PORT_BUFFER_UCHAR (ordinal 332, 3 args = 12 bytes) --- */
static void bridge_WRITE_PORT_BUFFER_UCHAR(void)
{
    (void)STACK_ARG(0);
    (void)STACK_ARG(1);
    (void)STACK_ARG(2);
    g_eax = 0;
}

static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ── Display / AV ── */
    case   1: return  0;  /* AvGetSavedDataAddress (void) */
    case   2: return 16;  /* AvSendTVEncoderOption (4) */
    case   3: return 24;  /* AvSetDisplayMode (6) */
    case   4: return  4;  /* AvSetSavedDataAddress (1) */
    case   5: return  0;  /* DbgBreakPoint (void) */
    case   8: return  0;  /* DbgPrint - __cdecl varargs, caller cleans */
    case   9: return  8;  /* HalReadSMCTrayState (2) */
    case  14: return  4;  /* ExAllocatePool (1) */
    case  15: return  8;  /* ExAllocatePoolWithTag (2) */
    case  17: return  4;  /* ExFreePool (1) */
    case  23: return  4;  /* ExQueryPoolBlockSize (1) */
    case  24: return 20;  /* ExQueryNonVolatileSetting (5) */
    case  29: return 16;  /* ExSaveNonVolatileSetting (4) */
    case  35: return  0;  /* FscGetCacheSize (void) */
    case  37: return  4;  /* FscSetCacheSize (1) */
    case  38: return  4;  /* HalClearSoftwareInterrupt (1) */
    case  39: return  8;  /* HalDisableSystemInterrupt (2) */
    case  42: return  0;  /* HalDiskSerialNumber - data export */
    case  44: return  8;  /* HalGetInterruptVector (2) */
    case  47: return  8;  /* HalRegisterShutdownNotification (2) */
    case  46: return 24;  /* HalReadWritePCISpace (6) */
    case  45: return 16;  /* HalReadSMBusValue (4) */
    case  50: return 16;  /* HalWriteSMBusValue (4) */
    case  48: return  4;  /* HalRequestSoftwareInterrupt (1) */
    case  49: return  4;  /* HalReturnToFirmware (1) */
    case  61: return 36;  /* IoBuildDeviceIoControlRequest (9) */
    case  62: return 28;  /* IoBuildSynchronousFsdRequest (7) */
    case  65: return 24;  /* IoCreateDevice (6) */
    case  66: return 40;  /* IoCreateFile (10) */
    case  67: return  8;  /* IoCreateSymbolicLink (2) */
    case  68: return  4;  /* IoDeleteDevice (1) */
    case  69: return  4;  /* IoDeleteSymbolicLink (1) */
    case  73: return 12;  /* IoInitializeIrp (3) */
    case  74: return  8;  /* IoInvalidDeviceRequest (2) */
    case  79: return 20;  /* IoSetIoCompletion (5) */
    case  81: return  8;  /* IoStartNextPacket (2) */
    case  82: return 12;  /* IoStartNextPacketByKey (3) */
    case  83: return 16;  /* IoStartPacket (4) */
    case  84: return 32;  /* IoSynchronousDeviceIoControlRequest (8) */
    case  85: return 20;  /* IoSynchronousFsdRequest (5) */
    case  86: return  0;  /* IofCallDriver (fastcall: args in ecx/edx) */
    case  87: return  0;  /* IofCompleteRequest (fastcall: args in ecx/edx) */
    /* Missing this entry cost the Xbox Dashboard its whole boot. Ordinal 91 has
     * no bridge, so the generic stub ran -- and with no arg count it left the
     * one pushed argument on the guest stack. The caller (sub_00032859) then
     * ran `pop edi; pop esi; pop ebx` four bytes low and came back with its
     * registers rotated, which destroyed the XApp `this` pointer two frames up.
     * Its scene manager was never created, its scene never loaded, and it
     * returned to firmware -- reported as nothing more than "returning 0". */
    case  90: return  4;  /* IoDismountVolume (1) */
    case  91: return  4;  /* IoDismountVolumeByName (1) */
    case  93: return  8;  /* KeAlertThread (2) */
    case  95: return  4;  /* KeBugCheck (1) */
    case  96: return 20;  /* KeBugCheckEx (5) */
    case  97: return  4;  /* KeCancelTimer (1) */
    case  98: return  4;  /* KeConnectInterrupt (1) */
    case  99: return 12;  /* KeDelayExecutionThread (3) */
    case 100: return  4;  /* KeDisconnectInterrupt (1) */
    case 107: return 12;  /* KeInitializeDpc (3) */
    case 109: return 28;  /* KeInitializeInterrupt (7) */
    case 113: return  8;  /* KeInitializeTimerEx (2) */
    case 119: return 12;  /* KeInsertQueueDpc (3) */
    case 124: return  4;  /* KeQueryBasePriorityThread (1) */
    case 125: return  0;  /* KeQueryInterruptTime (void) */
    case 126: return  0;  /* KeQueryPerformanceCounter (void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency (void) */
    case 128: return  4;  /* KeQuerySystemTime (1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel (void) */
    case 137: return  4;  /* KeRemoveQueueDpc (1) */
    case 139: return  4;  /* KeRestoreFloatingPointState (1) */
    case 142: return  4;  /* KeSaveFloatingPointState (1) */
    case 143: return  8;  /* KeSetBasePriorityThread (2) */
    case 144: return  8;  /* KeSetDisableBoostThread (2) */
    case 145: return 12;  /* KeSetEvent (3) */
    case 149: return 16;  /* KeSetTimer (Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx (Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor (1) */
    case 153: return 12;  /* KeSynchronizeExecution (3) */
    case 158: return 32;  /* KeWaitForMultipleObjects (8) */
    case 159: return 20;  /* KeWaitForSingleObject (5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */
    case 165: return  4;  /* MmAllocateContiguousMemory (1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx (5) */
    case 167: return  8;  /* MmAllocateSystemMemory (2) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory (2) */
    case 169: return  8;  /* MmCreateKernelStack (2) */
    case 170: return  8;  /* MmDeleteKernelStack (2) */
    case 171: return  4;  /* MmFreeContiguousMemory (1) */
    case 172: return  8;  /* MmFreeSystemMemory (2) */
    case 173: return  4;  /* MmGetPhysicalAddress (1) */
    case 175: return 12;  /* MmLockUnlockBufferPages (3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage (2) */
    case 177: return 12;  /* MmMapIoSpace (3) */
    case 178: return 12;  /* MmPersistContiguousMemory (3) */
    case 179: return  4;  /* MmQueryAddressProtect (1) */
    case 180: return  4;  /* MmQueryAllocationSize (1) */
    case 181: return  4;  /* MmQueryStatistics (1) */
    case 182: return 12;  /* MmSetAddressProtect (3) */
    case 183: return  8;  /* MmUnmapIoSpace (2) */
    case 184: return 20;  /* NtAllocateVirtualMemory (5) */
    case 185: return  8;  /* NtCancelTimer (2) */
    case 186: return  4;  /* NtClearEvent (1) */
    case 187: return  4;  /* NtClose (1) */
    case 188: return  8;  /* NtCreateDirectoryObject (2) */
    case 189: return 16;  /* NtCreateEvent (4) */
    case 190: return 36;  /* NtCreateFile (9) */
    case 191: return 16;  /* NtCreateIoCompletion (4) */
    case 192: return 12;  /* NtCreateMutant (3) */
    case 193: return 16;  /* NtCreateSemaphore (4) */
    case 194: return 12;  /* NtCreateTimer (3) */
    case 195: return  4;  /* NtDeleteFile (1) */
    case 196: return 40;  /* NtDeviceIoControlFile (10) */
    case 197: return 12;  /* NtDuplicateObject (3) */
    case 198: return  8;  /* NtFlushBuffersFile (2) */
    case 199: return 12;  /* NtFreeVirtualMemory (3) */
    case 200: return 40;  /* NtFsControlFile (10) */
    case 202: return 24;  /* NtOpenFile (6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject (2) */
    case 204: return 16;  /* NtProtectVirtualMemory (4) */
    case 205: return  8;  /* NtPulseEvent (2) */
    case 206: return 20;  /* NtQueueApcThread (5) */
    case 207: return 40;  /* NtQueryDirectoryFile (10) */
    case 210: return  8;  /* NtQueryFullAttributesFile (2) */
    case 211: return 20;  /* NtQueryInformationFile (5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject (3) */
    case 217: return 8;   /* NtQueryVirtualMemory (2) -- Xbox takes
                             BaseAddress and Info only, not NT's four */
    case 218: return 20;  /* NtQueryVolumeInformationFile (5) */
    case 219: return 32;  /* NtReadFile (8) */
    case 220: return 32;  /* NtReadFileScatter (8) */
    case 221: return  8;  /* NtReleaseMutant (2) */
    case 222: return 12;  /* NtReleaseSemaphore (3) */
    case 223: return 20;  /* NtRemoveIoCompletion (5) */
    case 224: return  8;  /* NtResumeThread (2) */
    case 225: return  8;  /* NtSetEvent (2) */
    case 226: return 20;  /* NtSetInformationFile (5) */
    case 227: return 20;  /* NtSetIoCompletion (5) */
    case 228: return  8;  /* NtSetSystemTime (2) */
    case 229: return 32;  /* NtSetTimerEx (8) */
    case 230: return 20;  /* NtSignalAndWaitForSingleObjectEx (5) */
    case 231: return  8;  /* NtSuspendThread (2) */
    case 232: return 12;  /* NtUserIoApcDispatcher (3) */
    case 233: return 12;  /* NtWaitForSingleObject (3) */
    case 234: return 16;  /* NtWaitForSingleObjectEx (4) */
    case 235: return 24;  /* NtWaitForMultipleObjectsEx (6) */
    case 236: return 32;  /* NtWriteFile (8) */
    case 237: return 32;  /* NtWriteFileGather (8) */
    case 238: return  0;  /* NtYieldExecution (void) */
    case 243: return 16;  /* ObOpenObjectByName (4) */
    case 247: return 20;  /* ObReferenceObjectByName (5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */
    case 251: return  0;  /* ObfReferenceObject (fastcall: arg in ecx) */
    case 252: return  4;  /* PhyGetLinkState (1) */
    case 253: return  8;  /* PhyInitialize (2) */
    case 255: return 40;  /* PsCreateSystemThreadEx (10) */
    case 258: return  4;  /* PsTerminateSystemThread (1) */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString (3) */
    case 268: return 12;  /* RtlCompareMemory (3) */
    case 269: return 12;  /* RtlCompareMemoryUlong (3) */
    case 270: return 12;  /* RtlCompareString (3) */
    case 261: return  8;  /* RtlAppendStringToString (2) */
    case 262: return  8;  /* RtlAppendUnicodeStringToString (2) */
    case 263: return  8;  /* RtlAppendUnicodeToString (2) */
    case 264: return 12;  /* RtlAssert (3) */
    case 265: return  4;  /* RtlCaptureContext (1) */
    case 266: return 16;  /* RtlCaptureStackBackTrace (4): nxdk xboxkrnl.h */
    case 267: return 12;  /* RtlCharToInteger (3) */
    case 271: return 12;  /* RtlCompareUnicodeString (3) */
    case 272: return  8;  /* RtlCopyString (2) */
    case 273: return  8;  /* RtlCopyUnicodeString (2) */
    case 274: return  8;  /* RtlCreateUnicodeString (2) */
    case 275: return  4;  /* RtlDowncaseUnicodeChar (1) */
    case 276: return 12;  /* RtlDowncaseUnicodeString (3) */
    case 278: return  4;  /* RtlEnterCriticalSectionAndRegion (1) */
    case 280: return 12;  /* RtlEqualUnicodeString (3) */
    case 284: return 12;  /* RtlFillMemory (3) */
    case 287: return  4;  /* RtlFreeUnicodeString (1) */
    case 288: return  8;  /* RtlGetCallersAddress (2) */
    case 292: return 16;  /* RtlIntegerToChar (4) */
    case 293: return 12;  /* RtlIntegerToUnicodeString (3) */
    case 295: return  4;  /* RtlLeaveCriticalSectionAndRegion (1) */
    case 296: return  4;  /* RtlLowerChar (1) */
    case 297: return  8;  /* RtlMapGenericMask (2) */
    case 298: return 12;  /* RtlMoveMemory (3) */
    case 299: return 20;  /* RtlMultiByteToUnicodeN (5) */
    case 300: return 12;  /* RtlMultiByteToUnicodeSize (3) */
    case 306: return  4;  /* RtlTryEnterCriticalSection (1) */
    case 307: return  4;  /* RtlUlongByteSwap (1) */
    case 309: return 12;  /* RtlUnicodeStringToInteger (3) */
    case 310: return 20;  /* RtlUnicodeToMultiByteN (5) */
    case 311: return 12;  /* RtlUnicodeToMultiByteSize (3) */
    case 313: return  4;  /* RtlUpcaseUnicodeChar (1) */
    case 314: return 12;  /* RtlUpcaseUnicodeString (3) */
    case 315: return 20;  /* RtlUpcaseUnicodeToMultiByteN (5) */
    case 316: return  4;  /* RtlUpperChar (1) */
    case 317: return  8;  /* RtlUpperString (2) */
    case 318: return  4;  /* RtlUshortByteSwap (1) */
    case 319: return 12;  /* RtlWalkFrameChain (3) */
    case 320: return  8;  /* RtlZeroMemory (2) */
    case 321: return  0;  /* XboxEEPROMKey (void) */
    case 277: return  4;  /* RtlEnterCriticalSection (1) */
    case 279: return 12;  /* RtlEqualString (3) */
    case 285: return 12;  /* RtlFillMemoryUlong (3) */
    case 286: return  4;  /* RtlFreeAnsiString (1) */
    case 289: return  8;  /* RtlInitAnsiString (2) */
    case 290: return  8;  /* RtlInitUnicodeString (2) */
    case 291: return  4;  /* RtlInitializeCriticalSection (1) */
    case 294: return  4;  /* RtlLeaveCriticalSection (1) */
    case 301: return  4;  /* RtlNtStatusToDosError (1) */
    case 302: return  4;  /* RtlRaiseException (1) */
    case 304: return  8;  /* RtlTimeFieldsToTime (2) */
    case 305: return  8;  /* RtlTimeToTimeFields (2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString (3) */
    case 312: return 16;  /* RtlUnwind (4) */
    case 327: return  4;  /* XeLoadSection (1) */
    case 328: return  4;  /* XeUnloadSection (1) */
    case 333: return 12;  /* WRITE_PORT_BUFFER_USHORT (3) */
    case 334: return 12;  /* WRITE_PORT_BUFFER_ULONG (3) */
    case 335: return  4;  /* XcSHAInit (1) */
    case 336: return 12;  /* XcSHAUpdate (3) */
    case 337: return  8;  /* XcSHAFinal (2) */
    case 338: return 12;  /* XcRC4Key (3) */
    case 339: return 12;  /* XcRC4Crypt (3) */
    case 340: return 28;  /* XcHMAC (7) */
    case 341: return 12;  /* XcPKEncPublic (3) */
    case 342: return 12;  /* XcPKDecPrivate (3) */
    case 343: return  4;  /* XcPKGetKeyLen (1) */
    case 344: return 12;  /* XcVerifyPKCS1Signature (3) */
    case 345: return 20;  /* XcModExp (5) */
    case 346: return  8;  /* XcDESKeyParity (2) */
    case 347: return 12;  /* XcKeyTable (3) */
    case 348: return 20;  /* XcBlockCrypt (5) */
    case 349: return 28;  /* XcBlockCryptCBC (7) */
    case 350: return  8;  /* XcCryptService (2) */
    case 351: return  8;  /* XcUpdateCrypto (2) */
    case 352: return 12;  /* RtlRip (3) */
    case 358: return  0;  /* HalIsResetOrShutdownPending (void) */
    case 359: return  4;  /* IoMarkIrpMustComplete (1) */

    /* ── Unknown stubs ── */

    /* ── Pool Allocator ── */
    /* ordinal 16 is the ExEventObjectType data export; see
     * kernel_thunks.c, which points its thunk at kernel data.
     * 17 is ExFreePool and is a real function. */

    /* ── HAL ── */

    /* ── I/O Manager ── */
    /* ordinal 64 is the IoCompletionObjectType data export; see
     * kernel_thunks.c. 65 is IoCreateDevice, a real function. */
    /* case  71: DATA export - IoDeviceObjectType */

    /* ── Kernel Synchronization ── */
    /* case 156: DATA export - KeTickCount */

    /* ── Launch Data ── */
    /* case 164: DATA export - LaunchDataPage */

    /* ── Memory Management ── */

    /* ── NT Virtual Memory ── */

    /* ── NT File I/O & Handle ── */

    /* ── Object Manager ── */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 360: return  0;  /* HalInitiateShutdown (void) */

    /* ── Network / PHY ── */

    /* ── Threading ── */
    /* case 259: DATA export - PsThreadObjectType */

    /* ── Runtime Library ── */

    /* ── Xbox Identity (data exports) ── */
    /* cases 322-328, 355-357: DATA exports */

    /* ── Port I/O ── */

    /* ── Crypto ── */

    /* Data exports */
    case 102: return 0;  /* MmGlobalData (void/data) */
    case 120: return 0;  /* KeInterruptTime (void/data) */
    case 154: return 0;  /* KeSystemTime (void/data) */
    case 240: return 0;  /* ObDirectoryObjectType (void/data) */
    case 245: return 0;  /* ObpObjectHandleTable (void/data) */
    case 249: return 0;  /* ObSymbolicLinkObjectType (void/data) */

    /* Dbg */
    case   6: return  4;  /* DbgBreakPointWithStatus (1) */
    case   7: return 12;  /* DbgLoadImageSymbols (3) */
    case  10: return  8;  /* DbgPrompt (2) */
    case  11: return 12;  /* DbgUnLoadImageSymbols (3) */

    /* Ex */
    case  12: return  4;  /* ExAcquireReadWriteLockExclusive (1) */
    case  13: return  4;  /* ExAcquireReadWriteLockShared (1) */
    case  18: return  4;  /* ExInitializeReadWriteLock (1) */
    case  19: return 12;  /* ExInterlockedAddLargeInteger (3) */
    case  20: return  8;  /* ExInterlockedAddLargeStatistic (2) */
    case  21: return 16;  /* ExInterlockedCompareExchange64 (4) */
    case  25: return 12;  /* ExReadWriteRefurbInfo (3) */
    case  26: return  4;  /* ExRaiseException (1) */
    case  27: return  4;  /* ExRaiseStatus (1) */
    case  28: return  4;  /* ExReleaseReadWriteLock (1) */

    /* ExfInterlocked */
    case  32: return  8;  /* ExfInterlockedInsertHeadList (2) */
    case  33: return  8;  /* ExfInterlockedInsertTailList (2) */
    case  34: return  4;  /* ExfInterlockedRemoveHeadList (1) */

    /* Fsc */
    case  36: return  0;  /* FscInvalidateIdleBlocks (void) */

    /* Hal */
    case  43: return  8;  /* HalEnableSystemInterrupt (2) */
    case 365: return  0;  /* HalEnableSecureTrayEject (void) */
    case 366: return  4;  /* HalWriteSMCScratchRegister (1) */

    /* Interlocked (fastcall, args in ecx/edx) */
    case  51: return  0;  /* InterlockedCompareExchange (fastcall) */
    case  52: return  0;  /* InterlockedDecrement (fastcall) */
    case  53: return  0;  /* InterlockedIncrement (fastcall) */
    case  54: return  0;  /* InterlockedExchange (fastcall) */
    case  55: return  0;  /* InterlockedExchangeAdd (fastcall) */
    case  56: return  0;  /* InterlockedFlushSList (fastcall) */
    case  57: return  0;  /* InterlockedPopEntrySList (fastcall) */
    case  58: return  0;  /* InterlockedPushEntrySList (fastcall) */

    /* Io */
    case  59: return  4;  /* IoAllocateIrp (1): nxdk xboxkrnl.h */
    case  60: return 24;  /* IoBuildAsynchronousFsdRequest (6): nxdk xboxkrnl.h */
    case  63: return 20;  /* IoCheckShareAccess (5) */
    case  72: return  4;  /* IoFreeIrp (1) */
    case  75: return 20;  /* IoQueryFileInformation (5) */
    case  76: return 20;  /* IoQueryVolumeInformation (5) */
    case  77: return  4;  /* IoQueueThreadIrp (1) */
    case  78: return  8;  /* IoRemoveShareAccess (2) */
    case  80: return 16;  /* IoSetShareAccess (4): nxdk xboxkrnl.h */

    /* Kd */
    case  88: return  0;  /* KdDebuggerEnabled (void) */
    case  89: return  0;  /* KdDebuggerNotPresent (void) */

    /* Ke (new) */
    case  92: return  4;  /* KeAlertResumeThread (1) */
    case  94: return  8;  /* KeBoostPriorityThread (2) */
    case 101: return  0;  /* KeEnterCriticalRegion (void) */
    case 103: return  0;  /* KeGetCurrentIrql (void) */
    case 104: return  0;  /* KeGetCurrentThread (void) */
    case 105: return 28;  /* KeInitializeApc (7) */
    case 106: return  4;  /* KeInitializeDeviceQueue (1) */
    case 108: return 12;  /* KeInitializeEvent (3) */
    case 110: return  8;  /* KeInitializeMutant (2) */
    case 111: return  8;  /* KeInitializeQueue (2) */
    case 112: return 12;  /* KeInitializeSemaphore (3) */
    case 114: return 12;  /* KeInsertByKeyDeviceQueue (3) */
    case 115: return  8;  /* KeInsertDeviceQueue (2) */
    case 116: return  8;  /* KeInsertHeadQueue (2) */
    case 117: return  8;  /* KeInsertQueue (2) */
    case 118: return 16;  /* KeInsertQueueApc (4) */
    case 121: return  0;  /* KeIsExecutingDpc (void) */
    case 122: return  0;  /* KeLeaveCriticalRegion (void) */
    case 123: return 12;  /* KePulseEvent (3) */
    case 130: return  0;  /* KeRaiseIrqlToSynchLevel (void) */
    case 131: return 16;  /* KeReleaseMutant (4) */
    case 132: return 16;  /* KeReleaseSemaphore (4) */
    case 133: return  8;  /* KeRemoveByKeyDeviceQueue (2) */
    case 134: return  4;  /* KeRemoveDeviceQueue (1) */
    case 135: return  8;  /* KeRemoveEntryDeviceQueue (2) */
    case 136: return 12;  /* KeRemoveQueue (3) */
    case 138: return  4;  /* KeResetEvent (1) */
    case 140: return  4;  /* KeResumeThread (1) */
    case 141: return  4;  /* KeRundownQueue (1) */
    case 146: return  8;  /* KeSetEventBoostPriority (2) */
    case 147: return  8;  /* KeSetPriorityProcess (2) */
    case 148: return  8;  /* KeSetPriorityThread (2) */
    case 152: return  4;  /* KeSuspendThread (1) */
    case 155: return  4;  /* KeTestAlertThread (1) */
    case 162: return  0;  /* KiBugCheckData (void/data) */
    case 163: return  0;  /* KiUnlockDispatcherDatabase: fastcall, OldIrql in cl */

    /* Mm */
    case 174: return  4;  /* MmIsAddressValid (1) */
    case 374: return  8;  /* MmDbgAllocateMemory (2) */
    case 375: return  8;  /* MmDbgFreeMemory (2) */
    case 376: return  0;  /* MmDbgQueryAvailablePages (void) */
    case 377: return  8;  /* MmDbgReleaseAddress (2) */
    case 378: return  8;  /* MmDbgWriteCheck (2) */

    /* Nt */
    case 201: return  8;  /* NtOpenDirectoryObject (2): nxdk xboxkrnl.h */
    case 208: return 24;  /* NtQueryDirectoryObject (6): nxdk xboxkrnl.h */
    case 209: return  8;  /* NtQueryEvent (2): nxdk xboxkrnl.h */
    case 212: return  8;  /* NtQueryIoCompletion (2): nxdk xboxkrnl.h */
    case 213: return  8;  /* NtQueryMutant (2): nxdk xboxkrnl.h */
    case 214: return  8;  /* NtQuerySemaphore (2): nxdk xboxkrnl.h */
    case 216: return  8;  /* NtQueryTimer (2): nxdk xboxkrnl.h */

    /* Ob */
    case 239: return 16;  /* ObCreateObject (4): nxdk xboxkrnl.h */
    case 241: return 16;  /* ObInsertObject (4): nxdk xboxkrnl.h */
    case 242: return  4;  /* ObMakeTemporaryObject (1) */
    case 244: return 12;  /* ObOpenObjectByPointer (3): nxdk xboxkrnl.h */
    case 248: return  8;  /* ObReferenceObjectByPointer (2): nxdk xboxkrnl.h */

    /* Ps */
    case 254: return 28;  /* PsCreateSystemThread (7) */
    case 256: return  4;  /* PsQueryStatistics (1) */
    case 257: return  4;  /* PsSetCreateThreadNotifyRoutine (1): nxdk xboxkrnl.h */

    /* Rtl */
    case 281: return 12;  /* RtlExtendedIntegerMultiply (3) */
    case 282: return 16;  /* RtlExtendedLargeIntegerDivide (4) */
    case 283: return 16;  /* RtlExtendedMagicDivide (4) */
    case 303: return  4;  /* RtlRaiseStatus (1) */
    case 361: return  0;  /* RtlSnprintf - __cdecl, caller cleans */
    case 362: return  0;  /* RtlSprintf - __cdecl, caller cleans */
    case 363: return  0;  /* RtlVsnprintf - __cdecl, caller cleans */
    case 364: return  0;  /* RtlVsprintf - __cdecl, caller cleans */

    /* Port I/O */
    case 329: return 12;  /* READ_PORT_BUFFER_UCHAR (3) */
    case 330: return 12;  /* READ_PORT_BUFFER_USHORT (3) */
    case 331: return 12;  /* READ_PORT_BUFFER_ULONG (3) */
    case 332: return 12;  /* WRITE_PORT_BUFFER_UCHAR (3) */

    /* Not 0: a genuine zero-argument function and an ordinal nobody has
     * written down are both "pop nothing", but only one of them is a
     * problem, and the warning below could not tell them apart -- it
     * accused FscGetCacheSize, which really does take no arguments. */
    default:  return -1;  /* DATA exports or truly unknown */
    }
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 279: return bridge_RtlEqualString;
    case 289: return bridge_RtlInitAnsiString;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 312: return bridge_RtlUnwind;
    case 327: return bridge_XeLoadSection;
    case 328: return bridge_XeUnloadSection;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;
    case 215: return bridge_NtQuerySymbolicLinkObject;
    case 217: return bridge_NtQueryVirtualMemory;

    /* Pool */
    case  14: return bridge_ExAllocatePool;
    case  15: return bridge_ExAllocatePoolWithTag;
    case  23: return bridge_ExQueryPoolBlockSize;
    case 268: return bridge_RtlCompareMemory;
    case 269: return bridge_RtlCompareMemoryUlong;
    case  35: return bridge_FscGetCacheSize;
    case  37: return bridge_FscSetCacheSize;
    case  24: return bridge_ExQueryNonVolatileSetting;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    /* Wreckless asks for the AV pack during D3D device creation; unbridged it
     * read 0, which is "no pack connected". Writes one ULONG through a guest
     * pointer it is given and nothing else, and XBOX_TO_NATIVE turns a guest
     * NULL into a host NULL so xbox_AvSendTVEncoderOption's own !Result guard
     * still fires. */
    case   2: return bridge_AvSendTVEncoderOption;
    /* Reads a LARGE_INTEGER and fills a TIME_FIELDS, both at caller-supplied
     * guest addresses -- the case the NOT ROUTED note below names as fine. */
    case 305: return bridge_RtlTimeToTimeFields;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Timing */
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimerEx;

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 119: return bridge_KeInsertQueueDpc;
    case 137: return bridge_KeRemoveQueueDpc;
    case 153: return bridge_KeSynchronizeExecution;
    case 113: return bridge_KeInitializeTimerEx;

    /* NV2A interrupt plumbing */
    case  44: return bridge_HalGetInterruptVector;
    case  98: return bridge_KeConnectInterrupt;
    case 109: return bridge_KeInitializeInterrupt;
    case  47: return bridge_HalRegisterShutdownNotification;
    case 168: return bridge_MmClaimGpuInstanceMemory;

    /* Synchronization */
    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case  99: return bridge_KeDelayExecutionThread;
    case 179: return bridge_MmQueryAddressProtect;
    case 232: return bridge_NtUserIoApcDispatcher;
    case  95: return bridge_KeBugCheck;
    case  96: return bridge_KeBugCheckEx;
    case 186: return bridge_NtClearEvent;
    case 205: return bridge_NtPulseEvent;
    case 225: return bridge_NtSetEvent;
    case 233: return bridge_NtWaitForSingleObject;
    case 234: return bridge_NtWaitForSingleObjectEx;
    case 235: return bridge_NtWaitForMultipleObjectsEx;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case   9: return bridge_HalReadSMCTrayState;
    case  49: return bridge_HalReturnToFirmware;

    /* Display */
    case   3: return bridge_AvSetDisplayMode;

    /* I/O */
    case  66: return bridge_IoCreateFile;
    case  65: return bridge_IoCreateDevice;
    case  67: return bridge_IoCreateSymbolicLink;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;

    /* Section B: per-ordinal bridges (261-321), defined in the splice above. */
    case 261: return bridge_RtlAppendStringToString;
    case 262: return bridge_RtlAppendUnicodeStringToString;
    case 263: return bridge_RtlAppendUnicodeToString;
    case 264: return bridge_RtlAssert;
    case 265: return bridge_RtlCaptureContext;
    case 266: return bridge_RtlCaptureStackBackTrace;
    case 267: return bridge_RtlCharToInteger;
    case 270: return bridge_RtlCompareString;
    case 271: return bridge_RtlCompareUnicodeString;
    case 272: return bridge_RtlCopyString;
    case 273: return bridge_RtlCopyUnicodeString;
    case 274: return bridge_RtlCreateUnicodeString;
    case 275: return bridge_RtlDowncaseUnicodeChar;
    case 276: return bridge_RtlDowncaseUnicodeString;
    case 278: return bridge_RtlEnterCriticalSectionAndRegion;
    case 280: return bridge_RtlEqualUnicodeString;
    case 284: return bridge_RtlFillMemory;
    case 287: return bridge_RtlFreeUnicodeString;
    case 288: return bridge_RtlGetCallersAddress;
    case 292: return bridge_RtlIntegerToChar;
    case 293: return bridge_RtlIntegerToUnicodeString;
    case 295: return bridge_RtlLeaveCriticalSectionAndRegion;
    case 296: return bridge_RtlLowerChar;
    case 297: return bridge_RtlMapGenericMask;
    case 298: return bridge_RtlMoveMemory;
    case 299: return bridge_RtlMultiByteToUnicodeN;
    case 300: return bridge_RtlMultiByteToUnicodeSize;
    case 306: return bridge_RtlTryEnterCriticalSection;
    case 307: return bridge_RtlUlongByteSwap;
    case 309: return bridge_RtlUnicodeStringToInteger;
    case 310: return bridge_RtlUnicodeToMultiByteN;
    case 311: return bridge_RtlUnicodeToMultiByteSize;
    case 313: return bridge_RtlUpcaseUnicodeChar;
    case 314: return bridge_RtlUpcaseUnicodeString;
    case 315: return bridge_RtlUpcaseUnicodeToMultiByteN;
    case 316: return bridge_RtlUpperChar;
    case 317: return bridge_RtlUpperString;
    case 318: return bridge_RtlUshortByteSwap;
    case 319: return bridge_RtlWalkFrameChain;
    case 320: return bridge_RtlZeroMemory;
    case 321: return bridge_XboxEEPROMKey;


    /* Routed. The XcRC4 pair was turned OFF mid-bisect and never turned back
     * on. Both pass the memory-model bar: a guest RC4_CONTEXT and a guest data
     * buffer, every access translated through XBOX_TO_NATIVE, no allocation
     * and no host pointer returned. Unrouted, savegame/EEPROM code that used
     * RC4 silently did nothing. */
    case 338: return bridge_XcRC4Key;
    case 339: return bridge_XcRC4Crypt;

    /* Routed. The lesson of the block that used to sit here was that an
     * xbox_* existence does not make a wrapper mechanical: each candidate has
     * to be checked for which side owns the allocation and whether an
     * out-pointer must carry a guest VA. IoCreateDevice was fixed by
     * allocating the device object in guest memory; ExFreePool below answers
     * through the guest heap; MmUnmapIoSpace/IoDeleteDevice do the same.
     *
     * The three beside them were parked, never broken: AvGetSavedDataAddress
     * returns a ULONG value, KeCancelTimer takes a guest KTIMER through
     * XBOX_TO_NATIVE, KeDisconnectInterrupt takes a guest KINTERRUPT the same
     * way. None of them allocates, frees, or hands back a host pointer. */
    case   1: return bridge_AvGetSavedDataAddress;
    case  17: return bridge_ExFreePool;
    case  97: return bridge_KeCancelTimer;
    case 100: return bridge_KeDisconnectInterrupt;
    /* Routed: thread base priority, queried and set. Neither allocates,
     * frees, nor returns a pointer -- each takes a thread handle and a
     * LONG. Half-Life 2 calls both while starting its worker threads,
     * and unbridged they returned 0, so every thread read its own base
     * priority as 0 and any priority the title set was discarded. */
    case 124: return bridge_KeQueryBasePriorityThread;
    case 143: return bridge_KeSetBasePriorityThread;
    /* Routed. Both clear the memory-model bar above: neither allocates,
     * frees, nor hands back a host pointer. KeStallExecutionProcessor takes a
     * microsecond count and busy-waits -- no pointers at all.
     * MmLockUnlockBufferPages takes (BaseAddress, Length, UnlockPages) and
     * pins physical pages, which is a no-op on the host; XBOX_TO_NATIVE is the
     * correct marshalling for its one address argument, and it writes nothing
     * through it.
     *
     * Half-Life 2 calls both during C++ static initialisation. Unbridged they
     * returned 0 without stalling or locking anything -- harmless in isolation,
     * but they are exactly the kind of silent no-op that makes a later failure
     * unattributable. */
    case 151: return bridge_KeStallExecutionProcessor;
    case 175: return bridge_MmLockUnlockBufferPages;
    /* Routed, both checked against the memory-model bar above.
     *
     * MmQueryAllocationSize now answers from the guest heap's block table
     * instead of the host's VirtualQuery, so nothing crosses the two worlds.
     *
     * NtCreateMutant creates a host mutex and hands it back through
     * bridge_write_handle, which is a guest token -- the same shape as
     * NtCreateEvent, which has been routed all along. It allocates no guest
     * memory and returns no host pointer.
     *
     * The Xbox Dashboard needs the mutant: its audio thread creates one during
     * the first tick, and unbridged the call returned STATUS_SUCCESS without
     * writing a handle, so the main thread waited on five events that nothing
     * would ever signal. */
    case 180: return bridge_MmQueryAllocationSize;
    case 192: return bridge_NtCreateMutant;
    case 221: return bridge_NtReleaseMutant;
    /* Routed. Checked against the memory-model warning above rather than
     * assumed mechanical: NtResumeThread takes a handle token and writes a
     * 4-byte suspend count through an optional out-parameter. Guest ULONG and
     * host ULONG are both 4 bytes, XBOX_TO_NATIVE already maps NULL to NULL,
     * and xbox_NtResumeThread checks the pointer before writing. Nothing
     * allocates, frees, or hands back a host pointer -- which is what
     * disqualified IoCreateDevice and ExFreePool.
     *
     * Halo 2276 calls this immediately before its first camera frustum build;
     * unbridged it returned 0 (STATUS_SUCCESS) without resuming anything, so a
     * thread the title had created suspended never started. */
    case 224: return bridge_NtResumeThread;
    case 231: return bridge_NtSuspendThread;
    /* Routed. ObfDereferenceObject is the fastcall partner of 251 (see its
     * wrapper) and only touches the object's reference count; PhyGetLinkState
     * reports link-up and PhyInitialize is a successful no-op. None of the
     * three allocates, frees, or hands back a host pointer; PhyGetLinkState is
     * what titles poll for Ethernet presence before selecting their network
     * path. */
    case 250: return bridge_ObfDereferenceObject;
    case 252: return bridge_PhyGetLinkState;
    case 253: return bridge_PhyInitialize;
    /* Routed. The memory-model note above already names this group as the
     * safe kind: each one reads or writes bytes at an address the caller
     * supplied, and none allocates, frees, or hands back a host pointer.
     *
     * Unbridged they returned 0 without hashing anything, which is invisible
     * until something checks a digest. The Xbox Dashboard verifies each XIP
     * archive it loads against a 20-byte digest in its own table
     * (sub_00034924) and calls HalReturnToFirmware(4) when the compare fails
     * -- so a no-op SHA does not corrupt anything, it reboots the console. */
    case 335: return bridge_XcSHAInit;
    case 336: return bridge_XcSHAUpdate;
    case 337: return bridge_XcSHAFinal;
    case 340: return bridge_XcHMAC;
    case 346: return bridge_XcDESKeyParity;

    /* Routed. Each of these was checked against the memory-model bar
     * described above before being added: reads or writes only happen at
     * caller-supplied guest addresses, and nothing allocates, frees, or hands
     * back a host pointer. Two of them (MmUnmapIoSpace, IoDeleteDevice) free
     * guest memory, so they deliberately answer through the guest heap rather
     * than calling the xbox_* host-heap version -- see the wrappers. */
    case  29: return bridge_ExSaveNonVolatileSetting;
    case  38: return bridge_HalClearSoftwareInterrupt;
    case  45: return bridge_HalReadSMBusValue;
    case  46: return bridge_HalReadWritePCISpace;
    case  48: return bridge_HalRequestSoftwareInterrupt;
    case  50: return bridge_HalWriteSMBusValue;
    case  68: return bridge_IoDeleteDevice;
    case  93: return bridge_KeAlertThread;
    case 125: return bridge_KeQueryInterruptTime;
    case 139: return bridge_KeRestoreFloatingPointState;
    case 142: return bridge_KeSaveFloatingPointState;
    case 158: return bridge_KeWaitForMultipleObjects;
    case 183: return bridge_MmUnmapIoSpace;
    case 193: return bridge_NtCreateSemaphore;
    case 222: return bridge_NtReleaseSemaphore;
    case 228: return bridge_NtSetSystemTime;
    case 247: return bridge_ObReferenceObjectByName;
    case 251: return bridge_ObfReferenceObject;
    case 290: return bridge_RtlInitUnicodeString;
    case 304: return bridge_RtlTimeFieldsToTime;

    /* Routed. The next batch is every remaining xbox_* implementation that
     * clears the memory-model bar, split by category:
     *
     *  - HAL/AV/port-I/O and the I/O Manager stubs all take or produce value
     *    arguments and caller-supplied guest structures; none of them returns
     *    a pointer the title dereferences. IofCallDriver/IofCompleteRequest
     *    are __fastcall (ns in ecx/edx).
     *  - The Xc* entries are documented stubs (no Xbox Live, no on-console key
     *    derivation); pointers are translated but never dereferenced.
     *  - MmAllocateSystemMemory/MmCreateKernelStack answer through the guest
     *    heap rather than VirtualAlloc so the title sees a 32-bit guest VA;
     *    their frees pair with them.
     *  - NtDuplicateObject dups a real native handle and hands it back as a
     *    fresh guest token; RtlRip logs guest strings; the ANSI<->Unicode
     *    conversion bridges do the whole conversion in guest memory (their
     *    xbox_* versions write host pointers into 4-byte guest slots).
     */
    case   4: return bridge_AvSetSavedDataAddress;
    case   5: return bridge_DbgBreakPoint;
    case  39: return bridge_HalDisableSystemInterrupt;
    case 360: return bridge_HalInitiateShutdown;
    case 358: return bridge_HalIsResetOrShutdownPending;
    case 333: return bridge_WRITE_PORT_BUFFER_USHORT;
    case 334: return bridge_WRITE_PORT_BUFFER_ULONG;

    case  61: return bridge_IoBuildDeviceIoControlRequest;
    case  62: return bridge_IoBuildSynchronousFsdRequest;
    case  69: return bridge_IoDeleteSymbolicLink;
    case  73: return bridge_IoInitializeIrp;
    case  74: return bridge_IoInvalidDeviceRequest;
    case  79: return bridge_IoSetIoCompletion;
    case  81: return bridge_IoStartNextPacket;
    case  82: return bridge_IoStartNextPacketByKey;
    case  83: return bridge_IoStartPacket;
    case  84: return bridge_IoSynchronousDeviceIoControlRequest;
    case  85: return bridge_IoSynchronousFsdRequest;
    case  86: return bridge_IofCallDriver;
    case  87: return bridge_IofCompleteRequest;
    case 359: return bridge_IoMarkIrpMustComplete;

    case 176: return bridge_MmLockUnlockPhysicalPage;
    /* Allocator and free together: both answer from the guest heap, and the
     * allocator page-aligns and zeroes because that is what the console's
     * page allocator returns and callers assume it. */
    case 167: return bridge_MmAllocateSystemMemory;
    case 172: return bridge_MmFreeSystemMemory;
    case 169: return bridge_MmCreateKernelStack;
    case 170: return bridge_MmDeleteKernelStack;

    case 341: return bridge_XcPKEncPublic;
    case 342: return bridge_XcPKDecPrivate;
    case 343: return bridge_XcPKGetKeyLen;
    case 344: return bridge_XcVerifyPKCS1Signature;
    case 345: return bridge_XcModExp;
    case 347: return bridge_XcKeyTable;
    case 348: return bridge_XcBlockCrypt;
    case 349: return bridge_XcBlockCryptCBC;
    case 350: return bridge_XcCryptService;
    case 351: return bridge_XcUpdateCrypto;

    case 352: return bridge_RtlRip;
    case 260: return bridge_RtlAnsiStringToUnicodeString;
    case 308: return bridge_RtlUnicodeStringToAnsiString;
    case 197: return bridge_NtDuplicateObject;

    /* Data exports */
    case 102: return bridge_MmGlobalData;
    case 120: return bridge_KeInterruptTime;
    case 154: return bridge_KeSystemTime;
    case 240: return bridge_ObDirectoryObjectType;
    case 245: return bridge_ObpObjectHandleTable;
    case 249: return bridge_ObSymbolicLinkObjectType;

    /* Dbg */
    case   6: return bridge_DbgBreakPointWithStatus;
    case   7: return bridge_DbgLoadImageSymbols;
    case   8: return bridge_DbgPrint;
    case  10: return bridge_DbgPrompt;
    case  11: return bridge_DbgUnLoadImageSymbols;

    /* Ex */
    case  12: return bridge_ExAcquireReadWriteLockExclusive;
    case  13: return bridge_ExAcquireReadWriteLockShared;
    case  18: return bridge_ExInitializeReadWriteLock;
    case  19: return bridge_ExInterlockedAddLargeInteger;
    case  20: return bridge_ExInterlockedAddLargeStatistic;
    case  21: return bridge_ExInterlockedCompareExchange64;
    case  25: return bridge_ExReadWriteRefurbInfo;
    case  26: return bridge_ExRaiseException;
    case  27: return bridge_ExRaiseStatus;
    case  28: return bridge_ExReleaseReadWriteLock;

    /* ExfInterlocked */
    case  32: return bridge_ExfInterlockedInsertHeadList;
    case  33: return bridge_ExfInterlockedInsertTailList;
    case  34: return bridge_ExfInterlockedRemoveHeadList;

    /* Fsc */
    case  36: return bridge_FscInvalidateIdleBlocks;

    /* Hal */
    case  43: return bridge_HalEnableSystemInterrupt;
    case 365: return bridge_HalEnableSecureTrayEject;
    case 366: return bridge_HalWriteSMCScratchRegister;

    /* Interlocked */
    case  51: return bridge_InterlockedCompareExchange;
    case  52: return bridge_InterlockedDecrement;
    case  53: return bridge_InterlockedIncrement;
    case  54: return bridge_InterlockedExchange;
    case  55: return bridge_InterlockedExchangeAdd;
    case  56: return bridge_InterlockedFlushSList;
    case  57: return bridge_InterlockedPopEntrySList;
    case  58: return bridge_InterlockedPushEntrySList;

    /* Io */
    case  59: return bridge_IoAllocateIrp;
    case  60: return bridge_IoBuildAsynchronousFsdRequest;
    case  63: return bridge_IoCheckShareAccess;
    case  72: return bridge_IoFreeIrp;
    case  75: return bridge_IoQueryFileInformation;
    case  76: return bridge_IoQueryVolumeInformation;
    case  77: return bridge_IoQueueThreadIrp;
    case  78: return bridge_IoRemoveShareAccess;
    case  80: return bridge_IoSetShareAccess;
    case  90: return bridge_IoDismountVolume;
    case  91: return bridge_IoDismountVolumeByName;

    /* Kd */

    /* Ke (new) */
    case  92: return bridge_KeAlertResumeThread;
    case  94: return bridge_KeBoostPriorityThread;
    case 101: return bridge_KeEnterCriticalRegion;
    case 103: return bridge_KeGetCurrentIrql;
    case 104: return bridge_KeGetCurrentThread;
    case 105: return bridge_KeInitializeApc;
    case 106: return bridge_KeInitializeDeviceQueue;
    case 108: return bridge_KeInitializeEvent;
    case 110: return bridge_KeInitializeMutant;
    case 111: return bridge_KeInitializeQueue;
    case 112: return bridge_KeInitializeSemaphore;
    case 114: return bridge_KeInsertByKeyDeviceQueue;
    case 115: return bridge_KeInsertDeviceQueue;
    case 116: return bridge_KeInsertHeadQueue;
    case 117: return bridge_KeInsertQueue;
    case 118: return bridge_KeInsertQueueApc;
    case 121: return bridge_KeIsExecutingDpc;
    case 122: return bridge_KeLeaveCriticalRegion;
    case 123: return bridge_KePulseEvent;
    case 130: return bridge_KeRaiseIrqlToSynchLevel;
    case 131: return bridge_KeReleaseMutant;
    case 132: return bridge_KeReleaseSemaphore;
    case 133: return bridge_KeRemoveByKeyDeviceQueue;
    case 134: return bridge_KeRemoveDeviceQueue;
    case 135: return bridge_KeRemoveEntryDeviceQueue;
    case 136: return bridge_KeRemoveQueue;
    case 138: return bridge_KeResetEvent;
    case 140: return bridge_KeResumeThread;
    case 141: return bridge_KeRundownQueue;
    case 144: return bridge_KeSetDisableBoostThread;
    case 146: return bridge_KeSetEventBoostPriority;
    case 147: return bridge_KeSetPriorityProcess;
    case 148: return bridge_KeSetPriorityThread;
    case 152: return bridge_KeSuspendThread;
    case 155: return bridge_KeTestAlertThread;
    case 162: return bridge_KiBugCheckData;
    case 163: return bridge_KiUnlockDispatcherDatabase;

    /* Mm */
    case 174: return bridge_MmIsAddressValid;
    case 374: return bridge_MmDbgAllocateMemory;
    case 375: return bridge_MmDbgFreeMemory;
    case 376: return bridge_MmDbgQueryAvailablePages;
    case 377: return bridge_MmDbgReleaseAddress;
    case 378: return bridge_MmDbgWriteCheck;

    /* Nt */
    case 185: return bridge_NtCancelTimer;
    case 191: return bridge_NtCreateIoCompletion;
    case 194: return bridge_NtCreateTimer;
    case 201: return bridge_NtOpenDirectoryObject;
    case 204: return bridge_NtProtectVirtualMemory;
    case 206: return bridge_NtQueueApcThread;
    case 208: return bridge_NtQueryDirectoryObject;
    case 209: return bridge_NtQueryEvent;
    case 212: return bridge_NtQueryIoCompletion;
    case 213: return bridge_NtQueryMutant;
    case 214: return bridge_NtQuerySemaphore;
    case 216: return bridge_NtQueryTimer;
    case 220: return bridge_NtReadFileScatter;
    case 223: return bridge_NtRemoveIoCompletion;
    case 227: return bridge_NtSetIoCompletion;
    case 229: return bridge_NtSetTimerEx;
    case 230: return bridge_NtSignalAndWaitForSingleObjectEx;
    case 237: return bridge_NtWriteFileGather;

    /* Ob */
    case 239: return bridge_ObCreateObject;
    case 241: return bridge_ObInsertObject;
    case 242: return bridge_ObMakeTemporaryObject;
    case 243: return bridge_ObOpenObjectByName;
    case 244: return bridge_ObOpenObjectByPointer;
    case 248: return bridge_ObReferenceObjectByPointer;

    /* Ps */
    case 254: return bridge_PsCreateSystemThread;
    case 256: return bridge_PsQueryStatistics;
    case 257: return bridge_PsSetCreateThreadNotifyRoutine;

    /* Rtl */
    case 281: return bridge_RtlExtendedIntegerMultiply;
    case 282: return bridge_RtlExtendedLargeIntegerDivide;
    case 283: return bridge_RtlExtendedMagicDivide;
    case 285: return bridge_RtlFillMemoryUlong;
    case 286: return bridge_RtlFreeAnsiString;
    case 303: return bridge_RtlRaiseStatus;
    case 361: return bridge_RtlSnprintf;
    case 362: return bridge_RtlSprintf;
    case 363: return bridge_RtlVsnprintf;
    case 364: return bridge_RtlVsprintf;

    /* Port I/O */
    case 329: return bridge_READ_PORT_BUFFER_UCHAR;
    case 330: return bridge_READ_PORT_BUFFER_USHORT;
    case 331: return bridge_READ_PORT_BUFFER_ULONG;
    case 332: return bridge_WRITE_PORT_BUFFER_UCHAR;

    default:  return NULL;
    }
}

/* ── Per-slot bridge functions (resolved at init) ────────── */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];
static uint8_t g_slot_arg_unknown[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Xbox VA to sample around each bridge call; 0 = off. See dispatch. */
uint32_t g_kernel_watch_va = 0;

/* Arm the watch from the environment.
 *
 * The facility existed but nothing set it, so it was unreachable.
 * RECOMP_KERNEL_WATCH=<guest addr> samples that dword either side of every
 * bridge call and names the ordinal that changed it -- which is the one fact
 * a hardware watchpoint cannot give, because a bridge corrupting Xbox memory
 * faults inside ntdll with no recompiled frame to blame.
 *
 * A change seen between the previous call's "after" and this call's "before"
 * is guest code, not a bridge, which is just as useful to know. */
static void kernel_watch_arm_once(void)
{
    static int done;
    const char *env;
    if (done)
        return;
    done = 1;
    env = getenv("RECOMP_KERNEL_WATCH");
    if (env)
        g_kernel_watch_va = (uint32_t)strtoul(env, NULL, 0);
}

/* Current dispatching slot */
static RECOMP_TLS int g_kernel_dispatch_slot = -1;

static void kernel_thunk_dispatch_body(void);

/* The guest CPU lock (RECOMP_GUEST_LOCK=1).
 *
 * The Xbox has one CPU: two guest threads never run guest code at the same
 * instant, and titles lean on that without knowing it -- a worker fills a
 * buffer and sets a flag with plain stores, and the reader trusts the order.
 * Here every guest thread runs on its own host core, and the lifted C gives
 * no ordering either. T()NY crashes somewhere different on every run
 * once its loader workers start.
 *
 * With the lock on, a guest thread holds it while it runs guest code and
 * lets go for the length of every kernel call -- the waits, sleeps and I/O
 * where the console's scheduler would switch threads anyway. Host threads
 * that only run a guest callback (vblank, interrupts) never take it.
 *
 * The lock itself, its hand-off to a waiting thread and the preemption at
 * the lifted code's loop back edges are kernel_guest_cpu.c; these are the
 * bridge's entry points. */

/* Called once on each host thread that runs a guest thread, before its first
 * guest instruction. */
void guest_cpu_join(void)
{
    xbox_GuestCpuJoin();
}

void guest_cpu_part(void)
{
    xbox_GuestCpuPart();
}

/* A guest thread lets go of the guest CPU for the length of every kernel
 * call, and takes it back when it had it. A guest routine a bridge runs (an
 * APC, an unwind handler) makes its own kernel calls through here too: they
 * find the CPU as the bridge's callback entry left it and restore that, so
 * the routine returns into the bridge holding what it held. */
static void kernel_thunk_dispatch(void)
{
    int slot = g_kernel_dispatch_slot;
    uint32_t ordinal = slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE
                     ? g_slot_ordinals[slot] : 0;
    /* A thread re-entering a save state's chain in a fresh process (XBA-250) arrives at the call it was parked
     * in: the record's registers, and either the status the call had returned or the call made again. */
    if (xbox_ResumeKernelPending() && xbox_ResumeKernelArrive(ordinal))
        return;
    for (;;) {
        int held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, ordinal);
        /* A kernel boundary: the thread is parked for a snapshot (XBA-244) until
         * it runs guest code again, so also while it queues for the guest CPU;
         * a freeze holds it at the end. */
        xbox_ParkKernelEnter(ordinal, g_esp ? BRIDGE_MEM32(g_esp) : 0);
        ks_thread_kernel_enter();   /* a queue's thread is not running in here (XBA-133) */
        xbox_SehRuntimeEnter();      /* a host fault in the bridge stays the runtime's (XBA-132) */
        kernel_thunk_dispatch_body();
        xbox_SehRuntimeLeave();
        ks_thread_kernel_leave();
        xbox_ParkKernelReturned();   /* back from the bridge, maybe queued for the guest CPU next */
        if (held)
            xbox_GuestCpuAcquire();
        xbox_ParkKernelLeave();
        /* A same-run state load (XBA-249) put this thread back « in this call »:
         * its registers are the ones the guest made the call with (the restored
         * guest stack holds the arguments), so it is made again. */
        if (!xbox_ParkTakeReissue())
            break;
        g_kernel_dispatch_slot = slot;
    }
}

static void kernel_thunk_dispatch_body(void)
{
    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;

    /* A kernel call is the gate holder's safe point: an interrupt posted
     * while it held the gate runs here, before the bridge reads its
     * arguments or takes any lock (xbox_IrqPost). After the slot is read,
     * since a kernel call in the routine sets it again. */
    xbox_IrqSafePoint();

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];

    g_kernel_call_count++;
    if (ordinal < XBOX_KERNEL_THUNK_TABLE_SIZE)
        g_ordinal_calls[ordinal]++;

    if (KERNEL_LOG_ON()) {
        /* The guest return address sits at the top of the guest stack: the
         * caller pushed it before dispatching here. Logging it turns "some
         * function is calling this" into "this call site is", which is the
         * difference between guessing and knowing when a title recurses. */
        fprintf(stderr,
                "  [KERNEL] #%lld: ordinal %u (slot %d) esp=0x%08X ret=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_esp,
                g_esp ? BRIDGE_MEM32(g_esp) : 0);
        fflush(stderr);
    }

    {
        static DWORD last_summary_tick = 0;
        DWORD now = GetTickCount();
        if (last_summary_tick == 0) last_summary_tick = now;
        if (now - last_summary_tick >= 2000 && g_kernel_call_count > 200) {
            fprintf(stderr, "  [KERNEL] summary: %lld total calls, latest ordinal %u (slot %d) esp=0x%08X\n",
                    g_kernel_call_count, ordinal, slot, g_esp);
            /* And which ones, ranked. "Latest" names whatever the sample
             * happened to land on; the question behind this line is what a
             * title sitting still is actually asking the kernel for, and
             * that wants counting rather than sampling. */
            {
                static unsigned char shown_ord[XBOX_KERNEL_THUNK_TABLE_SIZE];
                int r, shown;

                memset(shown_ord, 0, sizeof shown_ord);
                for (shown = 0; shown < 6; shown++) {
                    int best = -1;
                    for (r = 0; r < XBOX_KERNEL_THUNK_TABLE_SIZE; r++)
                        if (g_ordinal_calls[r] && !shown_ord[r]
                            && (best < 0 || g_ordinal_calls[r] > g_ordinal_calls[best]))
                            best = r;
                    if (best < 0)
                        break;
                    shown_ord[best] = 1;
                    fprintf(stderr, "  [KERNEL]   ordinal %3d x%llu\n", best,
                            (unsigned long long)g_ordinal_calls[best]);
                }
            }
            /* And what the threads are waiting on, from where
             * (kernel_wait_stats.h, RECOMP_WAIT_REPORT). */
            xbox_wait_stats_report(stderr);
            /* And how often a snapshot could have been taken (XBA-244). */
            xbox_ParkReport(stderr);
            fflush(stderr);
            last_summary_tick = now;
        }
    }

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    /* Keep the caller before popping it. A bridge that blocks -- a critical
     * section above all -- can then say which guest call site is holding the
     * lock, which is the one fact a deadlock report otherwise lacks. */
    g_xbox_kernel_caller = g_esp ? BRIDGE_MEM32(g_esp) : 0;
    g_esp += 4;

    /* Name the bridge that corrupts a watched dword.
     *
     * A bridge hands Xbox pointers to real Win32 calls, so a bad one has
     * Windows write into Xbox memory -- the resulting wild write has a stack
     * inside ntdll with no recompiled frame to blame, and a watchpoint just
     * says "something changed". Sampling either side of the call names the
     * ordinal directly, which is the one fact those tools cannot give.
     *
     * Set g_kernel_watch_va to arm; zero (the default) costs one compare. */
    uint32_t _watch_before = 0;
    kernel_watch_arm_once();
    if (g_kernel_watch_va) {
        _watch_before = BRIDGE_MEM32(g_kernel_watch_va);
        /* Reporting only on change misses the case that matters most: a value
         * that was already wrong before the first call sampled it. Printing
         * every sample under RECOMP_KERNEL_WATCH_ALL shows when it changed
         * even if no single bridge did it. */
        if (getenv("RECOMP_KERNEL_WATCH_ALL")) {
            static uint32_t seen = 0xDEADBEEFu;
            if (_watch_before != seen) {
                seen = _watch_before;
                fprintf(stderr, "  [KWATCH] 0x%08X = %08X before ordinal %u"
                                " (call #%lld)\n",
                        g_kernel_watch_va, _watch_before, ordinal,
                        g_kernel_call_count);
                fflush(stderr);
            }
        }
    }

    /* A stub route, or no route at all: say so the first time (XBA-115,
     * kernel_stubs.h). */
    if (!bridge || xbox_kernel_is_stub(ordinal))
        xbox_kernel_stub_note(ordinal, bridge != NULL, g_xbox_kernel_caller);

    /* Queued APCs (KeInsertQueueApc, XBA-113) run at this thread's kernel
     * calls; an alertable wait they answer is not made. */
    if (ks_apc_pending() && bridge_apc_safe_point(ordinal)) {
        /* answered: STATUS_USER_APC */
    } else if (bridge) {
        bridge();
    } else {
        /* No specific bridge - return 0. Warn once per ordinal rather than
         * gating on g_kernel_call_count: a missing bridge is rare and is
         * usually the reason a game misbehaves, so it must not be swallowed
         * by the general call-trace throttle. Bounded to one line per slot. */
        static uint8_t warned[XBOX_KERNEL_THUNK_TABLE_SIZE];
        if (!warned[slot]) {
            warned[slot] = 1;
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u (slot %d), returning 0\n",
                    ordinal, slot);
            /* "Returning 0" is the harmless half. The damaging half is the
             * stack: the Xbox kernel is stdcall, so the callee owes the caller
             * its arguments back, and an ordinal missing from
             * stdcall_args_for_ordinal() returns 0 bytes and leaves them
             * there. The caller's own `pop`s then run low by that much and it
             * returns with its callee-saved registers rotated -- silently,
             * frames away from here. Say so, because a title that dies of this
             * looks nothing like a title that is missing a kernel function. */
            if (g_slot_arg_unknown[slot])
                fprintf(stderr, "  [KERNEL]   ordinal %u has no entry in "
                        "stdcall_args_for_ordinal(). If it takes arguments, "
                        "this call is corrupting the caller's stack -- add its "
                        "argument size there before anything else.\n", ordinal);
            fflush(stderr);
        }
        g_eax = 0;
    }

    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    if (g_kernel_watch_va) {
        uint32_t _after = BRIDGE_MEM32(g_kernel_watch_va);
        if (_after != _watch_before) {
            fprintf(stderr,
                    "  [KWATCH] ordinal %u changed Xbox VA 0x%08X: "
                    "%08X -> %08X\n",
                    ordinal, g_kernel_watch_va, _watch_before, _after);
            fflush(stderr);
        }
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }
}

/* The thunk VA a kernel call by ordinal goes through in this run (0: not imported), XBA-250. */
uint32_t xbox_KernelThunkVa(uint32_t ordinal)
{
    int slot;
    for (slot = 0; slot < XBOX_KERNEL_THUNK_TABLE_SIZE; slot++)
        if (g_slot_ordinals[slot] == ordinal && ordinal)
            return KERNEL_VA_BASE + (uint32_t)slot * 4u;
    return 0;
}

/* ── Dispatch lookup ────────────────────────────────────── */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
        return NULL;
    }
    /* A handler jumping to an __except block or a catch continuation of a
     * live function (xbox_seh.c): not a function start, so every lookup
     * before this one missed. Cheap when no frame is anchored. */
    return (recomp_func_t)xbox_SehLookup(xbox_va);
}

/* ── Initialization ─────────────────────────────────────── */

/*
 * Where this title's kernel thunk table lives. Defaults to the compile-time
 * constant, but every XBE puts it somewhere different (it comes from the
 * header's KernelImageThunkAddress), so xbox_MemoryLayoutInit() parses the
 * real address out of the binary and overrides it here.
 *
 * Halo build 2276 puts it at 0x00253090 against the default's 0x0036B7C0 --
 * without the override the bridge patches ordinals into whatever happens to
 * live at the wrong address and every kernel call goes somewhere arbitrary.
 */
static uint32_t g_thunk_table_base  = XBOX_KERNEL_THUNK_TABLE_BASE;
static uint32_t g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;

/**
 * Return the kernel thunk table address and entry count currently in effect.
 * The address is parsed from the XBE header during memory layout init, so the
 * values are per-title, not hardcoded. *base/*count are set to 0 if unavailable.
 */
void xbox_kernel_get_thunk_address(uint32_t *xbox_va, uint32_t *count)
{
    if (xbox_va) *xbox_va = g_thunk_table_base;
    if (count)  *count  = g_thunk_table_count;
}

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count)
{
    if (!xbox_va) {
        return;
    }

    g_thunk_table_base = xbox_va;

    /* count indexes g_slot_* arrays, which are sized by the macro. A title
     * importing more slots than the real kernel exports would run off them. */
    if (count && count <= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        g_thunk_table_count = count;
    } else if (count > XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr,
                "  Kernel thunk bridge: XBE declares %u thunk slots, clamping to %d\n",
                count, XBOX_KERNEL_THUNK_TABLE_SIZE);
        g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;
    }
}

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
/* Per-title ordinal remap; NULL = identity (the kernel's own XDK). Set by
 * xbox_kernel_set_ordinal_remap before init. See kernel.h. */
static const unsigned short *g_ordinal_remap = NULL;
static int g_ordinal_remap_count = 0;

void xbox_kernel_set_ordinal_remap(const unsigned short *map, int count)
{
    g_ordinal_remap = map;
    g_ordinal_remap_count = count;
}

void xbox_kernel_bridge_init(void)
{
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    /* The caller goes on to run the title's entry point: a guest thread
     * from here, so its raises drain queued DPCs before it creates any
     * other thread. */
    xbox_DpcMarkGuestThread();
    guest_cpu_join();
#ifndef _WIN32
    {   /* RECOMP_STATE_PROBE: periodic save-state captures, measured (XBA-245) */
        extern void xbox_StateProbeStart(void);
        extern void xbox_StateServiceStart(void);
        xbox_StateProbeStart();
        xbox_StateServiceStart();   /* the menu's save states, the chords, rewind (XBA-249) */
    }
#endif
    if (!g_thunk_table_base) {
        fprintf(stderr, "  Kernel thunk bridge: no thunk table address (the XBE "
                        "header's was not read): nothing patched\n");
        return;
    }
    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    /* The thunk table lives in .rdata which is marked PAGE_READONLY.
     * Temporarily make it writable so we can patch the ordinals. */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory, then translate it into
             * the kernel's canonical ordinal space. Identity unless the title
             * set a remap (a different XDK). Every routing decision below --
             * data export, bridge, arg size -- keys off the canonical ordinal,
             * so one translation here covers all three. */
            ULONG ordinal = current & 0x7FFFFFFF;
            if (g_ordinal_remap && ordinal < (ULONG)g_ordinal_remap_count
                && g_ordinal_remap[ordinal]) {
                ordinal = g_ordinal_remap[ordinal];
            }
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            { int _n = stdcall_args_for_ordinal(ordinal);
              g_slot_arg_unknown[i] = (uint8_t)(_n < 0);
              g_slot_arg_bytes[i] = (_n < 0) ? 0 : _n; }
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
                fprintf(stderr, "  [KERNEL] unbridged function thunk: ordinal %lu"
                        " (slot %u, VA 0x%08X)\n",
                        (unsigned long)ordinal, i, KERNEL_VA_BASE + i * 4);
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /*
     * Thunk entries below the header-declared base.
     *
     * KernelImageThunkAddress points at the main import run, but the linker can
     * emit further runs just before it, separated by a NULL. Halo has three at
     * base-0x10 (ordinals 52, 51 and 5). Those stay unpatched, so game code
     * doing "mov ebx,[thunk]; call ebx" jumps to the raw 0x8000xxxx marker
     * instead of a kernel function. The indirect call cannot resolve it, yields
     * 0, and a caller looping until it sees an error code never sees one - in
     * Halo that hung main() in a file-enumeration loop before it reached any
     * initialisation.
     *
     * Only entries still carrying the ordinal marker are touched, so scanning
     * back over unrelated .rdata is harmless.
     */
    {
        const int LOOKBEHIND = 16;   /* entries, i.e. 64 bytes */
        DWORD scan_protect;
        uint32_t low = g_thunk_table_base - LOOKBEHIND * 4;

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, PAGE_READWRITE, &scan_protect);

        for (i = 1; i <= LOOKBEHIND; i++) {
            uint32_t va = g_thunk_table_base - i * 4;
            uint32_t current = BRIDGE_MEM32(va);
            int slot;

            if (!(current & 0x80000000u) ||
                (current & 0x7FFFFFFFu) == 0 ||
                (current & 0x7FFFFFFFu) > XBOX_KERNEL_THUNK_TABLE_SIZE) {
                continue;            /* NULL separator or ordinary data */
            }
            slot = g_thunk_table_count + i;   /* park these above the main run */
            if (slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
                break;
            }

            g_slot_ordinals[slot] = current & 0x7FFFFFFF;
            g_slot_bridges[slot] = bridge_for_ordinal(g_slot_ordinals[slot]);
            { int _n = stdcall_args_for_ordinal(g_slot_ordinals[slot]);
              g_slot_arg_unknown[slot] = (uint8_t)(_n < 0);
              g_slot_arg_bytes[slot] = (_n < 0) ? 0 : _n; }
            BRIDGE_MEM32(va) = KERNEL_VA_BASE + slot * 4;
            resolved++;
            if (g_slot_bridges[slot]) bridged++; else unbridged++;

            fprintf(stderr, "  [KERNEL] extra thunk at 0x%08X: ordinal %u (slot %d)\n",
                    va, g_slot_ordinals[slot], slot);
        }

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, scan_protect, &scan_protect);
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}

/* ═══════════════════════════════════════════════════════════════════════════
 * Save states: the bridge's tables (XBA-246, kernel_kobj_state.h)
 *
 * What the bridge keeps for the guest, host-side: the handle table (token ->
 * host object, and the guest thread object or ObCreateObject body behind a
 * token), the guest thread object pool, the KE shadows (guest KEVENT /
 * KMUTANT / KSEMAPHORE / KTIMER / thread object -> host object), kernel
 * timers, the DPC queue, threads' alert state, ObCreateObject objects, and
 * the small registration tables. Host objects go through xbox_kobj_ref, which
 * gives each one id however many tables name it; the records carry ids.
 * ═══════════════════════════════════════════════════════════════════════════ */

enum {
    KOBJ_TAB_ISR = 1, KOBJ_TAB_SHUTDOWN, KOBJ_TAB_NOTIFY, KOBJ_TAB_APC_POOL,
    KOBJ_TAB_ASYNC, KOBJ_TAB_PROMOTED, KOBJ_TAB_SET_SEEN
};

static void kobj_u32tab(XboxKobjSaveCtx *c, uint32_t id, const volatile uint32_t *v,
                        uint32_t n)
{
    XboxKobjBuf *b;
    uint32_t i, used = 0;
    for (i = 0; i < n; i++)
        if (v[i]) used = i + 1;
    if (!used)
        return;
    b = xbox_kobj_rec_begin(c, XKOB_R_U32TAB);
    xbox_kobj_u32(b, id);
    xbox_kobj_u32(b, used);
    for (i = 0; i < used; i++)
        xbox_kobj_u32(b, v[i]);
    xbox_kobj_rec_end(c);
}

/* The timer thread and its lock, made once. This ran on whichever guest
 * threads called KeSetTimer first, unguarded: two at once initialised the
 * lock twice (on POSIX that replaces a mutex another thread may hold) and
 * started two timer threads, after which every timer DPC and vblank tick
 * was delivered twice (KRN-9). */
static BOOL CALLBACK kernel_timer_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_timer_lock);
    g_timer_started = 1;
    CloseHandle(CreateThread(NULL, 0, kernel_timer_thread, NULL, 0, NULL));
    return TRUE;
}

static void kernel_timer_start_once(void)
{
    InitOnceExecuteOnce(&g_timer_once, kernel_timer_init, NULL, NULL);
}

void xbox_bridge_kobj_handles(void (*fn)(void *ctx, uint32_t guest_va, void *h), void *ctx)
{
    uint32_t i;

    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i])
            fn(ctx, s_token_thobj[i], s_handle_table[i]);
    ke_shadow_init();
    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < g_ke_shadow_cap; i++)
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].host_handle)
            fn(ctx, g_ke_shadow[i].guest_va, g_ke_shadow[i].host_handle);
    LeaveCriticalSection(&g_ke_shadow_cs);
    thalert_lock();
    for (i = 0; i < THALERT_MAX; i++)
        if (s_thalert[i].kthread && s_thalert[i].host)
            fn(ctx, s_thalert[i].kthread, s_thalert[i].host);
    thalert_unlock();
}

void xbox_bridge_kobj_save(XboxKobjSaveCtx *c)
{
    XboxKobjBuf *b;
    uint32_t i;

    /* Handle table: one record per live token. */
    handle_lock();
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++) {
        uint32_t id, token = BRIDGE_HANDLE_TAG | i;
        if (!s_handle_table[i] && !s_token_thobj[i] && !s_ob_token[i])
            continue;
        id = xbox_kobj_ref(c, s_handle_table[i], token, s_token_thobj[i]);
        b = xbox_kobj_rec_begin(c, XKOB_R_HANDLE);
        xbox_kobj_u32(b, i);
        xbox_kobj_u32(b, id);
        xbox_kobj_u32(b, s_token_thobj[i]);
        xbox_kobj_u32(b, s_ob_token[i]);
        xbox_kobj_rec_end(c);
    }
    handle_unlock();

    /* Guest thread object pool. */
    thobj_lock();
    if (s_thobj_count) {
        b = xbox_kobj_rec_begin(c, XKOB_R_THPOOL);
        xbox_kobj_u32(b, s_thobj_count);
        for (i = 0; i < s_thobj_count; i++) {
            xbox_kobj_u32(b, s_thobj_va[i]);
            xbox_kobj_u32(b, (uint32_t)s_thobj_refs[i]);
        }
        xbox_kobj_rec_end(c);
    }
    thobj_unlock();

    /* KE shadows. */
    ke_shadow_init();
    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < g_ke_shadow_cap; i++) {
        uint32_t id;
        if (!g_ke_shadow[i].in_use)
            continue;
        id = xbox_kobj_ref(c, g_ke_shadow[i].host_handle, 0, g_ke_shadow[i].guest_va);
        b = xbox_kobj_rec_begin(c, XKOB_R_SHADOW);
        xbox_kobj_u32(b, g_ke_shadow[i].guest_va);
        xbox_kobj_u32(b, id);
        xbox_kobj_u32(b, g_ke_shadow[i].in_use);
        xbox_kobj_rec_end(c);
    }
    LeaveCriticalSection(&g_ke_shadow_cs);

    /* Kernel timers, due times relative to the guest clock. */
    if (g_timer_started) {
        long long now = kernel_timer_clock_ms();
        EnterCriticalSection(&g_timer_lock);
        for (i = 0; i < XBOX_MAX_TIMERS; i++) {
            long long left;
            if (!g_timers[i].timer_va)
                continue;
            left = g_timers[i].due_ms - now;
            b = xbox_kobj_rec_begin(c, XKOB_R_TIMER);
            xbox_kobj_u32(b, g_timers[i].timer_va);
            xbox_kobj_u32(b, g_timers[i].dpc_va);
            xbox_kobj_u64(b, (uint64_t)(left > 0 ? left : 0));
            xbox_kobj_u32(b, (uint32_t)g_timers[i].period_ms);
            xbox_kobj_rec_end(c);
        }
        LeaveCriticalSection(&g_timer_lock);
    }

    /* Queued DPCs, in order. */
    dpc_lock();
    for (i = (uint32_t)g_dpc_head; i != (uint32_t)g_dpc_tail; i = (i + 1) % XBOX_MAX_PENDING_DPC) {
        b = xbox_kobj_rec_begin(c, XKOB_R_DPC);
        xbox_kobj_u32(b, g_dpc_queue[i].dpc);
        xbox_kobj_u32(b, g_dpc_queue[i].arg1);
        xbox_kobj_u32(b, g_dpc_queue[i].arg2);
        xbox_kobj_rec_end(c);
    }
    LeaveCriticalSection(&g_dpc_lock);

    /* Alert state. */
    thalert_lock();
    for (i = 0; i < THALERT_MAX; i++) {
        uint32_t id;
        if (!s_thalert[i].kthread)
            continue;
        id = xbox_kobj_ref(c, s_thalert[i].host, 0, s_thalert[i].kthread);
        b = xbox_kobj_rec_begin(c, XKOB_R_THALERT);
        xbox_kobj_u32(b, s_thalert[i].kthread);
        xbox_kobj_u32(b, id);
        xbox_kobj_u32(b, (uint32_t)s_thalert[i].waiting);
        xbox_kobj_u8(b, s_thalert[i].alerted[0]);
        xbox_kobj_u8(b, s_thalert[i].alerted[1]);
        xbox_kobj_rec_end(c);
    }
    thalert_unlock();

    /* ObCreateObject objects (their bodies and headers are guest memory). */
    ob_lock();
    for (i = 0; i < OB_MAX; i++) {
        if (!s_ob[i].body)
            continue;
        b = xbox_kobj_rec_begin(c, XKOB_R_OB);
        xbox_kobj_u32(b, s_ob[i].body);
        xbox_kobj_u32(b, s_ob[i].base);
        xbox_kobj_u32(b, (uint32_t)s_ob[i].inserted);
        xbox_kobj_str(b, s_ob[i].name);
        xbox_kobj_rec_end(c);
    }
    ob_unlock();

    kobj_u32tab(c, KOBJ_TAB_ISR, g_connected_isr, XBOX_MAX_VECTORS);
    kobj_u32tab(c, KOBJ_TAB_SHUTDOWN, s_shutdown_reg, SHUTDOWN_NOTIFY_MAX);
    kobj_u32tab(c, KOBJ_TAB_NOTIFY, s_thread_notify, THREAD_NOTIFY_MAX);
    kobj_u32tab(c, KOBJ_TAB_APC_POOL, s_apc_pool, APC_POOL_MAX);
    kobj_u32tab(c, KOBJ_TAB_ASYNC, g_async_handles, BRIDGE_ASYNC_MAX);
    kobj_u32tab(c, KOBJ_TAB_PROMOTED, g_ke_promoted,
                g_ke_promoted_n < KE_PROMOTED_MAX ? (uint32_t)g_ke_promoted_n : KE_PROMOTED_MAX);
    kobj_u32tab(c, KOBJ_TAB_SET_SEEN, g_ke_set_seen,
                g_ke_set_seen_n < KE_PROMOTED_MAX ? (uint32_t)g_ke_set_seen_n : KE_PROMOTED_MAX);
}

void xbox_bridge_kobj_reset(void)
{
    uint32_t i;

    /* Handle table: every token's reference, as NtClose drops it, but with
     * nothing settled on disk (xbox_FileStateForget). */
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++) {
        HANDLE h;
        handle_lock();
        h = s_handle_table[i];
        handle_slot_set(i, NULL);
        handle_unlock();
        if (h && h != INVALID_HANDLE_VALUE) {
#ifndef _WIN32
            xbox_FileStateForget(h);
#endif
            CloseHandle(h);
        }
    }
    thobj_lock();
    memset(s_token_thobj, 0, sizeof s_token_thobj);
    memset(s_thobj_refs, 0, sizeof s_thobj_refs);
    memset(s_thobj_va, 0, sizeof s_thobj_va);
    s_thobj_count = 0;
    thobj_unlock();

    ke_shadow_init();
    EnterCriticalSection(&g_ke_shadow_cs);
    for (i = 0; i < g_ke_shadow_cap; i++) {
        if (g_ke_shadow[i].in_use && g_ke_shadow[i].host_handle)
            CloseHandle(g_ke_shadow[i].host_handle);
        memset(&g_ke_shadow[i], 0, sizeof g_ke_shadow[i]);
    }
    g_ke_shadow_count = 0;
    LeaveCriticalSection(&g_ke_shadow_cs);

    if (g_timer_started) {
        EnterCriticalSection(&g_timer_lock);
        memset(g_timers, 0, sizeof g_timers);
        LeaveCriticalSection(&g_timer_lock);
    }
    dpc_lock();
    g_dpc_head = g_dpc_tail = 0;
    memset(g_dpc_queue, 0, sizeof g_dpc_queue);
    LeaveCriticalSection(&g_dpc_lock);

    thalert_lock();
    for (i = 0; i < THALERT_MAX; i++)
        if (s_thalert[i].host)
            CloseHandle(s_thalert[i].host);
    memset(s_thalert, 0, sizeof s_thalert);
    thalert_unlock();

    ob_lock();
    memset(s_ob, 0, sizeof s_ob);
    memset(s_ob_token, 0, sizeof s_ob_token);
    ob_unlock();

    memset(g_connected_isr, 0, sizeof g_connected_isr);
    memset(s_shutdown_reg, 0, sizeof s_shutdown_reg);
    memset(s_thread_notify, 0, sizeof s_thread_notify);
    s_thread_notify_count = 0;
    memset(s_apc_pool, 0, sizeof s_apc_pool);
    memset(g_async_handles, 0, sizeof g_async_handles);
    memset((void *)g_ke_promoted, 0, sizeof g_ke_promoted);
    g_ke_promoted_n = 0;
    memset((void *)g_ke_set_seen, 0, sizeof g_ke_set_seen);
    g_ke_set_seen_n = 0;
}

static void kobj_load_u32tab(XboxKobjRd *r)
{
    uint32_t id = xbox_kobj_get32(r), n = xbox_kobj_get32(r), i, cap = 0;
    volatile uint32_t *dst = NULL;
    switch (id) {
    case KOBJ_TAB_ISR:      dst = g_connected_isr;  cap = XBOX_MAX_VECTORS; break;
    case KOBJ_TAB_SHUTDOWN: dst = s_shutdown_reg;   cap = SHUTDOWN_NOTIFY_MAX; break;
    case KOBJ_TAB_NOTIFY:   dst = s_thread_notify;  cap = THREAD_NOTIFY_MAX; break;
    case KOBJ_TAB_APC_POOL: dst = s_apc_pool;       cap = APC_POOL_MAX; break;
    case KOBJ_TAB_ASYNC:    dst = g_async_handles;  cap = BRIDGE_ASYNC_MAX; break;
    case KOBJ_TAB_PROMOTED: dst = g_ke_promoted;    cap = KE_PROMOTED_MAX; break;
    case KOBJ_TAB_SET_SEEN: dst = g_ke_set_seen;    cap = KE_PROMOTED_MAX; break;
    default: break;
    }
    for (i = 0; i < n; i++) {
        uint32_t v = xbox_kobj_get32(r);
        if (dst && i < cap)
            dst[i] = v;
    }
    if (n > cap) n = cap;
    if (id == KOBJ_TAB_NOTIFY) {
        LONG k = 0;
        for (i = 0; i < n; i++) if (s_thread_notify[i]) k++;
        s_thread_notify_count = k;
    } else if (id == KOBJ_TAB_PROMOTED) {
        g_ke_promoted_n = (LONG)n;
    } else if (id == KOBJ_TAB_SET_SEEN) {
        g_ke_set_seen_n = (LONG)n;
    }
}

int xbox_bridge_kobj_load(XboxKobjLoadCtx *c, uint16_t type, XboxKobjRd *r)
{
    uint32_t i;

    switch (type) {
    case XKOB_R_HANDLE: {
        uint32_t idx = xbox_kobj_get32(r), id = xbox_kobj_get32(r);
        uint32_t thobj = xbox_kobj_get32(r), ob = xbox_kobj_get32(r);
        HANDLE h = id ? (HANDLE)xbox_kobj_obj(c, id) : NULL;
        if (!idx || idx >= BRIDGE_HANDLE_MAX) {
            xbox_kobj_fail(c, "handle index %u out of range", idx);
            if (h) CloseHandle(h);
            return 1;
        }
        if (id && !h) {
            xbox_kobj_fail(c, "handle 0x%08X: object %u was not made",
                           BRIDGE_HANDLE_TAG | idx, id);
            return 1;
        }
        handle_lock();
        if (s_handle_table[idx] && h) {
            handle_unlock();
            CloseHandle(h);
            xbox_kobj_fail(c, "handle 0x%08X twice", BRIDGE_HANDLE_TAG | idx);
            return 1;
        }
        handle_slot_set(idx, h);
        handle_unlock();
        s_token_thobj[idx] = thobj;   /* its reference is in the pool's counts */
        s_ob_token[idx] = ob;
        return 1;
    }
    case XKOB_R_THPOOL: {
        uint32_t n = xbox_kobj_get32(r);
        thobj_lock();
        for (i = 0; i < n; i++) {
            uint32_t va = xbox_kobj_get32(r), refs = xbox_kobj_get32(r);
            if (i < THOBJ_MAX) {
                s_thobj_va[i] = va;
                s_thobj_refs[i] = (LONG)refs;
            }
        }
        s_thobj_count = n < THOBJ_MAX ? n : THOBJ_MAX;
        thobj_unlock();
        return 1;
    }
    case XKOB_R_SHADOW: {
        uint32_t va = xbox_kobj_get32(r), id = xbox_kobj_get32(r), use = xbox_kobj_get32(r);
        HANDLE h = (HANDLE)xbox_kobj_obj(c, id);
        int done = 0;
        if (id && !h) {
            xbox_kobj_fail(c, "KE shadow of 0x%08X: object %u was not made", va, id);
            return 1;
        }
        ke_shadow_init();
        EnterCriticalSection(&g_ke_shadow_cs);
        {
            int k = ke_shadow_find_locked(va);
            if (k >= 0) {                      /* a duplicate record: the last wins */
                if (g_ke_shadow[k].host_handle && g_ke_shadow[k].host_handle != h)
                    CloseHandle(g_ke_shadow[k].host_handle);
                ke_shadow_erase_locked((uint32_t)k);
            }
            done = ke_shadow_add_locked(va, h, use ? use : 1);
            if (done)                          /* restored: never swept (see the table) */
                g_ke_shadow[ke_shadow_find_locked(va)].hdr = 0;
        }
        LeaveCriticalSection(&g_ke_shadow_cs);
        if (!done) {
            if (h) CloseHandle(h);
            xbox_kobj_fail(c, "KE shadow table: out of memory");
        }
        return 1;
    }
    case XKOB_R_TIMER: {
        uint32_t tva = xbox_kobj_get32(r), dpc = xbox_kobj_get32(r);
        long long left = (long long)xbox_kobj_get64(r);
        long period = (long)(int32_t)xbox_kobj_get32(r);
        int done = 0;
        kernel_timer_start_once();
        EnterCriticalSection(&g_timer_lock);
        for (i = 0; i < XBOX_MAX_TIMERS; i++)
            if (!g_timers[i].timer_va) {
                g_timers[i].timer_va = tva;
                g_timers[i].dpc_va = dpc;
                g_timers[i].due_ms = kernel_timer_clock_ms() + left;
                g_timers[i].period_ms = period;
                done = 1;
                break;
            }
        LeaveCriticalSection(&g_timer_lock);
        if (!done)
            xbox_kobj_fail(c, "kernel timer table full");
        return 1;
    }
    case XKOB_R_DPC: {
        uint32_t dpc = xbox_kobj_get32(r), a1 = xbox_kobj_get32(r), a2 = xbox_kobj_get32(r);
        LONG next;
        dpc_lock();
        next = (g_dpc_tail + 1) % XBOX_MAX_PENDING_DPC;
        if (next != g_dpc_head) {
            g_dpc_queue[g_dpc_tail].dpc = dpc;
            g_dpc_queue[g_dpc_tail].arg1 = a1;
            g_dpc_queue[g_dpc_tail].arg2 = a2;
            g_dpc_tail = next;
        }
        LeaveCriticalSection(&g_dpc_lock);
        return 1;
    }
    case XKOB_R_THALERT: {
        uint32_t kt = xbox_kobj_get32(r), id = xbox_kobj_get32(r), waiting = xbox_kobj_get32(r);
        uint8_t a0 = xbox_kobj_get8(r), a1 = xbox_kobj_get8(r);
        HANDLE h = id ? (HANDLE)xbox_kobj_obj(c, id) : NULL;
        ThAlert *e;
        thalert_lock();
        e = thalert_find(kt, 1);
        if (e) {
            e->host = h;
            /* The thread's own APC handle: the same value on POSIX, where a
             * duplicate is the object's one handle with one more reference. */
            e->src = h;
            e->waiting = (int)waiting;
            e->alerted[0] = a0;
            e->alerted[1] = a1;
            h = NULL;
        }
        thalert_unlock();
        if (h) {
            CloseHandle(h);
            xbox_kobj_fail(c, "thread alert table full");
        }
        return 1;
    }
    case XKOB_R_OB: {
        uint32_t body = xbox_kobj_get32(r), base = xbox_kobj_get32(r);
        uint32_t ins = xbox_kobj_get32(r);
        char name[OB_NAME_MAX];
        int done = 0;
        xbox_kobj_getstr(r, name, sizeof name);
        ob_lock();
        for (i = 0; i < OB_MAX; i++)
            if (!s_ob[i].body) {
                s_ob[i].body = body;
                s_ob[i].base = base;
                s_ob[i].inserted = (int)ins;
                memcpy(s_ob[i].name, name, sizeof name);
                done = 1;
                break;
            }
        ob_unlock();
        if (!done)
            xbox_kobj_fail(c, "object table full");
        return 1;
    }
    case XKOB_R_U32TAB:
        kobj_load_u32tab(r);
        return 1;
    default:
        return 0;
    }
}
