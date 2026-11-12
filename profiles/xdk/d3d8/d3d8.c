/*
 * XDK Direct3D 8 layer (profiles/xdk/d3d8): what the console's GPU and D3D's
 * interrupt handler do for D3D, which the recompiled title cannot do itself.
 *
 * D3D is linked into each title, so its device sits at a different address in
 * every XBE. Two ways to know it:
 *
 *   params (title.json "params", xba_params.h), which win when given:
 *     XBA_D3D_DEVICE_PTR   the slot holding the device pointer (D3D's g_pDevice)
 *     XBA_D3D_DEVICE       the device itself (a static in D3D's section)
 *     XBA_D3D_SW_METHODS   the software methods to service, bit n = method n:
 *                          0x20 the fence event (5), 0x7000 RunPushBuffer (0xC-0xE)
 *     XBA_D3D_DMA_WALK     1: walk the pushbuffer as the DMA engine does
 *
 *   or found at run time (XBA-80), when the title gives neither address: once
 *   D3D has made its device, the title's image is scanned for the one dword D
 *   where
 *     [D + 0x24A8] == D                      the pointer slot right after the device
 *     [D + 0x24] < [D + 0x28], both in the contiguous window, 4 KB to 64 MB apart
 *                                            the pushbuffer ring
 *     [D + 0x24] <= [D] <= [D + 0x28]        the put pointer inside it
 *   A unique hit only; it is printed and written to the discovery file
 *   ({"t":"d3d","va":D,"slot":D+0x24A8}) for the next prepare. Found this way,
 *   all the serviceable methods are on unless XBA_D3D_SW_METHODS says otherwise
 *   (5 sets the event only when RECOMP_TITLE_KEVENTS is on, which the title opts
 *   into: it can turn a spin into a wait).
 *
 * The offsets inside the device below were read on XDK 5849 (Burnout 3,
 * D3D8LTCG) and on Fable's D3D: the pointer slot is device + 0x24A8 in both
 * and in the original Burnout 3 build (docs/technical/d3d8ltcg-device-context.md).
 *
 * Switch: XBA_D3D_FENCE=mirror is the A/B fallback Burnout 3 was brought up
 * with (needs XBA_D3D_DEVICE_PTR): no software methods, no semaphore watch, no
 * DMA walk, and D3D's fence mirrored as "GPU always caught up".
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xba_params.h"

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#define SETENV_DEFAULT(k, v) setenv((k), (v), 0)
#else
#define SETENV_DEFAULT(k, v) do { if (!getenv(k)) _putenv_s((k), (v)); } while (0)
#endif

#define D3D_FENCE_COUNT  0x2Cu      /* fences submitted */
#define D3D_RING_START   0x24u      /* the pushbuffer ring, absolute VAs */
#define D3D_RING_END     0x28u
#define D3D_SEMAPHORE    0x30u      /* -> the GPU semaphore (contiguous memory) */
#define D3D_RPB_PENDING  0x50u      /* RunPushBuffer calls waiting for the GPU */
#define D3D_RPB_NODES    0x264u     /* RunPushBuffer's node table, 20 bytes a node */
#define D3D_FENCE_EVENT  0x1DCCu    /* the KEVENT BlockOnTime sleeps on */
#define D3D_SLOT         0x24A8u    /* the device pointer slot, from the device */

#define CONTIG_LO 0x80000000u
#define CONTIG_HI 0x84000000u
#define XBE_BASE  0x00010000u

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void nv2a_pb_set_software_method_hook(int (*hook)(uint32_t param));
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern int  xbox_KeSetGuestEvent(uint32_t guest_va);
extern int  xbox_Nv2aMirrorFence(uint32_t device_ptr_va, uint32_t put_off, uint32_t get_ptr_off);
extern void recomp_discovery_d3d(uint32_t device, uint32_t slot);

#define G32(a) (*(volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + (uint32_t)(a)))

#if defined(XBA_D3D_DEVICE_PTR) || defined(XBA_D3D_DEVICE)
#define D3D_GIVEN 1                 /* the title profile says where it is */
#else
#define D3D_GIVEN 0
#endif

#ifdef XBA_D3D_SW_METHODS
#define D3D_METHODS_DEFAULT (XBA_D3D_SW_METHODS)
#elif D3D_GIVEN
#define D3D_METHODS_DEFAULT 0u
#else
#define D3D_METHODS_DEFAULT 0x7020u /* found at run time: everything serviceable */
#endif

static int d3d_mirror;
static volatile uint32_t s_device;  /* the device, once known (0: not yet) */
static volatile uint32_t s_slot;    /* its pointer slot */
static uint32_t s_methods = D3D_METHODS_DEFAULT;

static int in_contig(uint32_t va) { return va >= CONTIG_LO && va < CONTIG_HI; }

/* ── Finding the device (XBA-80) ─────────────────────────────────────────── */

/* 1 when D looks like D3D's device (see the comment at the top). */
int xba_d3d8_is_device(uint32_t d)
{
    uint32_t s, e, p;
    if (G32(d + D3D_SLOT) != d)
        return 0;
    s = G32(d + D3D_RING_START);
    e = G32(d + D3D_RING_END);
    p = G32(d);
    return in_contig(s) && e <= CONTIG_HI && s < e && e - s >= 0x1000u && e - s <= 0x4000000u
        && p >= s && p <= e;
}

/* The device in [lo, hi): its address when exactly one dword passes, else 0
 * (none yet, or two: then nothing is trusted). */
uint32_t xba_d3d8_scan(uint32_t lo, uint32_t hi)
{
    uint32_t found = 0, d;
    lo = (lo + 3u) & ~3u;
    if (hi < D3D_SLOT + 4u)
        return 0;
    for (d = lo; d + D3D_SLOT + 4u <= hi; d += 4u) {
        if (G32(d + D3D_SLOT) != d || !xba_d3d8_is_device(d))
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

/* Try once; on success remember it, say so and record it. */
int xba_d3d8_locate(void)
{
    uint32_t end = image_end(), d;
    if (s_device || !end)
        return s_device != 0;
    d = xba_d3d8_scan(XBE_BASE, end);
    if (!d)
        return 0;
    s_slot = d + D3D_SLOT;
    s_device = d;
    fprintf(stderr, "[D3D] device found at 0x%08X (pointer slot 0x%08X, ring 0x%08X-0x%08X)\n",
            d, d + D3D_SLOT, G32(d + D3D_RING_START), G32(d + D3D_RING_END));
    recomp_discovery_d3d(d, d + D3D_SLOT);
    return 1;
}

static uint32_t current_device(void)
{
#if defined(XBA_D3D_DEVICE)
    return XBA_D3D_DEVICE;
#elif defined(XBA_D3D_DEVICE_PTR)
    return G32(XBA_D3D_DEVICE_PTR);
#else
    return s_device;
#endif
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
 * A node is { return target | flags, next, slot, fixups, base }. As the
 * handler does, for each node of the chain: *slot = JUMP(target); with flag
 * bit 29, copy each fixup { size, offset, bytes } to base + offset until size
 * 0xFFFFFFFF; next = -1; with bit 31, clear bit 30. Every address written to
 * (the slot, a fixup) is checked to be in the contiguous window, where D3D's
 * pushbuffers live: with a device found at run time the layout is assumed,
 * and a wrong guess must not write into the title's data.
 *
 * Runs on the executor thread, while the main thread may be in RunPushBuffer
 * bumping the same pending count. */
static void d3d_run_pushbuffer_method(uint32_t dev, uint32_t type, uint32_t payload)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t node = type == 0xDu ? dev + D3D_RPB_NODES + payload * 20u : payload | 0x80000000u;
    int guard;

    if (type != 0xEu) {
#if defined(__GNUC__) || defined(__clang__)
        __atomic_fetch_sub((uint32_t *)(mem + dev + D3D_RPB_PENDING), 1u, __ATOMIC_SEQ_CST);
#else
        G32(dev + D3D_RPB_PENDING) -= 1u;
#endif
    }
    for (guard = 0; guard < 4096; guard++) {
        uint32_t target, next, slot;

        target = G32(node);
        slot = G32(node + 8);
        if (!in_contig(slot))
            break;
        G32(slot) = (target & 0x0FFFFFFFu) + 1u;            /* JUMP target */
        if (target & 0x20000000u) {
            uint32_t base = G32(node + 16), fix = G32(node + 12), size;
            while (in_contig(fix) && (size = G32(fix)) != 0xFFFFFFFFu) {
                uint32_t to = base + G32(fix + 4);
                if (size > 0x10000u || !in_contig(to) || !in_contig(to + size - 1u))
                    break;
                memcpy(mem + to, mem + fix + 8, size);
                fix += 8u + size;
            }
        }
        next = G32(node + 4);
        G32(node + 4) = 0xFFFFFFFFu;
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

    if (!(s_methods & (1u << type)) || !(dev = current_device()))
        return 0;
    /* 5: KeSetEvent on the fence event. BlockOnTime patches it into the
     * pushbuffer at the fence it waits for when the GPU is more than 0x2000
     * bytes behind, then waits without a timeout. D3D built that event itself
     * (no KeInitializeEvent import), hence RECOMP_TITLE_KEVENTS; the kernel
     * checks the header reads as an event before it touches it. */
    if (type == 5u) {
        (void)xbox_KeSetGuestEvent(dev + D3D_FENCE_EVENT);
        return 1;
    }
    if (type >= 0xCu && type <= 0xEu) {
        d3d_run_pushbuffer_method(dev, type, param >> 5);
        return 1;
    }
    return 0;
}

void xba_d3d8_configure(void)
{
    const char *fence = getenv("XBA_D3D_FENCE");

    d3d_mirror = fence && !strcmp(fence, "mirror");
#ifdef XBA_D3D_DEVICE_PTR
    s_slot = XBA_D3D_DEVICE_PTR;
#endif
    if (d3d_mirror) {
#ifdef XBA_D3D_DEVICE_PTR
        xbox_Nv2aMirrorFence(XBA_D3D_DEVICE_PTR, D3D_FENCE_COUNT, D3D_SEMAPHORE);
#else
        fprintf(stderr, "[D3D] XBA_D3D_FENCE=mirror needs XBA_D3D_DEVICE_PTR: ignored\n");
#endif
        if (D3D_GIVEN)
            return;
        d3d_mirror = 0;
    }
#if defined(XBA_D3D_DMA_WALK) && XBA_D3D_DMA_WALK
    /* D3D's fence wait reads DMA_GET and the 0x0310 latch (PGRAPH 0x400B10),
     * which only the DMA walk keeps honest. */
    SETENV_DEFAULT("RECOMP_PB_WALK", "dma");
#endif
#if D3D_GIVEN
    if (s_methods & (1u << 5))
        SETENV_DEFAULT("RECOMP_TITLE_KEVENTS", "1");
#endif
    if (s_methods)
        nv2a_pb_set_software_method_hook(d3d_software_method);
}

#ifndef _WIN32
/* Find the device if the profile did not say where it is, then hand the
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
                (void)xba_d3d8_locate();
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

void xba_d3d8_start(const char *game_dir, const char *save_dir)
{
    pthread_t t;
    (void)game_dir; (void)save_dir;
    if (!d3d_mirror && pthread_create(&t, NULL, d3d_watch, NULL) == 0)
        pthread_detach(t);
}
#else
void xba_d3d8_start(const char *game_dir, const char *save_dir) { (void)game_dir; (void)save_dir; }
#endif
