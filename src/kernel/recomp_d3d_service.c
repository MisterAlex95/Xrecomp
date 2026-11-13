/*
 * The XDK D3D service (recomp_d3d_service.h): what the console's GPU and
 * D3D's interrupt handler do for D3D, which the recompiled title cannot do
 * itself. A toolkit default (it was the profile layer profiles/xdk/d3d8/d3d8.c,
 * compiled into each title that layer matched): templates/new-game main()
 * calls recomp_d3d_configure / recomp_d3d_start for every title, and a title
 * without D3D finds no device and stays idle.
 *
 * D3D is linked into each title, so its device sits at a different address in
 * every XBE. It is found at run time (XBA-80): once D3D has made its device,
 * the title's image is scanned for the one dword D where
 *     [D + 0x24A8] == D                      the pointer slot right after the device
 *     [D + 0x24] < [D + 0x28], both in the contiguous window, 4 KB to 64 MB apart
 *                                            the pushbuffer ring
 *     [D + 0x24] <= [D] <= [D + 0x28]        the put pointer inside it
 *   That slot rule is XDK 5849's layout (and Fable's). When nothing passes it,
 *   the slot may sit anywhere (XDK 5455, KOTOR: device - 8), so any dword P of
 *   the image naming a D inside the image is tried with the ring and put tests,
 *   [D + 0x30] (the GPU semaphore pointer) in the contiguous window, and D3D's
 *   two adjacent KEVENTs inside D (below). A unique D only, both ways; it is
 *   printed and written to the discovery file ({"t":"d3d","va":D,"slot":P}).
 *   Found this way, all the serviceable methods are on unless
 *   RECOMP_D3D_SW_METHODS says otherwise.
 *
 * Run-time overrides, for an A/B (they were a title profile's compile-time
 * XBA_D3D_* params; numbers in C notation, 0x... for hex):
 *     RECOMP_D3D_DEVICE_PTR   the slot holding the device pointer (D3D's g_pDevice)
 *     RECOMP_D3D_DEVICE       the device itself (a static in D3D's section)
 *     RECOMP_D3D_SW_METHODS   the software methods to service, bit n = method n:
 *                             0x7000 RunPushBuffer (0xC-0xE); the fence event (5)
 *                             is serviced whatever it says (see D3D_METHODS_FENCE)
 *   With an address given, the device is not searched for and RunPushBuffer's
 *   methods write as D3D's handler does, without the run-time guards.
 *
 * Method 5 sets the fence event, which D3D builds itself right after its
 * vblank event: two KEVENT headers 16 bytes apart, the only such pair in the
 * device (5849: +0x1DBC/+0x1DCC; 5455: +0x199C/+0x19AC). Its offset is read
 * from the device found (recomp_d3d_fence_event_offset); with no pair the
 * table's offset for the build (k_layouts), else 5849's, said. Title-built
 * KEVENTs are guest events by default (kernel_bridge.c, RECOMP_TITLE_KEVENTS),
 * so BlockOnTime's wait blocks until the method comes instead of failing at
 * once and spinning. With RECOMP_TITLE_KEVENTS=0, the one fence event is still
 * adopted under the DMA walk, which delivers method 5 (xbox_KeAdoptGuestEvent).
 *
 * The DMA walk (RECOMP_PB_WALK=dma) is the default once this service is
 * configured: D3D8LTCG's fence wait reads DMA_GET and the 0x0310 latch
 * (PGRAPH 0x400B10), which only the walk keeps honest (Black froze at the
 * menu under the segment scan, Burnout 3 needs it too), and the walk delivers
 * software method 5, which this service answers. RECOMP_PB_WALK=scan keeps the
 * segment scan (the A/B value; KOTOR, D3D8 5455, ran 5 of 5 runs either way,
 * K-7). A project that does not call recomp_d3d_configure keeps the segment
 * scan (nv2a_pb_scan.c): nothing would answer the software methods the walk
 * makes D3D ask for.
 *
 * The other offsets inside the device were read on XDK 5849 (Burnout 3 and
 * Fable, both D3D8LTCG 5849) and are the same on XDK 5455 (KOTOR): ring
 * +0x24/+0x28, fence count +0x2C, semaphore pointer +0x30. The pointer slot
 * is device + 0x24A8 on 5849 (docs/technical/d3d8ltcg-device-context.md).
 *
 * Every offset is either read from the device's structure (the device
 * itself, its slot, its fence event) or looked up in k_layouts below, keyed by
 * the title's D3D library and build as its XBE header says (XBA-105). A build
 * the table does not know takes the offsets of the closest known layout for
 * each rule that cannot be read from the structure, and says so once per rule:
 *   [D3D] unknown XDK build N (D3D8LTCG): fence event fell back to +0x1DCC (D3D8LTCG 5849's)
 * with a {"t":"d3d_layout",...} line in the discovery file, so a run on a new
 * build tells what to add to the table. Run-time guards stay on such
 * fallbacks: a found device's RunPushBuffer methods write only into the
 * pushbuffer the node names (its return slot in the contiguous window, its
 * fixups inside [base, slot)), and decrement a pending count only when it
 * reads as one.
 *
 * Switches: RECOMP_D3D_SERVICE=0 leaves the service out (no software methods,
 * no semaphore watch, no DMA-walk default: the runtime as a project that never
 * calls it). RECOMP_D3D_FENCE=mirror (the old name XBA_D3D_FENCE is still
 * read) is the A/B fallback Burnout 3 was brought up with; it needs
 * RECOMP_D3D_DEVICE_PTR: no software methods, no semaphore watch, the segment
 * scan, and D3D's fence mirrored as "GPU always caught up".
 */
#include "recomp_d3d_service.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#define SETENV_DEFAULT(k, v) setenv((k), (v), 0)
#else
#define SETENV_DEFAULT(k, v) do { if (!getenv(k)) _putenv_s((k), (v)); } while (0)
#endif

/* ── D3D's device layouts, by library and build (XBA-105) ──────────────────
 *
 * What cannot be read from the device's structure. 0 in a field: not known
 * for that build (the fallback's value is used, and said). slot: the device
 * pointer slot relative to the device, 0 when it is found by any pointer
 * (recomp_d3d_scan_slots). Ring, put, fence count and semaphore are the same in
 * every layout seen; they are what finds the device, so a build where they
 * moved would not be found at all (RECOMP_D3D_DEVICE_PTR then says where). */
typedef struct {
    const char *lib;            /* the XBE library name */
    unsigned build;
    int32_t  slot;
    uint32_t ring_start, ring_end;      /* the pushbuffer ring, absolute VAs */
    uint32_t fence_count;               /* fences submitted */
    uint32_t semaphore;                 /* -> the GPU semaphore (contiguous memory) */
    uint32_t fence_event;               /* the KEVENT BlockOnTime sleeps on (the pair read from the device wins) */
    uint32_t rpb_pending;               /* RunPushBuffer calls waiting for the GPU */
    uint32_t rpb_nodes;                 /* RunPushBuffer's node table, 20 bytes a node */
    const char *seen;           /* how a message names it */
} D3dLayout;

/* Seen on: D3D8LTCG 5849 Burnout 3, Black, Fable; D3D8 5455 KOTOR. */
static const D3dLayout k_layouts[] = {
    { "D3D8LTCG", 5849, 0x24A8, 0x24, 0x28, 0x2C, 0x30, 0x1DCC, 0x50, 0x264, "D3D8LTCG 5849's" },
    { "D3D8",     5455, -8,     0x24, 0x28, 0x2C, 0x30, 0x19AC, 0,    0,     "D3D8 5455's" },
};
/* What a rule falls back to: D3D8LTCG 5849's (Fable's D3D is D3D8LTCG 5849:
 * its runs print "[D3D] D3D8LTCG build 5849", and the RunPushBuffer offsets
 * were read on it). */
static const D3dLayout k_fallback =
    { "D3D8LTCG", 5849, 0x24A8, 0x24, 0x28, 0x2C, 0x30, 0x1DCC, 0x50, 0x264, "D3D8LTCG 5849's" };

#define D3D_FENCE_COUNT  (layout()->fence_count)
#define D3D_RING_START   (layout()->ring_start)
#define D3D_RING_END     (layout()->ring_end)
#define D3D_SEMAPHORE    (layout()->semaphore)
#define D3D_SLOT_5849    0x24A8u    /* the slot rule tried first on any build */

#define CONTIG_LO 0x80000000u
#define CONTIG_HI 0x84000000u
#define XBE_BASE  0x00010000u

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern size_t xbox_GetMappedSize(void);
extern void *xbox_GetMemoryBase(void);
extern void nv2a_pb_set_software_method_hook(int (*hook)(uint32_t param));
extern void nv2a_pb_set_software_method_refusals(uint32_t (*count)(void));
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern int  xbox_KeSetGuestEvent(uint32_t guest_va);
extern int  xbox_KeAdoptGuestEvent(uint32_t guest_va);
extern int  nv2a_pb_dma_walk(void);
extern int  xbox_Nv2aMirrorFence(uint32_t device_ptr_va, uint32_t put_off, uint32_t get_ptr_off);
extern void recomp_discovery_d3d(uint32_t device, uint32_t slot);
extern void recomp_discovery_d3d_layout(const char *lib, uint32_t build, const char *rule, uint32_t used);
extern void nv2a_pb_exec_insert_callback_args(uint32_t *routine, uint32_t *context);
extern int  xbox_DpcQueueGuestCall(uint32_t routine, uint32_t context);

#define G32(a) (*(volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + (uint32_t)(a)))

/* RECOMP_D3D_DEVICE / RECOMP_D3D_DEVICE_PTR (read by recomp_d3d_configure):
 * where the device is, when the environment says so. */
static uint32_t s_given_device, s_given_ptr;
#define D3D_GIVEN (s_given_device != 0 || s_given_ptr != 0)

/* 8 and 9 (XBA-94): D3D's handler pokes PGRAPH through its RDI (RAM data
 * interface) window, 0x400750 / 0x400754 -- internal GPU state no part of this
 * runtime reads, since the executor keeps its own. On the console the GPU
 * stops until the handler has run and then goes on; here going on is all
 * there is to do, so they are acknowledged (counted as serviced) whatever
 * RECOMP_D3D_SW_METHODS says. The first few payloads are printed, to document them per XDK
 * build (docs/technical/pushbuffer-executor.md). */
#define D3D_METHODS_ACK 0x0300u

/* 6 and 7 (XBA-94): D3DDevice_InsertCallback. The handler calls the routine
 * the title gave, with its context, at DPC level; the two come in the clear
 * values set just before (nv2a_pb_exec_insert_callback_args). Queued on the
 * DPC path (xbox_DpcQueueGuestCall), which refuses a routine that is not
 * code. For every title, device or not. RECOMP_D3D_CALLBACKS=0 leaves them
 * unserviced, as before. */
#define D3D_METHODS_CALLBACK 0x00C0u

/* 5: the fence event, whenever the device is known, whatever
 * RECOMP_D3D_SW_METHODS says. Title-built KEVENTs are guest events by default
 * (kernel_bridge.c), so D3D's fence wait really blocks on that event, and this
 * method is the only thing that sets it: Fable (methods 0x7000) froze in
 * Oakvale with its main thread blocked in sub_00855260's KeWaitForSingleObject
 * right after one unserviced method 5. Its offset is read from the device. */
#define D3D_METHODS_FENCE 0x0020u

/* Found at run time: every method serviced (recomp_d3d_configure). */
#define D3D_METHODS_ALL (0x7020u | D3D_METHODS_ACK | D3D_METHODS_CALLBACK)

static int d3d_mirror;
static int d3d_off;                 /* RECOMP_D3D_SERVICE=0 */
static volatile uint32_t s_device;  /* the device, once known (0: not yet) */
static volatile uint32_t s_slot;    /* its pointer slot */
static uint32_t s_methods = D3D_METHODS_ALL;
static volatile uint32_t s_fence_event;  /* offset in the device; read at run time */
static volatile int s_fence_known;  /* s_fence_event read from the device (locate, or the first method 5) */

static int in_contig(uint32_t va) { return va >= CONTIG_LO && va < CONTIG_HI; }

/* The title's D3D library and build, from the XBE header the kernel maps at
 * 0x10000 (its library table), and the layout for them. Read on first use:
 * recomp_d3d_configure runs before guest memory is mapped. */
static char s_lib[9];
static unsigned s_build;
static const D3dLayout *s_layout;   /* the table's entry, or NULL: unknown build */
static int s_layout_read;

static void read_library(void)
{
    uint32_t n, va, i;
    int found = 0;
    if (s_layout_read || !xbox_GetMemoryBase())  /* not mapped yet (configure runs before) */
        return;
    if (G32(0x10000u) != 0x48454258u)           /* "XBEH": the header is not mapped yet */
        return;
    s_layout_read = 1;
    n = G32(0x10000u + 0x160u);
    va = G32(0x10000u + 0x164u);
    if (n > 64 || va < 0x10000u || va >= 0x20000u)
        n = 0;
    for (i = 0; i < n; i++) {
        char name[9];
        uint32_t e = va + i * 16u, w0 = G32(e), w1 = G32(e + 4);
        memcpy(name, &w0, 4);
        memcpy(name + 4, &w1, 4);
        name[8] = 0;
        /* D3D8LTCG first: a title links one of the two. */
        if (!strcmp(name, "D3D8LTCG") || (!found && !strcmp(name, "D3D8"))) {
            memcpy(s_lib, name, sizeof s_lib);
            s_build = G32(e + 12) & 0xFFFFu;
            found = 1;
        }
    }
    for (i = 0; found && i < sizeof k_layouts / sizeof k_layouts[0]; i++)
        if (!strcmp(k_layouts[i].lib, s_lib) && k_layouts[i].build == s_build)
            s_layout = &k_layouts[i];
    if (found)
        fprintf(stderr, "[D3D] %s build %u: %s\n", s_lib, s_build,
                s_layout ? "a known device layout" : "not in the layout table (XBA-105): fallbacks are said");
}

static const D3dLayout *layout(void)
{
    read_library();
    return s_layout ? s_layout : &k_fallback;
}

/* A rule the table cannot answer for this build: the fallback's value, said
 * and recorded once per rule. Silent for a known build (the table answered)
 * and before the library is read. Returns value. */
static uint32_t fell_back(const char *rule, const char *what, uint32_t value)
{
    static const char *said[8];
    unsigned i;
    read_library();
    if (s_layout || !s_layout_read)
        return value;
    for (i = 0; i < 8 && said[i]; i++)
        if (!strcmp(said[i], rule))
            return value;
    if (i < 8)
        said[i] = rule;
    fprintf(stderr, "[D3D] unknown XDK build %u (%s): %s fell back to +0x%X (%s)\n",
            s_build, s_lib[0] ? s_lib : "no D3D library", what, value, k_fallback.seen);
    recomp_discovery_d3d_layout(s_lib[0] ? s_lib : "?", s_build, rule, value);
    return value;
}

/* The device pointer slot rule for this build: the table's offset when it
 * names one after the device, else 5849's (tried first on any build). */
static uint32_t slot_offset(void)
{
    const D3dLayout *l = layout();
    return l->slot > 0 ? (uint32_t)l->slot : D3D_SLOT_5849;
}

/* ── Finding the device (XBA-80) ─────────────────────────────────────────── */

/* 1 when D looks like D3D's device (see the comment at the top). */
int recomp_d3d_is_device(uint32_t d)
{
    uint32_t s, e, p;
    if (G32(d + slot_offset()) != d)
        return 0;
    s = G32(d + D3D_RING_START);
    e = G32(d + D3D_RING_END);
    p = G32(d);
    return in_contig(s) && e <= CONTIG_HI && s < e && e - s >= 0x1000u && e - s <= 0x4000000u
        && p >= s && p <= e;
}

/* The device in [lo, hi): its address when exactly one dword passes, else 0
 * (none yet, or two: then nothing is trusted). */
uint32_t recomp_d3d_scan(uint32_t lo, uint32_t hi)
{
    uint32_t found = 0, d, so = slot_offset();
    lo = (lo + 3u) & ~3u;
    for (d = lo; d < hi && hi - d >= so + 4u; d += 4u) {
        if (G32(d + so) != d || !recomp_d3d_is_device(d))
            continue;
        if (found)
            return 0;
        found = d;
    }
    return found;
}

/* The title's image, from the XBE header the kernel maps at 0x10000. */
static uint32_t image_end(void)
{
    uint32_t size = G32(XBE_BASE + 0x10Cu);     /* SizeOfImage */
    return size > 0x1000u && size < 0x04000000u ? XBE_BASE + size : 0;
}

/* A KEVENT header D3D wrote itself: Type 0 (notification) or 1
 * (synchronization), Size 4 dwords, an empty wait list (Flink == Blink == the
 * list head at +8). */
static int is_kevent(uint32_t va)
{
    uint32_t hdr = G32(va);
    if (((hdr & 0xFFu) != 0 && (hdr & 0xFFu) != 1) || ((hdr >> 16) & 0xFFu) != 4)
        return 0;
    return G32(va + 8) == va + 8 && G32(va + 12) == va + 8;
}

#define D3D_EVENTS_LO 0x400u        /* where the event pair is looked for in the device */
#define D3D_EVENTS_HI 0x4000u

/* The device's two events sit side by side (vblank, then fence), 16 bytes
 * each: the offset of the second when exactly one such pair lies in
 * [0x400, 0x4000), else 0. */
uint32_t recomp_d3d_fence_event_offset(uint32_t d)
{
    uint32_t off, found = 0;
    if (d > 0xFFFFFFFFu - D3D_EVENTS_HI - 32u)
        return 0;
    for (off = D3D_EVENTS_LO; off < D3D_EVENTS_HI; off += 4u) {
        if (is_kevent(d + off) && is_kevent(d + off + 16u)) {
            if (found)
                return 0;
            found = off + 16u;
        }
    }
    return found;
}

/* D without the slot rule: the ring, the put pointer inside it, the GPU
 * semaphore pointer in the contiguous window. */
static int is_device_core(uint32_t d)
{
    uint32_t s = G32(d + D3D_RING_START), e = G32(d + D3D_RING_END), p = G32(d);
    return in_contig(s) && e <= CONTIG_HI && s < e && e - s >= 0x1000u && e - s <= 0x4000000u
        && p >= s && p <= e && in_contig(G32(d + D3D_SEMAPHORE));
}

/* The device in [lo, hi) through any dword P of it that names D, D inside
 * [lo, hi) with the whole event window before hi (XDK 5455: P = D - 8). The
 * device when exactly one D passes, its first P in *slot; else 0. */
uint32_t recomp_d3d_scan_slots(uint32_t lo, uint32_t hi, uint32_t *slot)
{
    uint32_t found = 0, first = 0, p, d;
    lo = (lo + 3u) & ~3u;
    if (hi < lo + 4u)
        return 0;
    for (p = lo; p <= hi - 4u; p += 4u) {
        d = G32(p);
        /* hi - d, not d + window: d may be anywhere up to 0xFFFFFFFF */
        if (d < lo || d >= hi || (d & 3u) || hi - d < D3D_EVENTS_HI + 32u || d == found)
            continue;
        if (!is_device_core(d) || !recomp_d3d_fence_event_offset(d))
            continue;
        if (found)
            return 0;
        found = d;
        first = p;
    }
    if (found && slot)
        *slot = first;
    return found;
}

/* The fence event's offset in device d: the KEVENT pair read from it, else
 * the table's for this build, else the fallback's (said). *read: 1 when it
 * came from the device. */
static uint32_t fence_event_of(uint32_t d, int *read)
{
    uint32_t ev = recomp_d3d_fence_event_offset(d);
    *read = ev != 0;
    if (ev)
        return ev;
    if (layout() != &k_fallback)
        return layout()->fence_event;
    return fell_back("fence_event", "fence event (no KEVENT pair in the device)", k_fallback.fence_event);
}

/* Try once; on success remember it, say so and record it. */
int recomp_d3d_locate(void)
{
    uint32_t end = image_end(), d, slot = 0, ev;
    int read;
    if (s_device || !end)
        return s_device != 0;
    if ((d = recomp_d3d_scan(XBE_BASE, end)) != 0) {
        slot = d + slot_offset();
        /* Found by a slot offset the table did not give for this build. */
        (void)fell_back("slot", "device pointer slot", slot_offset());
    } else if (!(d = recomp_d3d_scan_slots(XBE_BASE, end, &slot))) {
        return 0;
    }
    /* The ring, put and semaphore offsets are what found it; on a build the
     * table does not know they are the known layouts' (said once). */
    (void)fell_back("device_core", "ring/put/semaphore offsets (+0x24/+0x28/+0/+0x30)", D3D_RING_START);
    ev = fence_event_of(d, &read);
    s_fence_event = ev;
    s_fence_known = 1;
    s_slot = slot;
    s_device = d;
    fprintf(stderr, "[D3D] device found at 0x%08X (pointer slot 0x%08X, ring 0x%08X-0x%08X, fence event +0x%X%s)\n",
            d, slot, G32(d + D3D_RING_START), G32(d + D3D_RING_END), s_fence_event, read ? "" : " assumed");
    recomp_discovery_d3d(d, slot);
    /* The fence event, when method 5 will set it (see the top). */
    if ((s_methods & (1u << 5)) && nv2a_pb_dma_walk() && xbox_KeAdoptGuestEvent(d + s_fence_event))
        fprintf(stderr, "[D3D] fence event 0x%08X: set by software method 5 under the DMA walk\n",
                d + s_fence_event);
    return 1;
}

static uint32_t current_device(void)
{
    if (s_given_device)
        return s_given_device;
    if (s_given_ptr)
        return G32(s_given_ptr);
    return s_device;
}

/* ── RunPushBuffer's deferred return JUMP (software methods 0xC, 0xD, 0xE) ──
 *
 * D3DDevice_RunPushBuffer JUMPs from the ring into a recorded pushbuffer whose
 * last word is a JUMP back. If the GPU has not yet passed the pushbuffer's
 * previous use, that return JUMP cannot be patched yet: D3D puts a software
 * method before the entering JUMP and lets its interrupt handler patch it when
 * the GPU gets there. Unserviced, the walk follows the old return target and
 * runs earlier commands again (Fable: the visibility reports it waits on
 * in-world never come).
 *
 *   0xC  node = payload | 0x80000000 (in the ring), one pending run fewer
 *   0xD  node = device + 0x264 + payload * 20 (D3D's table), one pending run fewer
 *   0xE  node = payload | 0x80000000, recording: the count is left alone
 *
 * A node is { return target | flags, next, slot, fixups, base }: slot is
 * the pushbuffer's last word (its return JUMP, base + size - 4 by
 * construction in RunPushBuffer), base its first, fixups the caller's
 * D3DFixup data. As the handler does, for each node of the chain:
 * *slot = JUMP(target); with flag bit 29, copy each fixup { size, offset,
 * bytes } to base + offset until size 0xFFFFFFFF; next = -1; with bit 31,
 * clear bit 30.
 *
 * With a device found at run time the layout is assumed, so each write is
 * checked against the structure the node gives, not against where D3D's
 * callers keep their data:
 *   - the slot must be in the contiguous window (D3D's pushbuffers live there);
 *   - the fixup list may be anywhere in mapped guest memory (D3DFixup buffers
 *     are on the title's heap: Fable's at 0x0129xxxx, which a contiguous-only
 *     rule dropped, every deferred list, silently), each entry header and
 *     payload inside it, an entry at most 64 KB, the list at most 1 MB;
 *   - each destination must lie inside the node's own pushbuffer:
 *     base <= to, to + size <= slot (the return JUMP is not a fixup's), no wrap.
 * A refused list stops at the entry refused (the entries before it stay
 * applied, the return JUMP is patched anyway); refusals are printed (the
 * first 4) and counted for the [GPU] software methods report line, so a
 * dropped list is never silent. With the addresses given
 * (RECOMP_D3D_DEVICE[_PTR]) it writes as D3D's handler does, unchecked.
 *
 * Runs on the executor thread, while the main thread may be in RunPushBuffer
 * bumping the same pending count and linking a new node to the chain with a
 * compare-exchange on the last node's next (0 -> node): next is taken with an
 * atomic exchange, so a node linked meanwhile is not overwritten with -1. */
/* A write the run-time guards refused (device found at run time only: with
 * the addresses given the handler writes as D3D's does). */
static void skip_note(const char *what, uint32_t va)
{
    static int shown;
    if (shown++ < 4)
        fprintf(stderr, "[D3D] RunPushBuffer method: %s 0x%08X outside the contiguous window, not written\n",
                what, va);
}

/* Fixup lists refused (a found device only), for the report. */
static volatile uint32_t s_fixups_refused;

uint32_t recomp_d3d_fixups_refused(void)
{
    return s_fixups_refused;
}

#define FIXUP_ENTRY_MAX 0x10000u    /* bytes in one entry */
#define FIXUP_LIST_MAX  0x100000u   /* bytes walked in one list */

/* [va, va + n) inside mapped guest memory: the low RAM above the first 64 KB
 * (xbox_GetMappedSize, 64 MB if not known yet) or the contiguous window. */
static int guest_readable(uint32_t va, uint32_t n)
{
    size_t mapped = xbox_GetMappedSize();
    uint64_t lo_end = mapped ? (uint64_t)mapped : 0x04000000u, end = (uint64_t)va + n;
    if (lo_end > CONTIG_LO)
        lo_end = CONTIG_LO;
    return (va >= 0x10000u && end <= lo_end) || (va >= CONTIG_LO && end <= CONTIG_HI);
}

static void fixup_refused(uint32_t node, uint32_t fix, uint32_t base, uint32_t slot, uint32_t to,
                          uint32_t size, const char *why)
{
    static int shown;
    s_fixups_refused++;     /* executor thread only */
    if (shown++ < 4)
        fprintf(stderr, "[D3D] RunPushBuffer method: fixup list of node 0x%08X refused at 0x%08X (%s: size %u "
                "to 0x%08X, pushbuffer 0x%08X-0x%08X), rest of the list not written\n",
                node, fix, why, size, to, base, slot);
}

/* A node's fixup list for a device found at run time (see above). */
static void apply_fixups_checked(uint8_t *mem, uint32_t node, uint32_t slot)
{
    uint32_t base = G32(node + 16), fix = G32(node + 12), walked = 0;

    if (!in_contig(base) || base >= slot) {
        fixup_refused(node, fix, base, slot, base, 0, "pushbuffer base not below its slot");
        return;
    }
    for (;;) {
        uint32_t size, to;
        if (!guest_readable(fix, 4)) {
            fixup_refused(node, fix, base, slot, 0, 0, "list not in guest memory");
            return;
        }
        size = G32(fix);
        if (size == 0xFFFFFFFFu)
            return;
        if (!guest_readable(fix, 8)) {
            fixup_refused(node, fix, base, slot, 0, size, "entry not in guest memory");
            return;
        }
        to = base + G32(fix + 4);
        if (size > FIXUP_ENTRY_MAX) {
            fixup_refused(node, fix, base, slot, to, size, "entry larger than 64 KB");
            return;
        }
        if (walked + 8u + size > FIXUP_LIST_MAX) {
            fixup_refused(node, fix, base, slot, to, size, "no end within 1 MB");
            return;
        }
        if (!guest_readable(fix + 8u, size)) {
            fixup_refused(node, fix, base, slot, to, size, "payload not in guest memory");
            return;
        }
        if (to < base || (uint64_t)to + size > slot) {
            fixup_refused(node, fix, base, slot, to, size, "destination outside the pushbuffer");
            return;
        }
        memcpy(mem + to, mem + fix + 8, size);
        fix += 8u + size;
        walked += 8u + size;
    }
}

/* RunPushBuffer's two device offsets for this build: the table's, else the
 * fallback's (Fable's D3D), said once each. */
static uint32_t rpb_offset(int nodes)
{
    const D3dLayout *l = layout();
    uint32_t v = nodes ? l->rpb_nodes : l->rpb_pending;
    if (v && l != &k_fallback)
        return v;
    if (l != &k_fallback) {             /* a known build without them: the fallback's, said */
        static int said[2];
        v = nodes ? k_fallback.rpb_nodes : k_fallback.rpb_pending;
        if (!said[nodes]++) {
            fprintf(stderr, "[D3D] %s build %u: RunPushBuffer %s not in the layout table, +0x%X (%s) used\n",
                    s_lib, s_build, nodes ? "node table" : "pending count", v, k_fallback.seen);
            recomp_discovery_d3d_layout(s_lib, s_build, nodes ? "rpb_nodes" : "rpb_pending", v);
        }
        return v;
    }
    return nodes ? fell_back("rpb_nodes", "RunPushBuffer node table", k_fallback.rpb_nodes)
                 : fell_back("rpb_pending", "RunPushBuffer pending count", k_fallback.rpb_pending);
}

/* Offsets the table did not confirm for this build (a found device only):
 * the pending count is decremented only when it reads as a small count. */
static int rpb_confirmed(void)
{
    const D3dLayout *l = layout();
    return D3D_GIVEN || (l != &k_fallback && l->rpb_pending && l->rpb_nodes);
}

static void d3d_run_pushbuffer_method(uint32_t dev, uint32_t type, uint32_t payload)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t pending = dev + rpb_offset(0);
    uint32_t node = type == 0xDu ? dev + rpb_offset(1) + payload * 20u : payload | 0x80000000u;
    int guard;

    if (type != 0xEu) {
        uint32_t n = G32(pending);
        if (!rpb_confirmed() && (n == 0 || n > 0x1000u)) {
            static int shown;
            if (shown++ < 4)
                fprintf(stderr, "[D3D] RunPushBuffer method: pending count +0x%X reads %u, not a count: "
                        "left alone\n", pending - dev, n);
        } else {
#if defined(__GNUC__) || defined(__clang__)
            __atomic_fetch_sub((uint32_t *)(mem + pending), 1u, __ATOMIC_SEQ_CST);
#else
            G32(pending) -= 1u;
#endif
        }
    }
    for (guard = 0; guard < 4096; guard++) {
        uint32_t target, next, slot;

        target = G32(node);
        slot = G32(node + 8);
        if (!D3D_GIVEN && !in_contig(slot)) {
            skip_note("slot", slot);
            break;
        }
        G32(slot) = (target & 0x0FFFFFFFu) + 1u;            /* JUMP target */
        if ((target & 0x20000000u) && !D3D_GIVEN) {
            apply_fixups_checked(mem, node, slot);
        } else if (target & 0x20000000u) {                  /* given: as D3D's handler, unchecked */
            uint32_t base = G32(node + 16), fix = G32(node + 12), size;
            while ((size = G32(fix)) != 0xFFFFFFFFu) {
                uint32_t to = base + G32(fix + 4);         /* wraps in the guest's 32 bits */
                memcpy(mem + to, mem + fix + 8, size);
                fix += 8u + size;
            }
        }
#if defined(__GNUC__) || defined(__clang__)
        next = __atomic_exchange_n((uint32_t *)(mem + node + 4), 0xFFFFFFFFu, __ATOMIC_SEQ_CST);
#elif defined(_MSC_VER)
        next = (uint32_t)_InterlockedExchange((volatile long *)(mem + node + 4), -1L);
#else
        next = G32(node + 4);
        G32(node + 4) = 0xFFFFFFFFu;
#endif
        if (target & 0x80000000u)
            G32(node) = target & 0xBFFFFFFFu;
        if (next == 0 || next == 0xFFFFFFFFu)
            break;
        node = next;
    }
}

/* The software methods (NO_OPERATION with a value) the console's PGRAPH
 * interrupt handler answers; 1 when serviced. */
static int d3d_software_method(uint32_t param)
{
    uint32_t type = param & 0x1Fu, dev;

    if (!(s_methods & (1u << type)))
        return 0;
    if (D3D_METHODS_ACK & (1u << type)) {
        static int shown;
        if (shown < 8) {
            shown++;
            fprintf(stderr, "[D3D] software method %u (PGRAPH RDI write), payload 0x%07X: acknowledged\n",
                    type, param >> 5);
        }
        return 1;
    }
    if (D3D_METHODS_CALLBACK & (1u << type)) {
        uint32_t routine, context;
        static int shown;
        nv2a_pb_exec_insert_callback_args(&routine, &context);
        if (!xbox_DpcQueueGuestCall(routine, context))
            return 0;                   /* not code, or the pool busy: counted unserviced */
        if (shown < 4) {
            shown++;
            fprintf(stderr, "[D3D] software method %u (InsertCallback): 0x%08X(0x%08X) queued at DPC level\n",
                    type, routine, context);
        }
        return 1;
    }
    if (!(dev = current_device()))
        return 0;
    /* 5: KeSetEvent on the fence event. BlockOnTime patches it into the
     * pushbuffer at the fence it waits for when the GPU is more than 0x2000
     * bytes behind, then waits without a timeout. D3D built that event itself
     * (no KeInitializeEvent import): a guest event by default (title-built
     * KEVENTs, kernel_bridge.c), or adopted alone under the DMA walk when the
     * device was found at run time (locate); the kernel checks the header
     * reads as an event before it touches it. Its offset is read from the
     * device (locate, or here at the first method 5 with the addresses
     * given), 5849's when the device holds no event pair. */
    if (type == 5u) {
        if (!s_fence_known) {           /* a given device: read the offset once */
            int read;
            s_fence_event = fence_event_of(dev, &read);
            s_fence_known = 1;
            fprintf(stderr, "[D3D] fence event 0x%08X (+0x%X%s): set by software method 5\n",
                    dev + s_fence_event, s_fence_event, read ? "" : ", assumed");
        }
        (void)xbox_KeSetGuestEvent(dev + s_fence_event);
        return 1;
    }
    if (type >= 0xCu && type <= 0xEu) {
        d3d_run_pushbuffer_method(dev, type, param >> 5);
        return 1;
    }
    return 0;
}

/* A number from the environment (C notation), 0 when unset or not one. */
static uint32_t env_number(const char *name)
{
    const char *v = getenv(name);
    char *end;
    unsigned long n;
    if (!v || !*v)
        return 0;
    n = strtoul(v, &end, 0);
    if (*end) {
        fprintf(stderr, "[D3D] %s=%s is not a number: ignored\n", name, v);
        return 0;
    }
    return (uint32_t)n;
}

int recomp_d3d_configure(void)
{
    const char *fence = getenv("RECOMP_D3D_FENCE");
    const char *off = getenv("RECOMP_D3D_SERVICE");
    const char *cb = getenv("RECOMP_D3D_CALLBACKS");

    d3d_off = off && *off == '0';
    if (d3d_off)
        return 0;
    if (!fence || !*fence)
        fence = getenv("XBA_D3D_FENCE");        /* its name as a profile switch */
    d3d_mirror = fence && !strcmp(fence, "mirror");
    s_given_device = env_number("RECOMP_D3D_DEVICE");
    s_given_ptr = env_number("RECOMP_D3D_DEVICE_PTR");
    if (getenv("RECOMP_D3D_SW_METHODS") && *getenv("RECOMP_D3D_SW_METHODS"))
        s_methods = env_number("RECOMP_D3D_SW_METHODS") | D3D_METHODS_FENCE | D3D_METHODS_ACK | D3D_METHODS_CALLBACK;
    else if (D3D_GIVEN)
        s_methods = D3D_METHODS_FENCE | D3D_METHODS_ACK | D3D_METHODS_CALLBACK;
    else
        s_methods = D3D_METHODS_ALL;
    if (cb && *cb == '0')
        s_methods &= ~D3D_METHODS_CALLBACK;
    s_slot = s_given_ptr;
    if (d3d_mirror) {
        if (s_given_ptr) {
            xbox_Nv2aMirrorFence(s_given_ptr, D3D_FENCE_COUNT, D3D_SEMAPHORE);
            return 1;                   /* the segment scan, no hook, no watch */
        }
        fprintf(stderr, "[D3D] RECOMP_D3D_FENCE=mirror needs RECOMP_D3D_DEVICE_PTR: ignored\n");
        d3d_mirror = 0;
    }
    /* D3D's fence wait reads DMA_GET and the 0x0310 latch (PGRAPH 0x400B10),
     * which only the DMA walk keeps honest; and the walk delivers the software
     * methods the hook below answers. RECOMP_PB_WALK=scan: the segment scan. */
    SETENV_DEFAULT("RECOMP_PB_WALK", "dma");
    if (s_methods) {
        nv2a_pb_set_software_method_hook(d3d_software_method);
        nv2a_pb_set_software_method_refusals(recomp_d3d_fixups_refused);
    }
    return 1;
}

#ifndef _WIN32
/* Find the device if the environment did not say where it is, then hand the
 * executor D3D's GPU semaphore (device + 0x30) once D3D has allocated it, and
 * again if a device reset moves it. The slot does not always hold the device
 * (Fable), so only a semaphore in contiguous memory is accepted: handing over a
 * heap address made the executor write into game data. */
static void *d3d_watch(void *unused)
{
    uint32_t last = 0;
    unsigned tries = 0;
    (void)unused;
    for (;;) {
        uint32_t dev, sem;
        if (!s_slot) {
            if (!(++tries % 250u))              /* every 250 ms until found */
                (void)recomp_d3d_locate();
            usleep(1000);
            continue;
        }
        dev = G32(s_slot);
        sem = dev ? G32(dev + D3D_SEMAPHORE) : 0;
        if (sem && sem != last && in_contig(sem)) {
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] device 0x%08X, GPU semaphore at 0x%08X\n", dev, sem);
            last = sem;
        }
        usleep(1000);
    }
    return NULL;
}

void recomp_d3d_start(const char *game_dir, const char *save_dir)
{
    pthread_t t;
    (void)game_dir; (void)save_dir;
    if (!d3d_off && !d3d_mirror && pthread_create(&t, NULL, d3d_watch, NULL) == 0)
        pthread_detach(t);
}
#else
void recomp_d3d_start(const char *game_dir, const char *save_dir) { (void)game_dir; (void)save_dir; }
#endif
